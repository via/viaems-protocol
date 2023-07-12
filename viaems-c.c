#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

#include "cbor.h"
#include "viaems-c.h"


typedef enum {
  STRUCTURE,
  GET,
  SET,
} request_type;

struct request {
  bool active;
  uint32_t id;
  request_type type;
  struct structure_node *node;
  struct config_value value;
  void *userdata;
  union {
    structure_callback structure_cb;
    get_callback get_callback;
  };
};

#define MAX_KEYS 64
struct protocol {
  size_t n_feed_fields;
  struct field_key field_keys[MAX_KEYS];
  feed_callback feed_cb;

  write_fn write;
  void *write_userdata;

  mtx_t request_client_mtx; /* Used to lock all api client access to commands */
  mtx_t request_mtx; /* Used to block access to request structure */
  cnd_t request_wakeup_cnd;
  struct request request;
};

static void check_thrd(int val) {
  assert(val == thrd_success);
}

bool viaems_create_protocol(struct protocol **dest) {
  assert(dest);
  *dest = (struct protocol *)malloc(sizeof(struct protocol));
  if (!*dest) {
    return false;
  }

  memset(*dest, 0, sizeof(struct protocol));
  mtx_init(&(*dest)->request_mtx, mtx_plain);
  mtx_init(&(*dest)->request_client_mtx, mtx_plain);
  cnd_init(&(*dest)->request_wakeup_cnd);
  return true;
}

void viaems_destroy_protocol(struct protocol **proto) {

  for (int i = 0; i < (*proto)->n_feed_fields; i++) {
    free((*proto)->field_keys[i].name);
  }
  free(*proto);
  *proto = NULL;
}

void viaems_set_feed_cb(struct protocol *p, feed_callback cb) {
  p->feed_cb = cb;
}

void viaems_set_write_fn(struct protocol *p, write_fn wfn, void *ud) {
  p->write_userdata = ud;
  p->write = wfn;
}

static void handle_desc_message(struct protocol *p, CborValue *msg) {
  CborValue keys;
  if (cbor_value_map_find_value(msg, "keys", &keys) != CborNoError) {
    /* TODO: handle */
    return;
  }

  if (!cbor_value_is_array(&keys)) {
    return;
  }

  CborValue i;
  size_t n_keys = 0;
  cbor_value_enter_container(&keys, &i);
  while(!cbor_value_at_end(&i)) {
    if (n_keys >= MAX_KEYS) {
      return;
    }
    if (!cbor_value_is_text_string(&i)) {
      return;
    }

    struct field_key *k = &p->field_keys[n_keys];
    if (k->name) {
      /* Already exists, check for change */
      bool match;
      cbor_value_text_string_equals(&i, k->name, &match);
      if (!match) {
        free(k->name);
        k->name = NULL;
      }
    }
    if (!k->name) {
      size_t len;
      if (cbor_value_calculate_string_length(&i, &len) != CborNoError) {
        return;
      }
      len += 1; /* Account for null byte */
      k->name = malloc(len);
      if (!k->name) {
        return;
      }
      if (cbor_value_copy_text_string(&i, k->name, &len, &i) != CborNoError) {
        return;
      }
    } else {
      cbor_value_advance(&i);
    }
    n_keys += 1;
  }

  p->n_feed_fields = n_keys;
}

static void handle_feed_message(struct protocol *p, CborValue *msg) {
  CborValue cbor_values;
  union field_value feed_values[MAX_KEYS];
  if (cbor_value_map_find_value(msg, "values", &cbor_values) != CborNoError) {
    /* TODO: handle */
    return;
  }

  if (!cbor_value_is_array(&cbor_values)) {
    return;
  }

  CborValue i;
  size_t n_values = 0;
  cbor_value_enter_container(&cbor_values, &i);
  while(!cbor_value_at_end(&i)) {
    if (n_values >= MAX_KEYS) {
      return;
    }

    struct field_key *k = &p->field_keys[n_values];

    if (cbor_value_is_unsigned_integer(&i)) {
      k->type = FIELD_UINT32;
      uint64_t val;
      cbor_value_get_uint64(&i, &val);
      feed_values[n_values].as_uint32 = val;
    } else if (cbor_value_is_float(&i)) {
      k->type = FIELD_FLOAT;
      float val;
      cbor_value_get_float(&i, &val);
      feed_values[n_values].as_float = val;
    } else {
      return;
    }
    n_values += 1;
    cbor_value_advance_fixed(&i);
  }
  if (n_values != p->n_feed_fields) {
    return;
  }
  if (p->feed_cb) {
    p->feed_cb(n_values, p->field_keys, feed_values);
  }
}

