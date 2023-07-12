#ifndef VIAEMS_USB_H
#define VIAEMS_USB_H

#include "viaems-c.h"

struct vp_usb;
struct vp_usb *vp_create_usb();
void vp_destroy_usb(struct vp_usb *usb);

struct protocol;
bool vp_usb_connect(struct vp_usb *usb, struct protocol *p);

#endif
