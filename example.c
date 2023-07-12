#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <threads.h>
#include "viaems-c.h"
#include "viaems-usb.h"

#include <libusb-1.0/libusb.h>

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}

static int count = 0;

static void new_feed_data(size_t n_fields, const struct field_key *keys, const union field_value *values) {
  for (int i = 0; i < n_fields; i++) {
    if (keys[i].type == FIELD_FLOAT) {
//      fprintf(stdout, "%s(%f) ", keys[i].name, values[i].as_float);
    } else if (keys[i].type == FIELD_UINT32) {
   //   fprintf(stdout, "%s(%u) ", keys[i].name, values[i].as_uint32);
    }
  }
  //fprintf(stdout, "\n");
  count += 1;
}

static int to_fds[2];
static int from_fds[2];
static thrd_t sim_thread;

static int sim_loop(void *ptr) {
  struct protocol *p = ptr;
  int times = 0;
  while (true) {
    times++;
    if (times > 100000) break;
    uint8_t buf[16384];
    ssize_t amt = read(from_fds[0], buf, sizeof(buf));
    viaems_new_data(p, buf, amt);
  }
}

static void sim_write(void *userdata, const uint8_t *bytes, size_t len) {
  ssize_t amt = write(to_fds[1], bytes, len);
}

static void do_sim(struct protocol *p, const char *path) {
  pipe(to_fds);
  pipe(from_fds);
  pid_t pid = fork();
  if (pid == 0) {
    dup2(to_fds[0], 0);
    dup2(from_fds[1], 1);
    char *const argv[] = {path, NULL};
    execv(path, argv);
  }

  viaems_set_write_fn(p, sim_write, NULL);
  thrd_create(&sim_thread, sim_loop, p);
}
  

static void indent(size_t l) {
  while(l > 0) {
    printf("    ");
    l--;
  }
}

static void dump_path(struct path_element **path) {
  fprintf(stderr, "[");
  if (path) {
    for (struct path_element **p = path; *p != NULL; p++) {
      if ((*p)->type == PATH_IDX) {
        fprintf(stderr, " %lu ", (*p)->idx);
      } else if ((*p)->type == PATH_STR) {
        fprintf(stderr, " %s ", (*p)->str);
      }
    }
  }
  fprintf(stderr, "]\n");
}


static void dump_structure(const struct structure_node *node, size_t level) {
  dump_path(node->path);
  if (node->type == LIST) {
    for (int i = 0; i < node->list.len; i++) {
      printf("\n");
      indent(level);
      printf("%d: ", i);
      dump_structure(&node->list.list[i], level + 1);
    }
  } else if (node->type == MAP) {
    printf("\n");
    for (int i = 0; i < node->map.len; i++) {
      indent(level);
      printf("%s: ", node->map.names[i]);
      dump_structure(&node->list.list[i], level + 1);
    } 
  } else if (node->type == LEAF) {
    if (node->leaf.choices) {
      printf("(%s) %s [", config_value_type_as_string(node->leaf.type), node->leaf.description ? node->leaf.description : "");
      char **choice = node->leaf.choices;
      while (*choice) {
        printf("%s,", *choice);
        choice++;
      }
      printf("]\n");

    } else {
      printf("(%s) %s\n", config_value_type_as_string(node->leaf.type), node->leaf.description ? node->leaf.description : "");
    }
  }
}


static int doer(void *ptr) {
  struct protocol *p = ptr;

  struct structure_node *root;
  if (!viaems_get_structure(p, &root)) {
    fprintf(stderr, "failed: viaems_get_structure\n");
    return 0;
  }

  struct structure_node *n = &root->map.list[0].map.list[2];
  struct config_value val;
  if (!viaems_send_get(p, n, &val)) {
    fprintf(stderr, "failed: viaems_send_get\n");
    return 0;
  }
  return 0;
}


int main(void) {
  struct protocol *p;

  if (!viaems_create_protocol(&p)) {
    die("viaems_create_protocol");
  }
  viaems_set_feed_cb(p, new_feed_data);

  struct vp_usb *usb = vp_create_usb();
  vp_usb_connect(usb, p);
//  do_sim(p, "/home/user/dev/viaems/obj/hosted/viaems");

  thrd_t threads[50];
  for (int i = 0; i < 50; i++) {
    thrd_create(&threads[i], doer, p);
  }
  for (int i = 0; i < 50; i++) {
    thrd_join(threads[i], NULL);
  }
  fprintf(stderr, "completed!\n");
//  thrd_join(sim_thread, NULL);
  sleep(10);
  vp_destroy_usb(usb);
  viaems_destroy_protocol(&p);
  return 0;
}