static size_t calculate_container_length(const CborValue *value) {
  CborValue i;
  if (cbor_value_enter_container(value, &i) != CborNoError) {
    return 0;
  }
  size_t count = 0;
  while(!cbor_value_at_end(&i)) {
    cbor_value_advance(&i);
    count += 1;
  }
  return count;// no call to leave container, leave `value` unaltered
}

static bool parse_cbor_structure_into_node(struct structure_node *dest, struct path_element **path, CborValue *entry);


static struct path_element **duplicate_and_extend_path_element(struct path_element **previous, struct path_element new) {
  size_t current_len = 0;
  if (previous) {
    for (struct path_element **p = previous; *p != NULL; p++);
    current_len++;
  }

  struct path_element **retval = calloc(sizeof(struct path_element *), current_len + 2); /* One extra new element, one null terminator */
  if (!retval) {
    return NULL;
  }
  for (int i = 0; i < current_len; i++) {
    struct path_element *newcopy = malloc(sizeof(struct path_element));
    if (!newcopy) {
      return NULL;
    }
    *newcopy = *previous[i];
    retval[i] = newcopy;
  }

  struct path_element *newcopy = malloc(sizeof(struct path_element));
  if (!newcopy) {
    return NULL;
  }
  *newcopy = new;
  retval[current_len] = newcopy;
  retval[current_len + 1] = NULL;
  return retval;

}

static bool parse_structure_list_into_node(struct structure_node *dest, struct path_element **path, CborValue *entry) {
  size_t len = calculate_container_length(entry);
  struct structure_node *list = calloc(sizeof(struct structure_node), len);
  if (!list) {
    return false;
  }

  CborValue element;
  if (cbor_value_enter_container(entry, &element) != CborNoError) {
    return false;
  }
  for (int i = 0; i < len; i++) {
    struct path_element **newpath = duplicate_and_extend_path_element(path, (struct path_element){
        .type = PATH_IDX,
        .idx = i,
        });
    if (!parse_cbor_structure_into_node(&list[i], newpath, &element)) {
      return false;
    }
  }
  cbor_value_leave_container(entry, &element);

  dest->type = LIST;
  dest->path = path;
  dest->list.len = len;
  dest->list.list = list;

  return true;
}

static bool parse_structure_map_into_node(struct structure_node *dest, struct path_element **path, CborValue *entry) {
  size_t len = calculate_container_length(entry);
  if (len % 2 != 0) {
    return false;
  }
  len /= 2; /* Map length should be double for key/value pairs */
  struct structure_node *list = calloc(sizeof(struct structure_node), len);
  char **names = calloc(sizeof(char *), len);

  if (!list || !names) {
    return false;
  }

  CborValue element;
  if (cbor_value_enter_container(entry, &element) != CborNoError) {
    return false;
  }
  for (int i = 0; i < len; i++) {
    /* handle key name first */
    if (!cbor_value_is_text_string(&element)) {
      return false;
    }
    size_t keylen;
    if (cbor_value_calculate_string_length(&element, &keylen) != CborNoError) {
      return false;
    }
    keylen += 1; /* For null byte */
    names[i] = malloc(keylen);
    cbor_value_copy_text_string(&element, names[i], &keylen, &element);

    struct path_element **newpath = duplicate_and_extend_path_element(path, (struct path_element){
        .type = PATH_STR,
        .str = names[i],
        });

    if (!parse_cbor_structure_into_node(&list[i], newpath, &element)) {
      return false;
    }
  }
  cbor_value_leave_container(entry, &element);

  dest->type = MAP;
  dest->path = path;
  dest->map.len = len;
  dest->map.list = list;
  dest->map.names = names;

  return true;
}


