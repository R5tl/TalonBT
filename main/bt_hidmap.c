// Talon — generic HID gamepad report parser (see bt_hidmap.h).
#include <string.h>
#include "bt_hidmap.h"

// HID Usage Pages / usages we care about.
#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_BUTTON          0x09
#define GD_X    0x30
#define GD_Y    0x31
#define GD_Z    0x32
#define GD_RX   0x33
#define GD_RY   0x34
#define GD_RZ   0x35
#define GD_HAT  0x39

// Talon state indices.
enum { S_DIGITAL, S_A, S_B, S_X, S_Y, S_BLACK, S_WHITE, S_LT, S_RT,
       S_LX, S_LY, S_RX, S_RY };
// Digital d-pad/START/BACK/thumb bits (byte 2 of the XID report).
#define D_UP 0x01
#define D_DOWN 0x02
#define D_LEFT 0x04
#define D_RIGHT 0x08
#define D_START 0x10
#define D_BACK 0x20
#define D_LS 0x40
#define D_RS 0x80

// ---- descriptor parser -----------------------------------------------------

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min, logical_max;
    uint8_t  report_size, report_id;
    uint16_t report_count;
} global_state_t;

static uint32_t item_uval(const uint8_t *p, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static int32_t item_sval(const uint8_t *p, int size) {
    int32_t v = (int32_t)item_uval(p, size);
    if (size == 1 && (v & 0x80)) v |= ~0xFF;
    else if (size == 2 && (v & 0x8000)) v |= ~0xFFFF;
    return v;
}

bool hid_parse_descriptor(const uint8_t *desc, size_t len, hid_layout_t *out) {
    memset(out, 0, sizeof(*out));

    global_state_t g = { 0 };
    uint16_t usages[16]; int n_usages = 0;
    uint16_t usage_min = 0, usage_max = 0; bool have_range = false;
    uint16_t bitpos[8] = { 0 };            // running bit offset per report-id slot
    uint8_t  ids[8]; int n_ids = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) {                   // long item: skip
            if (i >= len) break;
            uint8_t dsize = desc[i];
            i += 2 + dsize;
            continue;
        }
        int size = b & 0x03; if (size == 3) size = 4;
        int type = (b >> 2) & 0x03;
        int tag  = (b >> 4) & 0x0F;
        if (i + size > len) break;
        const uint8_t *data = &desc[i];
        i += size;

        if (type == 1) {                   // Global
            switch (tag) {
                case 0x0: g.usage_page = (uint16_t)item_uval(data, size); break;
                case 0x1: g.logical_min = item_sval(data, size); break;
                case 0x2: g.logical_max = item_sval(data, size); break;
                case 0x7: g.report_size = (uint8_t)item_uval(data, size); break;
                case 0x8: g.report_id = (uint8_t)item_uval(data, size);
                          out->has_report_id = true; break;
                case 0x9: g.report_count = (uint16_t)item_uval(data, size); break;
                default: break;
            }
        } else if (type == 2) {            // Local
            switch (tag) {
                case 0x0: if (n_usages < 16) usages[n_usages++] = (uint16_t)item_uval(data, size); break;
                case 0x1: usage_min = (uint16_t)item_uval(data, size); have_range = true; break;
                case 0x2: usage_max = (uint16_t)item_uval(data, size); have_range = true; break;
                default: break;
            }
        } else if (type == 0) {            // Main
            if (tag == 0x8) {              // Input
                uint8_t flags = size ? data[0] : 0;
                bool constant = flags & 0x01;

                // Find (or make) the running bit cursor for this report id.
                int slot = 0;
                for (; slot < n_ids; slot++) if (ids[slot] == g.report_id) break;
                if (slot == n_ids && n_ids < 8) { ids[n_ids] = g.report_id; n_ids++; }
                uint16_t *cursor = &bitpos[slot < 8 ? slot : 0];

                for (uint16_t f = 0; f < g.report_count; f++) {
                    if (!constant && out->n < HID_MAX_FIELDS) {
                        uint16_t u;
                        if (n_usages > 0) u = usages[f < n_usages ? f : n_usages - 1];
                        else if (have_range) u = usage_min + f;
                        else u = 0;
                        hid_field_t *fld = &out->fields[out->n++];
                        fld->report_id   = g.report_id;
                        fld->bit_offset  = *cursor;
                        fld->bit_size    = g.report_size;
                        fld->usage_page  = g.usage_page;
                        fld->usage       = u;
                        fld->logical_min = g.logical_min;
                        fld->logical_max = g.logical_max;
                    }
                    *cursor += g.report_size;
                }
            }
            // Any main item clears local state.
            n_usages = 0; usage_min = usage_max = 0; have_range = false;
        }
    }
    return out->n > 0;
}

