// Talon — USB descriptors, byte-faithful to the original Xbox "Duke" controller
// (Microsoft 045E:0202). One XID interface (class 0x58 / subclass 0x42) with two
// 32-byte interrupt endpoints polled every 4 ms. The Duke carries no string
// descriptors at all (iManufacturer/iProduct/iSerial = 0).
#include "tusb.h"
#include "xid_descriptors.h"

// ---- Device descriptor (18 bytes) ----------------------------------------
// bcdUSB 1.10, class 0, EP0 max packet 8 — matching the real pad. The EP0 size
// tracks CFG_TUD_ENDPOINT0_SIZE (pinned to 8 in tusb_config.h) so the stack and
// the descriptor can never disagree.
static const uint8_t s_device_desc[18] = {
    18, TUSB_DESC_DEVICE, 0x10, 0x01, 0x00, 0x00, 0x00, CFG_TUD_ENDPOINT0_SIZE,
    0x5E, 0x04,          // idVendor  0x045E (Microsoft)
    0x02, 0x02,          // idProduct 0x0202 (Xbox Controller / Duke)
    0x00, 0x01,          // bcdDevice 1.00
    0, 0, 0,             // no strings, like the real pad
    1                    // bNumConfigurations
};

// ---- Configuration descriptor (32 bytes) ---------------------------------
static const uint8_t s_config_desc[] = {
    // config: wTotalLength 32, 1 interface, bus-powered, 100 mA
    0x09, TUSB_DESC_CONFIGURATION, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    // interface 0, alt 0: 2 endpoints, class 0x58 (XID), subclass 0x42
    0x09, TUSB_DESC_INTERFACE, 0x00, 0x00, 0x02, XID_ITF_CLASS, XID_ITF_SUBCLASS, 0x00, 0x00,
    // EP2 IN, interrupt, maxpkt 32, interval 4 ms
    0x07, TUSB_DESC_ENDPOINT, XID_EP_IN, 0x03, 0x20, 0x00, 0x04,
    // EP2 OUT, interrupt, maxpkt 32, interval 4 ms
    0x07, TUSB_DESC_ENDPOINT, XID_EP_OUT, 0x03, 0x20, 0x00, 0x04,
};

// ---- XID descriptor -------------------------------------------------------
// Served for the vendor GET_DESCRIPTOR (bmRequestType 0xC1, wValue 0x4200).
// bType 1 = game controller, bSubType 1 = Duke gamepad.
const uint8_t xid_desc_xid[16] = {
    0x10, 0x42,          // bLength, bDescriptorType (XID)
    0x00, 0x01,          // bcdXid 1.00
    0x01, 0x01,          // bType gamepad, bSubType Duke
    XID_REPORT_LEN,      // bMaxInputReportSize
    XID_RUMBLE_LEN,      // bMaxOutputReportSize
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // wAlternateProductIds[4]
};

// ---- XID capabilities -----------------------------------------------------
// Vendor GET_CAPABILITIES (bRequest 0x01): a report image with every bit the
// device can drive set to 1.
const uint8_t xid_caps_in[XID_REPORT_LEN] = {
    0x00, XID_REPORT_LEN, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
const uint8_t xid_caps_out[XID_RUMBLE_LEN] = {
    0x00, XID_RUMBLE_LEN, 0xFF, 0xFF, 0xFF, 0xFF,
};

// The Duke has no strings; only the LangID slot exists so index-0 lookups are sane.
static const char *const s_strings[] = {
    (const char[]){ 0x09, 0x04 },   // 0: LangID 0x0409
};

void xid_get_descriptors(const tusb_desc_device_t **dev, const uint8_t **cfg,
                         const char ***strs, int *nstr) {
    *dev  = (const tusb_desc_device_t *)s_device_desc;   // same 18-byte layout
    *cfg  = s_config_desc;
    *strs = (const char **)s_strings;
    *nstr = 1;
}
