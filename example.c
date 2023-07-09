#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <threads.h>
#include "viaems-c.h"

#include <libusb-1.0/libusb.h>

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(EXIT_FAILURE);
}

static int count = 0;
static struct libusb_device_handle *devh;

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

static void usb_write(void *userdata, const uint8_t *bytes, size_t len) {
  int actual_length;
  int rc;
  while (libusb_bulk_transfer(devh, 0x1, bytes, len,
        &actual_length, 0) < 0);
}

static void read_callback(struct libusb_transfer *xfer) {
  const uint8_t *rxbuf = xfer->buffer;
  const size_t length = xfer->actual_length;
  struct protocol *p = xfer->user_data;
//  fprintf(stderr, "READ: %d\n", length);

  size_t remaining = length;
  while (remaining > 0) {
    size_t used;
    if (!viaems_new_data(p, rxbuf, remaining, &used)) {
      fprintf(stderr, "failed parse\n");
      break;
    }
    remaining -= used;
    rxbuf += used;
  }

  libusb_fill_bulk_transfer(xfer, devh, 0x81, xfer->buffer, xfer->length, read_callback, p, 1000);
  libusb_submit_transfer(xfer);
}

thrd_t usb_thread;

struct {
  struct libusb_transfer *xfer;
  uint8_t buf[16384];
} xfers[4] = {};

static int usb_loop(void *none) {
  while (count < 200000) {
    libusb_handle_events(NULL);
  }
  libusb_free_transfer(xfers[0].xfer);
  libusb_free_transfer(xfers[1].xfer);
  libusb_free_transfer(xfers[2].xfer);
  libusb_free_transfer(xfers[3].xfer);
  libusb_close(devh);
  libusb_exit(NULL);
  return 0;
}

void do_usb(struct protocol *p) {
   const uint16_t vid = 0x1209;
   const uint16_t pid = 0x2041;

    int rc = libusb_init(NULL);
    if (rc < 0) {
      return;
    }
    devh = libusb_open_device_with_vid_pid(NULL, vid, pid);
    if (!devh) {
      return;
    }

    for (int if_num = 0; if_num < 2; if_num++) {
        if (libusb_kernel_driver_active(devh, if_num)) {
            libusb_detach_kernel_driver(devh, if_num);
        }
        rc = libusb_claim_interface(devh, if_num);
        if (rc < 0) {
            return;
        }
    }
    /* Start configuring the device:
     * - set line state
     */
    const uint32_t ACM_CTRL_DTR = 0x01;
    const uint32_t ACM_CTRL_RTS = 0x02;
    rc = libusb_control_transfer(devh, 0x21, 0x22, ACM_CTRL_DTR | ACM_CTRL_RTS,
                                0, NULL, 0, 0);
    if (rc < 0) {
      return;
    }

    /* - set line encoding: here 9600 8N1
     * 9600 = 0x2580 ~> 0x80, 0x25 in little endian
     */
    unsigned char encoding[] = { 0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08 };
    rc = libusb_control_transfer(devh, 0x21, 0x20, 0, 0, encoding,
                                sizeof(encoding), 0);
    if (rc < 0) {
      return;
    }

     for (int i = 0; i < 4; i++) {
       xfers[i].xfer = libusb_alloc_transfer(0);
       if (!xfers[i].xfer) {
         return;
       }
       libusb_fill_bulk_transfer(xfers[i].xfer, devh, 0x81, xfers[i].buf, sizeof(xfers[i].buf), read_callback, p, 1000);

       int rc = libusb_submit_transfer(xfers[i].xfer);
       if (rc != 0) {
         return;
       }
     }
     // Start thread
     viaems_set_write_fn(p, usb_write, NULL);
     thrd_create(&usb_thread, usb_loop, NULL);
}

static int to_fds[2];
static int from_fds[2];
static thrd_t sim_thread;

static int sim_loop(void *ptr) {
  struct protocol *p = ptr;
  while (true) {
    uint8_t buf[16384];
    ssize_t amt = read(from_fds[0], buf, sizeof(buf));
    size_t remaining = amt;
    uint8_t *ptr = buf;
    while (remaining > 0) {
      size_t used;
      viaems_new_data(p, ptr, remaining, &used);
      remaining -= used;
      ptr += used;
    }
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


static struct structure_node *config_structure = NULL;
static struct protocol *proto = NULL;

static void get_uint32_response(uint32_t value, void *ud) {
  fprintf(stderr, "got uint32: %u\n", value); 
}

static void get_structure_response(struct structure_node *root, void *userdata) {
  fprintf(stderr, "got structure callback\n");
  dump_structure(root, 0);
  config_structure = root;
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

  do_usb(p);
//  do_sim(p, "/home/user/dev/viaems/obj/hosted/viaems");
  proto = p;

#if 0
  viaems_get_structure(p, &config_structure);
  dump_structure(config_structure, 0);

  struct structure_node *n = &config_structure->map.list[0].map.list[2];
  struct config_value val;
  viaems_send_get(p, n, &val);

#endif
  thrd_t threads[50];
  for (int i = 0; i < 50; i++) {
    thrd_create(&threads[i], doer, p);
  }
  for (int i = 0; i < 50; i++) {
    thrd_join(threads[i], NULL);
  }
  fprintf(stderr, "completed!\n");
  thrd_join(usb_thread, NULL);
//  thrd_join(sim_thread, NULL);
  viaems_destroy_protocol(&p);
  return 0;
}