// ---- report extraction -----------------------------------------------------

static uint32_t extract_bits(const uint8_t *data, size_t len, uint16_t off, uint8_t sz) {
    uint32_t v = 0;
    for (uint8_t b = 0; b < sz; b++) {
        uint16_t bit = off + b;
        if (bit / 8 >= len) break;
        if (data[bit / 8] & (1 << (bit % 8))) v |= (1u << b);
    }
    return v;
}

// Scale a raw axis value in [lmin,lmax] to signed 16-bit [-32768,32767].
static int scale_axis(uint32_t raw, int32_t lmin, int32_t lmax) {
    if (lmax <= lmin) return 0;
    double t = ((double)((int32_t)raw - lmin)) / (double)(lmax - lmin);   // 0..1
    return (int)(t * 65535.0) - 32768;
}
// Scale to 0..255 (triggers).
static uint8_t scale_u8(uint32_t raw, int32_t lmin, int32_t lmax) {
    if (lmax <= lmin) return raw ? 255 : 0;
    double t = ((double)((int32_t)raw - lmin)) / (double)(lmax - lmin);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (uint8_t)(t * 255.0);
}

// 8-direction hat (1=up, clockwise) -> d-pad bits.
static uint8_t hat_to_dpad(uint32_t h, int32_t lmin) {
    uint32_t v = h - (lmin == 1 ? 1 : lmin);   // normalize so up=0
    switch (v & 7) {
        case 0: return D_UP;
        case 1: return D_UP | D_RIGHT;
        case 2: return D_RIGHT;
        case 3: return D_DOWN | D_RIGHT;
        case 4: return D_DOWN;
        case 5: return D_DOWN | D_LEFT;
        case 6: return D_LEFT;
        case 7: return D_UP | D_LEFT;
        default: return 0;
    }
}

// Button number (1-based) -> Talon control. Order follows the common
// XInput-ish BLE gamepad layout (A,B,X,Y,LB,RB,...); tweak here if a specific
// pad reports a different order (the /api/bt/debug dump shows the raw bytes).
static void apply_button(int btn, uint32_t pressed, int state[13]) {
    if (!pressed) return;
    switch (btn) {
        case 1: state[S_A] = 255; break;
        case 2: state[S_B] = 255; break;
        case 3: state[S_X] = 255; break;
        case 4: state[S_Y] = 255; break;
        case 5: state[S_WHITE] = 255; break;   // LB
        case 6: state[S_BLACK] = 255; break;   // RB
        case 7: state[S_LT] = 255; break;      // digital LT fallback
        case 8: state[S_RT] = 255; break;      // digital RT fallback
        case 9: state[S_DIGITAL] |= D_BACK; break;   // View/Back
        case 10: state[S_DIGITAL] |= D_START; break; // Menu/Start
        case 11: state[S_DIGITAL] |= D_LS; break;
        case 12: state[S_DIGITAL] |= D_RS; break;
        default: break;
    }
}

bool hid_report_to_state(const hid_layout_t *l, uint8_t report_id,
                         const uint8_t *data, size_t len, int state[13]) {
    bool any = false;
    for (int k = 0; k < l->n; k++) {
        const hid_field_t *f = &l->fields[k];
        if (l->has_report_id && f->report_id != report_id) continue;
        uint32_t raw = extract_bits(data, len, f->bit_offset, f->bit_size);

        if (f->usage_page == PAGE_BUTTON) {
            apply_button(f->usage, raw, state); any = true;
        } else if (f->usage_page == PAGE_GENERIC_DESKTOP) {
            switch (f->usage) {
                case GD_X:  state[S_LX] = scale_axis(raw, f->logical_min, f->logical_max); any = true; break;
                case GD_Y:  state[S_LY] = -scale_axis(raw, f->logical_min, f->logical_max); any = true; break;
                case GD_Z:  state[S_RX] = scale_axis(raw, f->logical_min, f->logical_max); any = true; break;
                case GD_RZ: state[S_RY] = -scale_axis(raw, f->logical_min, f->logical_max); any = true; break;
                case GD_RX: state[S_LT] = scale_u8(raw, f->logical_min, f->logical_max); any = true; break;
                case GD_RY: state[S_RT] = scale_u8(raw, f->logical_min, f->logical_max); any = true; break;
                case GD_HAT: state[S_DIGITAL] |= hat_to_dpad(raw, f->logical_min); any = true; break;
                default: break;
            }
        }
    }
    return any;
}