static config_value_type parse_leaf_type(CborValue *entry) {
  CborValue cbor_type;
  if (cbor_value_map_find_value(entry, "_type", &cbor_type) != CborNoError) {
    return VALUE_INVALID;
  }
  if (!cbor_value_is_text_string(&cbor_type)) {
    return VALUE_INVALID;
  }

  config_value_type type = VALUE_INVALID;

  bool match;
  cbor_value_text_string_equals(&cbor_type, "uint32", &match);
  if (match) {
    type = VALUE_UINT32;
  }

  cbor_value_text_string_equals(&cbor_type, "float", &match);
  if (match) {
    type = VALUE_FLOAT;
  }

  cbor_value_text_string_equals(&cbor_type, "bool", &match);
  if (match) {
    type = VALUE_BOOL;
  }

  cbor_value_text_string_equals(&cbor_type, "string", &match);
  if (match) {
    type = VALUE_STRING;
  }

  cbor_value_text_string_equals(&cbor_type, "sensor", &match);
  if (match) {
    type = VALUE_SENSOR;
  }

  cbor_value_text_string_equals(&cbor_type, "table", &match);
  if (match) {
    type = VALUE_TABLE;
  }

  cbor_value_text_string_equals(&cbor_type, "output", &match);
  if (match) {
    type = VALUE_OUTPUT;
  }
  return type;
}

static char *parse_leaf_description(CborValue *entry) {
  CborValue cbor_desc;
  if (cbor_value_map_find_value(entry, "description", &cbor_desc) != CborNoError) {
    return NULL;
  }
  if (!cbor_value_is_text_string(&cbor_desc)) {
    return NULL;
  }

  size_t desc_len;
  if (cbor_value_calculate_string_length(&cbor_desc, &desc_len) != CborNoError) {
    return NULL;
  }

  size_t buflen = desc_len + 1;
  char *desc = malloc(buflen);
  if (!desc) {
    return NULL;
  }
  cbor_value_copy_text_string(&cbor_desc, desc, &buflen, NULL);
  return desc;
}

static char **parse_leaf_choices(CborValue *entry) {
  CborValue cbor_choices;
  if (cbor_value_map_find_value(entry, "choices", &cbor_choices) != CborNoError) {
    return NULL;
  }
  if (!cbor_value_is_array(&cbor_choices)) {
    return NULL;
  }
  size_t count = calculate_container_length(&cbor_choices);
  char **choices = calloc(sizeof(const char *), count + 1);
  choices[count] = NULL;

  CborValue choice_item;
  cbor_value_enter_container(&cbor_choices, &choice_item);
  for (int i = 0; i < count; i++) {
    size_t choice_len;
    if (cbor_value_calculate_string_length(&choice_item, &choice_len) != CborNoError) {
      return NULL;
    }
    choice_len += 1; // Account for the null
    choices[i] = malloc(choice_len);
    cbor_value_copy_text_string(&choice_item, choices[i], &choice_len, &choice_item);
  }
  return choices;
}

static bool parse_structure_leaf_into_node(struct structure_node *dest, struct path_element **path, CborValue *entry) {

  dest->leaf.type = parse_leaf_type(entry);
  if (dest->leaf.type == VALUE_INVALID) {
    return false;
  }

  if (dest->leaf.type == VALUE_STRING) {
    dest->leaf.choices = parse_leaf_choices(entry);
  }

  dest->leaf.description = parse_leaf_description(entry);
  dest->type = LEAF;
  dest->path = path;
  cbor_value_advance(entry);
  return true;
}

static bool parse_cbor_structure_into_node(struct structure_node *dest, struct path_element **path, CborValue *entry) {

  if (cbor_value_is_array(entry)) {
    return parse_structure_list_into_node(dest, path, entry);
  } else if (cbor_value_is_map(entry)) {
    CborValue cbor_type;
    cbor_value_map_find_value(entry, "_type", &cbor_type);
    if (cbor_value_get_type(&cbor_type) == CborInvalidType) {
      /* Not a leaf, parse as a map */
      return parse_structure_map_into_node(dest, path, entry);
    } else {
      /* Is a leaf, parse out the details */
      return parse_structure_leaf_into_node(dest, path, entry);
    }
  }
  return false;
}

