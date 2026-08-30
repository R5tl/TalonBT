// Talon — HTTP server: serves the controller web UI and the /api endpoints
// that drive the XID input state.
//
//   GET /                        the controller page (embedded index.html)
//   GET /api/btn?b=NAME&v=0|1    hold / release a control
//   GET /api/axis?a=NAME&v=INT   set an analog control (axes -32768..32767)
//   GET /api/press?b=NAME[&ms=N] tap: press, wait N ms (default 120), release
//   GET /api/release             release everything
//   GET /api/status              JSON diagnostics
//
// Everything is GET so a plain curl can drive the pad from scripts.
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "tusb.h"
#include "talon.h"
#include "wifi_net.h"

static const char *TAG = "talon.web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// Pull ?key= out of the query string; returns false if absent.
static bool qs_value(httpd_req_t *req, const char *key, char *out, size_t outlen) {
    char qs[128];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return false;
    return httpd_query_key_value(qs, key, out, outlen) == ESP_OK;
}

static esp_err_t api_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_bad(httpd_req_t *req, const char *why) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", why);
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t btn_get(httpd_req_t *req) {
    char b[16], v[12];
    if (!qs_value(req, "b", b, sizeof(b))) return api_bad(req, "missing b");
    int val = qs_value(req, "v", v, sizeof(v)) ? atoi(v) : 1;
    if (!talon_set_control(b, val)) return api_bad(req, "unknown control");
    return api_ok(req);
}

static esp_err_t axis_get(httpd_req_t *req) {
    char a[16], v[12];
    if (!qs_value(req, "a", a, sizeof(a)) || !qs_value(req, "v", v, sizeof(v)))
        return api_bad(req, "missing a/v");
    if (!talon_set_control(a, atoi(v))) return api_bad(req, "unknown control");
    return api_ok(req);
}

static esp_err_t press_get(httpd_req_t *req) {
    char b[16], ms[12];
    if (!qs_value(req, "b", b, sizeof(b))) return api_bad(req, "missing b");
    int hold = qs_value(req, "ms", ms, sizeof(ms)) ? atoi(ms) : 120;
    if (hold < 20) hold = 20;
    if (hold > 5000) hold = 5000;
    if (!talon_set_control(b, 1)) return api_bad(req, "unknown control");
    vTaskDelay(pdMS_TO_TICKS(hold));
    talon_set_control(b, 0);
    return api_ok(req);
}

static esp_err_t release_get(httpd_req_t *req) {
    talon_reset_controls();
    return api_ok(req);
}

static esp_err_t status_get(httpd_req_t *req) {
    char ip[16] = "";
    int rssi = 0;
    wifi_net_up(ip, &rssi);
    uint16_t rl, rr;
    talon_get_rumble(&rl, &rr);
    uint8_t rep[20];
    talon_report_build(rep);

    char body[320];
    snprintf(body, sizeof(body),
        "{\"mounted\":%d,\"ip\":\"%s\",\"rssi\":%d,"
        "\"in_ok\":%lu,\"in_err\":%lu,\"rumble_pkts\":%lu,"
        "\"open\":%lu,\"reset\":%lu,\"ctrl_xid\":%lu,"
        "\"rumble_l\":%u,\"rumble_r\":%u,"
        "\"digital\":%u,\"free_heap\":%u}",
        tud_mounted() ? 1 : 0, ip, rssi,
        (unsigned long)g_xid_in_ok, (unsigned long)g_xid_in_err,
        (unsigned long)g_xid_out_pkts,
        (unsigned long)g_xid_open, (unsigned long)g_xid_reset,
        (unsigned long)g_xid_ctrl_xid,
        rl, rr, rep[2], (unsigned)esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

void webui_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET, .handler = root_get },
        { .uri = "/api/btn",     .method = HTTP_GET, .handler = btn_get },
        { .uri = "/api/axis",    .method = HTTP_GET, .handler = axis_get },
        { .uri = "/api/press",   .method = HTTP_GET, .handler = press_get },
        { .uri = "/api/release", .method = HTTP_GET, .handler = release_get },
        { .uri = "/api/status",  .method = HTTP_GET, .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(srv, &uris[i]);
    ESP_LOGI(TAG, "web UI up on port %d", cfg.server_port);
}
