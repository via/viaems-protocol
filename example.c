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
  if (libusb_bulk_transfer(devh, 0x1, bytes, len,
        &actual_length, 0) < 0) {
    fprintf(stderr, "Error while sending char\n");
  }
}

static void read_callback(struct libusb_transfer *xfer) {
  const uint8_t *rxbuf = xfer->buffer;
  const size_t length = xfer->actual_length;
  struct protocol *p = xfer->user_data;

  viaems_new_data(p, rxbuf, length);

  libusb_fill_bulk_transfer(xfer, devh, 0x81, xfer->buffer, xfer->length, read_callback, p, 1000);
  libusb_submit_transfer(xfer);
}

thrd_t usb_thread;

static int usb_loop(void *) {
     while (count < 100000) {
       libusb_handle_events(NULL);
     }
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

     struct {
       struct libusb_transfer *xfer;
       uint8_t buf[16384];
     } xfers[4];

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

static void indent(size_t l) {
  while(l > 0) {
    printf("    ");
    l--;
  }
}

static void dump_structure(const struct structure_node *node, size_t level) {
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
    printf("(%s) %s\n", config_value_type_as_string(node->leaf.type), node->leaf.description ? node->leaf.description : "");
  }
}



static void get_structure_response(struct structure_node *root, void *userdata) {
  fprintf(stderr, "got structure callback\n");
  dump_structure(root, 0);
//  structure_destroy(root);
}


int main(void) {
  struct protocol *p;

  if (!viaems_create_protocol(&p)) {
    die("viaems_create_protocol");
  }
  viaems_set_feed_cb(p, new_feed_data);

  do_usb(p);

  viaems_send_get_structure(p, get_structure_response, NULL);

  thrd_join(usb_thread, NULL);
  viaems_destroy_protocol(&p);
  return 0;
}

