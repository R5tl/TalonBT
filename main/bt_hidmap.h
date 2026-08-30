// Talon — generic HID gamepad report parser.
//
// A BLE HID controller describes its input reports with a HID report
// descriptor. Rather than hardcode one controller's byte layout, this walks the
// descriptor to locate each control's bit offset/size, then extracts them from
// live reports. Covers the standard usages BLE gamepads use (Generic Desktop
// X/Y/Z/Rx/Ry/Rz + Hat, Button page 1..N), which includes BLE Xbox pads and
// most generic BLE gamepads.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  report_id;
    uint16_t bit_offset;     // within the report payload (report id excluded)
    uint8_t  bit_size;
    uint16_t usage_page;
    uint16_t usage;          // button number, or Generic-Desktop usage id
    int32_t  logical_min;
    int32_t  logical_max;
} hid_field_t;

#define HID_MAX_FIELDS 64

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    int         n;
    bool        has_report_id;
} hid_layout_t;

// Parse a raw HID report descriptor into a field table. Returns false if the
// descriptor is malformed or holds no usable gamepad fields.
bool hid_parse_descriptor(const uint8_t *desc, size_t len, hid_layout_t *out);

// Map one input report onto Talon's 13-int state vector
// (digital,a,b,x,y,black,white,lt,rt,lx,ly,rx,ry). Only fields whose report_id
// matches are applied; returns true if anything was mapped.
bool hid_report_to_state(const hid_layout_t *l, uint8_t report_id,
                         const uint8_t *data, size_t len, int state[13]);

#ifdef __cplusplus
}
#endif
