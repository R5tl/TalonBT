// Talon — controller state store. Written by the web UI (httpd task), read by
// the XID class driver (TinyUSB task) each time it arms an input report.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "talon.h"
#include "xid_descriptors.h"

typedef struct {
    uint8_t digital;                 // XID_* bits (byte 2 of the report)
    uint8_t a, b, x, y, black, white;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} talon_state_t;

static talon_state_t s_state;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void talon_report_build(uint8_t out[XID_REPORT_LEN]) {
    portENTER_CRITICAL(&s_mux);
    talon_state_t c = s_state;
    portEXIT_CRITICAL(&s_mux);

    out[0] = 0x00;
    out[1] = XID_REPORT_LEN;
    out[2] = c.digital;
    out[3] = 0x00;
    out[4] = c.a;  out[5] = c.b;  out[6] = c.x;  out[7] = c.y;
    out[8] = c.black;  out[9] = c.white;
    out[10] = c.lt;    out[11] = c.rt;
    out[12] = (uint8_t)(c.lx & 0xFF); out[13] = (uint8_t)((uint16_t)c.lx >> 8);
    out[14] = (uint8_t)(c.ly & 0xFF); out[15] = (uint8_t)((uint16_t)c.ly >> 8);
    out[16] = (uint8_t)(c.rx & 0xFF); out[17] = (uint8_t)((uint16_t)c.rx >> 8);
    out[18] = (uint8_t)(c.ry & 0xFF); out[19] = (uint8_t)((uint16_t)c.ry >> 8);
}

static uint8_t clamp_u8(int v)  { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static int16_t clamp_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

// Analog face buttons: accept 0/1 as full release/press so the same API drives
// digital-style and pressure-style callers.
static uint8_t face_val(int v) { return v == 1 ? 255 : clamp_u8(v); }

bool talon_set_control(const char *name, int v) {
    uint8_t bit = 0;
    if      (!strcmp(name, "up"))    bit = XID_DPAD_UP;
    else if (!strcmp(name, "down"))  bit = XID_DPAD_DOWN;
    else if (!strcmp(name, "left"))  bit = XID_DPAD_LEFT;
    else if (!strcmp(name, "right")) bit = XID_DPAD_RIGHT;
    else if (!strcmp(name, "start")) bit = XID_START;
    else if (!strcmp(name, "back"))  bit = XID_BACK;
    else if (!strcmp(name, "ls"))    bit = XID_LSTICK;
    else if (!strcmp(name, "rs"))    bit = XID_RSTICK;

    portENTER_CRITICAL(&s_mux);
    bool ok = true;
    if (bit) {
        if (v) s_state.digital |= bit; else s_state.digital &= ~bit;
    }
    else if (!strcmp(name, "a"))     s_state.a     = face_val(v);
    else if (!strcmp(name, "b"))     s_state.b     = face_val(v);
    else if (!strcmp(name, "x"))     s_state.x     = face_val(v);
    else if (!strcmp(name, "y"))     s_state.y     = face_val(v);
    else if (!strcmp(name, "black")) s_state.black = face_val(v);
    else if (!strcmp(name, "white")) s_state.white = face_val(v);
    else if (!strcmp(name, "lt"))    s_state.lt    = face_val(v);
    else if (!strcmp(name, "rt"))    s_state.rt    = face_val(v);
    else if (!strcmp(name, "lx"))    s_state.lx    = clamp_s16(v);
    else if (!strcmp(name, "ly"))    s_state.ly    = clamp_s16(v);
    else if (!strcmp(name, "rx"))    s_state.rx    = clamp_s16(v);
    else if (!strcmp(name, "ry"))    s_state.ry    = clamp_s16(v);
    else ok = false;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void talon_reset_controls(void) {
    portENTER_CRITICAL(&s_mux);
    memset(&s_state, 0, sizeof(s_state));
    portEXIT_CRITICAL(&s_mux);
}
