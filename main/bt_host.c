// Talon — BLE HID controller host (see bt_host.h). Uses ESP-IDF's esp_hidh
// (Bluedroid BLE) plus the vendored esp_hid_gap scan helper. Discovered/paired
// controllers stream input reports; a generic HID parser (bt_hidmap) maps them
// onto the XID input state, so the Xbox sees the same pad the browser relay
// drives.
#include <string.h>
#include <stdio.h>
#include "bt_host.h"
#include "bt_hidmap.h"
#include "talon.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

static const char *TAG = "talon.bt";
#define NVS_NS "talon"

static esp_hidh_dev_t *s_dev;
static volatile bool s_connected;
static bool     s_have_bond;
static uint8_t  s_bond_addr[6];
static uint8_t  s_bond_atype;
static char     s_dev_name[40];
static hid_layout_t s_layout;
static bool     s_layout_ok;
static volatile uint32_t s_reports;
static uint8_t  s_last_raw[32];
static uint8_t  s_last_len, s_last_id;

// ---- bond persistence ------------------------------------------------------

static void bond_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_bond_addr);
    if (nvs_get_blob(h, "bt_addr", s_bond_addr, &n) == ESP_OK && n == 6) {
        uint8_t at = 0;
        nvs_get_u8(h, "bt_atype", &at);
        s_bond_atype = at;
        s_have_bond = true;
    }
    nvs_close(h);
}