static void handle_response_message(struct protocol *p, CborValue *msg) {
  CborValue cbor_id;
  if (cbor_value_map_find_value(msg, "id", &cbor_id) != CborNoError) {
    /* TODO: handle */
    return;
  }

  if (!cbor_value_is_integer(&cbor_id)) {
    return;
  }

  uint64_t id;
  cbor_value_get_uint64(&cbor_id, &id);

  check_thrd(mtx_lock(&p->request_mtx));
  if (!p->request.active || p->request.id != id) {
    fprintf(stderr, "Does this happen?\n");
    check_thrd(mtx_unlock(&p->request_mtx));
    return;
  }

  CborValue cbor_response;
  if (cbor_value_map_find_value(msg, "response", &cbor_response) != CborNoError) {
    fprintf(stderr, "Does this happen2?\n");
    check_thrd(mtx_unlock(&p->request_mtx));
    return;
  }

  if (p->request.type == STRUCTURE) {
    struct structure_node *root = malloc(sizeof(struct structure_node));;
    parse_cbor_structure_into_node(root, NULL, &cbor_response);
    p->request.structure_cb(root, p->request.userdata);
  } else if (p->request.type == GET) {
    switch (p->request.node->leaf.type) {
      case VALUE_UINT32: {
        struct config_value val = {.type = VALUE_UINT32 };
        uint64_t u64val;
        cbor_value_get_uint64(&cbor_response, &u64val);
        val.as_uint32 = u64val;
        p->request.get_callback(val, p->request.userdata);
      default:
                         }
    }
  }

    p->request.active = false;
    check_thrd(mtx_unlock(&p->request_mtx));

}

bool viaems_new_data(struct protocol *p, const uint8_t *data, size_t len) {
  CborParser parser;
  CborValue root;

  if (cbor_parser_init(data, len, 0, &parser, &root) != CborNoError) {
    return false;
  }
  if (!cbor_value_is_map(&root)) {
    return false;
  }


  CborValue type_value;
  if (cbor_value_map_find_value(&root, "type", &type_value) != CborNoError) {
    return false;
  }

  bool is_feed;
  cbor_value_text_string_equals(&type_value, "feed", &is_feed);
  if (is_feed) {
    handle_feed_message(p, &root);
    return true;
  }

  bool is_description;
  cbor_value_text_string_equals(&type_value, "description", &is_description);
  if (is_description) {
    handle_desc_message(p, &root);
    return true;
  }

  bool is_response;
  cbor_value_text_string_equals(&type_value, "response", &is_response);
  if (is_response) {
    handle_response_message(p, &root);
    return true;
  }
  return false;
}

static void blocking_structure_callback(struct structure_node *root, void *userdata) {
  struct protocol *proto = userdata;
  proto->request.node = root;
  check_thrd(cnd_signal(&proto->request_wakeup_cnd));
}

static uint32_t global_id = 1;

bool viaems_get_structure_async(struct protocol *p, structure_callback callback, void *userdata) {

  check_thrd(mtx_lock(&p->request_mtx));
  p->request = (struct request){
    .active = true,
    .type = STRUCTURE,
    .id = global_id++,
    .structure_cb = callback,
    .userdata = userdata,
  };

  check_thrd(mtx_unlock(&p->request_mtx));

  uint8_t buf[512];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map_encoder;
  cbor_encoder_create_map(&encoder, &map_encoder, 3);
  cbor_encode_text_stringz(&map_encoder, "type");
  cbor_encode_text_stringz(&map_encoder, "request");
  cbor_encode_text_stringz(&map_encoder, "method");
  cbor_encode_text_stringz(&map_encoder, "structure");
  cbor_encode_text_stringz(&map_encoder, "id");
  cbor_encode_int(&map_encoder, p->request.id);
  cbor_encoder_close_container(&encoder, &map_encoder);
  size_t written_size = cbor_encoder_get_buffer_size(&encoder, buf);
  if (p->write) {
    p->write(p->write_userdata, buf, written_size);
  }
  return true;
}

static struct timespec time_ms_from_now(int ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (ms * 1000000);
  while (ts.tv_nsec >= 1000000000) {
    ts.tv_nsec -= 1000000000;
    ts.tv_sec += 1;
  }
  return ts;
}

bool viaems_get_structure(struct protocol *p, struct structure_node **res) {

  check_thrd(mtx_lock(&p->request_client_mtx));

  size_t timeout_ms = 1000;
  struct timespec ts = time_ms_from_now(timeout_ms);
  viaems_get_structure_async(p, blocking_structure_callback, p);

  check_thrd(mtx_lock(&p->request_mtx));
  while (p->request.active) {
    if (cnd_timedwait(&p->request_wakeup_cnd, &p->request_mtx, &ts) == thrd_timedout) {
    p->request.active = false;
    check_thrd(mtx_unlock(&p->request_mtx));
    check_thrd(mtx_unlock(&p->request_client_mtx));
    return false;
    }

  }
  *res = p->request.node;
  check_thrd(mtx_unlock(&p->request_mtx));
  check_thrd(mtx_unlock(&p->request_client_mtx));

  return true;
}

