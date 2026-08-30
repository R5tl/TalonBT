// Talon — USB descriptors for the original Xbox "Duke" controller (XID device).
#pragma once
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XID_EP_IN       0x82    // interrupt IN: 20-byte input report
#define XID_EP_OUT      0x02    // interrupt OUT: 6-byte rumble report
#define XID_REPORT_LEN  20
#define XID_RUMBLE_LEN  6

#define XID_ITF_CLASS    0x58   // USB_CLASS_XID
#define XID_ITF_SUBCLASS 0x42

// Digital-button bits in report byte 2.
#define XID_DPAD_UP     0x01
#define XID_DPAD_DOWN   0x02
#define XID_DPAD_LEFT   0x04
#define XID_DPAD_RIGHT  0x08
#define XID_START       0x10
#define XID_BACK        0x20
#define XID_LSTICK      0x40
#define XID_RSTICK      0x80

void xid_get_descriptors(const tusb_desc_device_t **dev, const uint8_t **cfg,
                         const char ***strs, int *nstr);

// XID-specific descriptor + capability blobs served over EP0.
extern const uint8_t xid_desc_xid[16];
extern const uint8_t xid_caps_in[XID_REPORT_LEN];
extern const uint8_t xid_caps_out[XID_RUMBLE_LEN];

#ifdef __cplusplus
}
#endif
