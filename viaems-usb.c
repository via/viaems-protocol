#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>
#include <libusb-1.0/libusb.h>
#include "viaems-usb.h"

struct vp_usb {
  thrd_t receive_thrd;
  struct protocol *proto;
  _Atomic bool alive;
  bool connected;
  struct libusb_device_handle *devh;
  struct {
    struct libusb_transfer *xfer;
    uint8_t buffer[16384];
  } transfers[4];
};

static int usb_loop(void *ptr) {
  struct vp_usb *usb = ptr;

  while (atomic_load_explicit(&usb->alive, memory_order_relaxed) == true) {
    libusb_handle_events(NULL);
  }

  libusb_free_transfer(usb->transfers[0].xfer);
  libusb_free_transfer(usb->transfers[1].xfer);
  libusb_free_transfer(usb->transfers[2].xfer);
  libusb_free_transfer(usb->transfers[3].xfer);
  libusb_close(usb->devh);
  libusb_exit(NULL);
  return 0;
}

static void read_callback(struct libusb_transfer *xfer) {
  const uint8_t *rxbuf = xfer->buffer;
  const size_t length = xfer->actual_length;
  struct vp_usb *usb = xfer->user_data;

  if (usb->proto) {
    if (!viaems_new_data(usb->proto, rxbuf, length)) {
      fprintf(stderr, "failed parse\n");
    }
  }

  libusb_fill_bulk_transfer(xfer, usb->devh, 0x81, xfer->buffer, xfer->length, read_callback, usb, 1000);
  libusb_submit_transfer(xfer);
}

static void usb_write(void *userdata, uint8_t *bytes, size_t len) {
  struct vp_usb *usb = userdata;
  int actual_length;
  while (libusb_bulk_transfer(usb->devh, 0x1, bytes, len,
        &actual_length, 0) < 0);
}

bool vp_usb_connect(struct vp_usb *usb, struct protocol *p) {
   const uint16_t vid = 0x1209;
   const uint16_t pid = 0x2041;
   usb->proto = p;

    int rc = libusb_init(NULL);
    if (rc < 0) {
      return false;
    }

    usb->devh = libusb_open_device_with_vid_pid(NULL, vid, pid);
    if (!usb->devh) {
      return false;
    }

    for (int if_num = 0; if_num < 2; if_num++) {
        if (libusb_kernel_driver_active(usb->devh, if_num)) {
            libusb_detach_kernel_driver(usb->devh, if_num);
        }
        rc = libusb_claim_interface(usb->devh, if_num);
        if (rc < 0) {
            return false;
        }
    }
    /* Start configuring the device:
     * - set line state
     */
    const uint32_t ACM_CTRL_DTR = 0x01;
    const uint32_t ACM_CTRL_RTS = 0x02;
    rc = libusb_control_transfer(usb->devh, 0x21, 0x22, ACM_CTRL_DTR | ACM_CTRL_RTS,
                                0, NULL, 0, 0);
    if (rc < 0) {
      return false;
    }

    /* - set line encoding: here 9600 8N1
     * 9600 = 0x2580 ~> 0x80, 0x25 in little endian
     */
    unsigned char encoding[] = { 0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08 };
    rc = libusb_control_transfer(usb->devh, 0x21, 0x20, 0, 0, encoding,
                                sizeof(encoding), 0);
    if (rc < 0) {
      return false;
    }

     for (int i = 0; i < 4; i++) {
       usb->transfers[i].xfer = libusb_alloc_transfer(0);
       if (!usb->transfers[i].xfer) {
         return false;
       }
       libusb_fill_bulk_transfer(usb->transfers[i].xfer, usb->devh, 0x81, usb->transfers[i].buffer, sizeof(usb->transfers[i].buffer), read_callback, p, 1000);

       int rc = libusb_submit_transfer(usb->transfers[i].xfer);
       if (rc != 0) {
         return false;
       }
     }
     // Start thread
     usb->connected = true;
     viaems_set_write_fn(usb->proto, usb_write, usb);
     thrd_create(&usb->receive_thrd, usb_loop, usb);
     return true;
}


struct vp_usb *vp_create_usb() {
  struct vp_usb *usb = malloc(sizeof(struct vp_usb));
  memset(usb, 0, sizeof(struct vp_usb));
  return usb;
}

void vp_destroy_usb(struct vp_usb *usb) {
  if (usb->connected) {
    usb->alive = false;
    thrd_join(usb->receive_thrd, NULL);
  }
  free(usb);
}