bool viaems_send_get_async(struct protocol *p, struct structure_node *node, get_callback cb, void *ud) {
  if (node->type != LEAF) {
    return false;
  }

  check_thrd(mtx_lock(&p->request_mtx));
  p->request = (struct request){
    .active = true,
    .type = GET,
    .node = node,
    .id = global_id++,
    .get_callback = cb,
    .userdata = ud,
  };
  check_thrd(mtx_unlock(&p->request_mtx));

  uint8_t buf[512];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map_encoder;
  cbor_encoder_create_map(&encoder, &map_encoder, 4);
  cbor_encode_text_stringz(&map_encoder, "type");
  cbor_encode_text_stringz(&map_encoder, "request");
  cbor_encode_text_stringz(&map_encoder, "method");
  cbor_encode_text_stringz(&map_encoder, "get");
  cbor_encode_text_stringz(&map_encoder, "id");
  cbor_encode_int(&map_encoder, p->request.id);
  cbor_encode_text_stringz(&map_encoder, "path");

  CborEncoder cbor_path;
  cbor_encoder_create_array(&map_encoder, &cbor_path, CborIndefiniteLength);
  for (struct path_element **i = node->path; *i != NULL; i++) {
    if ((*i)->type == PATH_IDX) {
      cbor_encode_int(&cbor_path, (*i)->idx);
    } else if ((*i)->type == PATH_STR) {
      cbor_encode_text_stringz(&cbor_path, (*i)->str);
    }
  }
  cbor_encoder_close_container(&map_encoder, &cbor_path);
  cbor_encoder_close_container(&encoder, &map_encoder);
  size_t written_size = cbor_encoder_get_buffer_size(&encoder, buf);
  if (p->write) {
    p->write(p->write_userdata, buf, written_size);
  }
  return true;
}

static void blocking_get_callback(struct config_value value, void *userdata) {
  struct protocol *proto = userdata;
  proto->request.value = value;
  cnd_signal(&proto->request_wakeup_cnd);
}

bool viaems_send_get(struct protocol *p, struct structure_node *node, struct config_value *dest) {
  check_thrd(mtx_lock(&p->request_client_mtx));
  size_t timeout_ms = 1000;
  struct timespec ts = time_ms_from_now(timeout_ms);

  viaems_send_get_async(p, node, blocking_get_callback, p);

  check_thrd(mtx_lock(&p->request_mtx));
  while (p->request.active) {
    if (cnd_timedwait(&p->request_wakeup_cnd, &p->request_mtx, &ts) == thrd_timedout) {
      p->request.active = false;
      check_thrd(mtx_unlock(&p->request_mtx));
      check_thrd(mtx_unlock(&p->request_client_mtx));
      return false;
    }
  }
  *dest = p->request.value;
  check_thrd(mtx_unlock(&p->request_mtx));
  check_thrd(mtx_unlock(&p->request_client_mtx));

  return true;
}


static void structure_destroy_child(struct structure_node *node) {
  if (node->path) {
    for (struct path_element **p = node->path; *p != NULL; p++) {
      free(*p);
    }
    free(node->path);
  }
  if (node->type == LEAF) {
    if (node->leaf.description) {
      free(node->leaf.description);
    }
    if (node->leaf.choices) {
      char **choice = node->leaf.choices;
      while (*choice) {
        free(*choice);
        choice++;
      }
      free(node->leaf.choices);
    }
  } else if (node->type == LIST) {
    for (int i = 0; i < node->list.len; i++) {
      structure_destroy_child(&node->list.list[i]);
    }
    free(node->list.list);
  } else if (node->type == MAP) {
    for (int i = 0; i < node->map.len; i++) {
      structure_destroy_child(&node->map.list[i]);
      free(node->map.names[i]);
    }
    free(node->map.list);
    free(node->map.names);
  }
}

void structure_destroy(struct structure_node *node) {
  structure_destroy_child(node);
  free(node);
}


