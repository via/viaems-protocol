#ifndef VIAEMS_PROTOCOL_H
#define VIAEMS_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
  FIELD_UINT32,
  FIELD_FLOAT,
} feed_field_type;

struct field_key {
  char *name;
  feed_field_type type;
};

union field_value {
  uint32_t as_uint32;
  float as_float;
};

typedef enum {
  VALUE_INVALID,
  VALUE_UINT32,
  VALUE_FLOAT,
  VALUE_STRING,
  VALUE_BOOL,
  VALUE_SENSOR,
  VALUE_TABLE,
  VALUE_OUTPUT,
} config_value_type;

static inline const char *config_value_type_as_string(const config_value_type t) {
  switch (t) {
    case VALUE_INVALID:
          return "invalid";
    case VALUE_UINT32:
          return "uint32";
    case VALUE_FLOAT:
          return "float";
    case VALUE_BOOL:
          return "bool";
    case VALUE_STRING:
          return "string";
    case VALUE_SENSOR:
          return "sensor";
    case VALUE_TABLE:
          return "table";
    case VALUE_OUTPUT:
          return "output";
  }
}

struct path_element {
  enum {
    PATH_STR,
    PATH_IDX,
  } type;
  union {
    char *str;
    uint32_t idx;
  };
};

struct structure_node;
struct structure_list {
  size_t len;
  struct structure_node *list;
};

struct structure_map {
  size_t len;
  struct structure_node *list;
  char **names;
};

struct structure_leaf {
  config_value_type type;
  char *description;
  char **choices;
};

struct structure_node {
  struct path_element **path;
  enum {
    LEAF,
    LIST,
    MAP,
  } type;
  union {
    struct structure_list list;
    struct structure_map map;
    struct structure_leaf leaf;
  };
};

bool structure_node_is_list(struct structure_node *);
bool structure_node_is_map(struct structure_node *);
bool structure_node_is_leaf(struct structure_node *);
void structure_destroy(struct structure_node *root);
struct structure_node *structure_find_node(struct structure_node *root, const char *path);

typedef void (*write_fn)(void *userdata, const uint8_t *bytes, size_t len);

typedef void (*feed_callback)(size_t n_fields, const struct field_key *keys, const union field_value *);
typedef void (*structure_callback)(struct structure_node *root, void *userdata);
typedef void (*get_uint32_callback)(uint32_t value, void *userdata);

struct protocol;
bool viaems_create_protocol(struct protocol **);
void viaems_destroy_protocol(struct protocol **);
void viaems_set_write_fn(struct protocol *, write_fn, void *userdata);
void viaems_set_feed_cb(struct protocol *, feed_callback);
bool viaems_new_data(struct protocol *, const uint8_t *data, size_t len);

bool viaems_send_get_structure(struct protocol *p, structure_callback, void *userdata);
bool viaems_send_get_uint32(struct protocol *p, struct structure_node *node, get_uint32_callback, void *userdata);

bool viaems_get_structure_blocking(struct protocol *p, struct structure_node **node);
bool viaems_get_uint32_blocking(struct protocol *p, uint32_t *value);

#endif