static void bond_store(const uint8_t addr[6], uint8_t atype) {
    memcpy(s_bond_addr, addr, 6);
    s_bond_atype = atype;
    s_have_bond = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bt_addr", addr, 6);
        nvs_set_u8(h, "bt_atype", atype);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void bond_erase(void) {
    s_have_bond = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "bt_addr");
        nvs_erase_key(h, "bt_atype");
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- connect / reconnect ---------------------------------------------------

static void open_bond_task(void *arg) {
    (void)arg;
    // Retry the bonded controller until it answers (it may be asleep/off).
    while (s_have_bond && !s_connected) {
        ESP_LOGI(TAG, "connecting to bonded controller %02x:%02x:%02x:%02x:%02x:%02x",
                 s_bond_addr[0], s_bond_addr[1], s_bond_addr[2],
                 s_bond_addr[3], s_bond_addr[4], s_bond_addr[5]);
        esp_hidh_dev_open(s_bond_addr, ESP_HID_TRANSPORT_BLE, s_bond_atype);
        // OPEN/CLOSE arrive on the event handler; wait before retrying.
        for (int i = 0; i < 50 && !s_connected && s_have_bond; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_connected) vTaskDelay(pdMS_TO_TICKS(3000));
    }
    vTaskDelete(NULL);
}

static void start_reconnect(void) {
    if (s_have_bond && !s_connected)
        xTaskCreate(open_bond_task, "bt_open", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
}

// ---- HID events ------------------------------------------------------------

static void build_layout(esp_hidh_dev_t *dev) {
    s_layout_ok = false;
    size_t nmaps = 0;
    esp_hid_raw_report_map_t *maps = NULL;
    if (esp_hidh_dev_report_maps_get(dev, &nmaps, &maps) != ESP_OK || !maps || !nmaps)
        return;
    // Parse the first map that yields gamepad fields.
    for (size_t m = 0; m < nmaps; m++) {
        if (maps[m].data && maps[m].len &&
            hid_parse_descriptor(maps[m].data, maps[m].len, &s_layout)) {
            s_layout_ok = true;
            ESP_LOGI(TAG, "parsed HID map %u: %d fields", (unsigned)m, s_layout.n);
            return;
        }
    }
    ESP_LOGW(TAG, "no usable gamepad fields in report map");
}

static void hidh_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_hidh_event_t ev = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)data;

    switch (ev) {
    case ESP_HIDH_OPEN_EVENT: {
        if (!p->open.dev) break;
        s_dev = p->open.dev;
        const char *nm = esp_hidh_dev_name_get(s_dev);
        strlcpy(s_dev_name, nm ? nm : "BLE controller", sizeof(s_dev_name));
        const uint8_t *bda = esp_hidh_dev_bda_get(s_dev);
        if (bda) bond_store(bda, s_bond_atype);
        build_layout(s_dev);
        s_connected = true;
        ESP_LOGI(TAG, "controller open: %s", s_dev_name);
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        s_reports++;
        s_last_id  = (uint8_t)p->input.report_id;
        s_last_len = p->input.length > sizeof(s_last_raw) ? sizeof(s_last_raw) : (uint8_t)p->input.length;
        memcpy(s_last_raw, p->input.data, s_last_len);
        if (s_layout_ok) {
            // The BLE pad fully owns the input while connected: rebuild the
            // whole report from this frame (absolute state, not deltas).
            int st[13];
            memset(st, 0, sizeof(st));
            if (hid_report_to_state(&s_layout, (uint8_t)p->input.report_id,
                                    p->input.data, p->input.length, st))
                talon_set_state_all(st);
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGW(TAG, "controller closed");
        s_connected = false;
        s_layout_ok = false;
        if (p->close.dev) esp_hidh_dev_free(p->close.dev);
        s_dev = NULL;
        talon_reset_controls();
        start_reconnect();                       // auto-reconnect if still bonded
        break;
    default:
        break;
    }
}

// ---- public API ------------------------------------------------------------

void bt_host_start(void) {
    // esp_hid_gap_init brings up the BT controller + Bluedroid + GAP in BLE
    // mode; WiFi is already running, so the coexistence arbiter shares the radio.
    if (esp_hid_gap_init(HIDH_BLE_MODE) != ESP_OK) {
        ESP_LOGE(TAG, "BLE GAP init failed — BT controller support disabled");
        return;
    }
    esp_hidh_config_t cfg = {
        .callback = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    if (esp_hidh_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed");
        return;
    }
    ESP_LOGI(TAG, "BLE HID host ready");
    bond_load();
    start_reconnect();
}

static void addr_to_str(const uint8_t a[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}
static bool str_to_addr(const char *s, uint8_t out[6]) {
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

// Remember the addr->addr_type from the most recent scan, so connect() knows
// how to open a public vs random address.
static uint8_t s_scan_addr[8][6];
static uint8_t s_scan_atype[8];
static int     s_scan_n;

int bt_host_scan_json(char *out, size_t cap) {
    size_t num = 0;
    esp_hid_scan_result_t *results = NULL;
    esp_hid_scan(3, &num, &results);

    s_scan_n = 0;
    size_t o = 0;
    o += snprintf(out + o, cap - o, "[");
    int written = 0;
    for (esp_hid_scan_result_t *r = results; r && o + 96 < cap; r = r->next) {
        if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
        char addr[18];
        addr_to_str(r->bda, addr);
        int is_pad = (r->usage == ESP_HID_USAGE_GAMEPAD || r->usage == ESP_HID_USAGE_JOYSTICK);
        char nm[40];
        strlcpy(nm, r->name ? r->name : "", sizeof(nm));
        for (char *c = nm; *c; c++) if (*c == '"' || *c == '\\') *c = ' ';
        o += snprintf(out + o, cap - o, "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"gamepad\":%d}",
                      written ? "," : "", addr, nm, r->rssi, is_pad);
        written++;
        if (s_scan_n < 8) { memcpy(s_scan_addr[s_scan_n], r->bda, 6);
                            s_scan_atype[s_scan_n] = r->ble.addr_type; s_scan_n++; }
    }
    snprintf(out + o, cap - o, "]");
    if (results) esp_hid_scan_results_free(results);
    return written;
}

bool bt_host_connect(const char *addr_str) {
    uint8_t addr[6];
    if (!str_to_addr(addr_str, addr)) return false;
    // Find the address type from the last scan (default public if unseen).
    uint8_t atype = 0;
    for (int i = 0; i < s_scan_n; i++)
        if (!memcmp(s_scan_addr[i], addr, 6)) { atype = s_scan_atype[i]; break; }
    s_bond_atype = atype;
    bond_store(addr, atype);
    start_reconnect();
    return true;
}

void bt_host_forget(void) {
    bool was = s_have_bond;
    bond_erase();
    if (s_dev && esp_hidh_dev_exists(s_dev)) esp_hidh_dev_close(s_dev);
    s_connected = false;
    if (was) talon_reset_controls();
    ESP_LOGI(TAG, "controller forgotten");
}

void bt_host_stop(void) {
    s_have_bond = false;                 // stop the reconnect task's retry loop
    if (s_dev && esp_hidh_dev_exists(s_dev)) esp_hidh_dev_close(s_dev);
    s_connected = false;
    esp_hidh_deinit();
    // Fully tear BT down (not just disable) so the WiFi/BT software-coexistence
    // layer is removed — with coex still registered, the first OTA flash write
    // asserts (xQueueSemaphoreTake, scheduler suspended). Bluedroid before the
    // controller; best-effort, the device reboots after OTA anyway.
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));      // let controller teardown settle
    ESP_LOGI(TAG, "BT torn down for OTA");
}

void bt_host_status_json(char *out, size_t cap) {
    char addr[18] = "";
    if (s_have_bond) addr_to_str(s_bond_addr, addr);
    // Last raw report as hex, for empirical mapping.
    char hex[70]; size_t ho = 0;
    for (int i = 0; i < s_last_len && ho + 3 < sizeof(hex); i++)
        ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x", s_last_raw[i]);
    hex[ho] = '\0';
    snprintf(out, cap,
        "\"bt_connected\":%d,\"bt_bonded\":%d,\"bt_addr\":\"%s\",\"bt_name\":\"%s\","
        "\"bt_reports\":%lu,\"bt_last_id\":%u,\"bt_last\":\"%s\",\"bt_mapped\":%d",
        s_connected ? 1 : 0, s_have_bond ? 1 : 0, addr, s_dev_name,
        (unsigned long)s_reports, s_last_id, hex, s_layout_ok ? 1 : 0);
}
