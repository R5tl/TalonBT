// Talon — WiFi manager, modeled on Kratos's network_manager:
//   * STA with credentials in NVS (seeded once from wifi_creds.h if present).
//   * No credentials -> open SoftAP "Talon-Setup" with a captive-portal DNS
//     (every A query answered with the AP IP) so the setup page pops up.
//   * WPS push-button join (120 s); negotiated credentials are persisted.
//   * mDNS: http://talon.local/ (plus the DHCP hostname for router UIs).
//
// USB always comes up before this runs: Falcon (the sibling camera emulator)
// showed radio bring-up can disturb the Xbox's timing-strict USB enumeration.
#include <string.h>
#include <stdio.h>
#include "wifi_net.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#if defined(__has_include)
#  if __has_include("wifi_creds.h")
#    include "wifi_creds.h"     // optional compile-time seed for a fresh flash
#  endif
#endif

static const char *TAG = "talon.wifi";

#define NVS_NS       "talon"
#define WPS_TIMEOUT_S 120

static esp_netif_t *s_sta_netif, *s_ap_netif;
static volatile talon_net_mode_t  s_mode = TALON_NET_STA;
static volatile talon_wps_state_t s_wps_state = TALON_WPS_IDLE;
static int64_t s_wps_start_us;
static volatile bool s_up;
static volatile bool s_ever_connected;    // got an IP at least once this boot
static char s_ip[16];
static char s_ssid[33], s_pass[65];
static esp_timer_handle_t s_apply_timer;
static esp_timer_handle_t s_fallback_timer;

// If the stored credentials never connect within this long, fall back to the
// SoftAP setup portal so wrong/stale settings don't loop forever. Only applies
// before the first successful connection — later drops keep retrying STA.
#define STA_FALLBACK_MS 45000

// ---- credential store ------------------------------------------------------

static void creds_load(void) {
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_ssid);
        nvs_get_str(h, "ssid", s_ssid, &n);
        n = sizeof(s_pass);
        nvs_get_str(h, "pass", s_pass, &n);
        nvs_close(h);
    }
#if defined(TALON_WIFI_SSID)
    if (s_ssid[0] == '\0') {
        // First boot with compiled-in seed credentials: adopt and persist them.
        strlcpy(s_ssid, TALON_WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, TALON_WIFI_PASS, sizeof(s_pass));
        if (s_ssid[0]) {
            nvs_handle_t w;
            if (nvs_open(NVS_NS, NVS_READWRITE, &w) == ESP_OK) {
                nvs_set_str(w, "ssid", s_ssid);
                nvs_set_str(w, "pass", s_pass);
                nvs_commit(w);
                nvs_close(w);
                ESP_LOGI(TAG, "seeded credentials from wifi_creds.h (ssid=%s)", s_ssid);
            }
        }
    }
#endif
}

static void creds_store(const char *ssid, const char *pass) {
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", s_ssid);
        nvs_set_str(h, "pass", s_pass);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void creds_erase(void) {
    s_ssid[0] = s_pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
}

bool wifi_net_has_creds(void) { return s_ssid[0] != '\0'; }
const char *wifi_net_ssid(void) { return s_ssid; }
talon_net_mode_t wifi_net_mode(void) { return s_mode; }
talon_wps_state_t wifi_net_wps_state(void) { return s_wps_state; }

int wifi_net_wps_remaining(void) {
    if (s_wps_state != TALON_WPS_CONNECTING) return 0;
    int elapsed = (int)((esp_timer_get_time() - s_wps_start_us) / 1000000);
    return elapsed >= WPS_TIMEOUT_S ? 0 : WPS_TIMEOUT_S - elapsed;
}

// ---- mDNS ------------------------------------------------------------------

static void start_mdns(void) {
    static bool inited;
    if (!inited) {
        if (mdns_init() != ESP_OK) return;
        inited = true;
        mdns_hostname_set(TALON_HOSTNAME);
        mdns_instance_name_set("Talon Xbox Controller");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS up: http://" TALON_HOSTNAME ".local/");
    }
}

// ---- captive-portal DNS (port of Kratos's native task) ---------------------
// Answers every A query with the SoftAP IP so joining the setup AP pops the
// portal open on phones (Android /generate_204, iOS hotspot-detect, ...).

static volatile int s_dns_sock = -1;
static TaskHandle_t s_dns_task;
static uint32_t s_ap_ip_n;              // AP IP, network byte order

static void captive_dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { s_dns_task = NULL; vTaskDelete(NULL); return; }

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_dns_sock = sock;

    uint8_t buf[512];
    while (s_dns_sock >= 0) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (len < 12) continue;

        // Turn the query into a response carrying one A answer -> the AP IP.
        buf[2] = 0x81; buf[3] = 0x80;             // QR=1, RA=1
        buf[6] = 0x00; buf[7] = 0x01;             // ANCOUNT = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;  // NSCOUNT/ARCOUNT = 0

        int p = 12;                               // walk the question name
        while (p < len && buf[p] != 0) p += buf[p] + 1;
        p += 1 + 4;                               // null label + QTYPE + QCLASS
        if (p + 16 > (int)sizeof(buf)) continue;

        buf[p++] = 0xC0; buf[p++] = 0x0C;         // name pointer to the question
        buf[p++] = 0x00; buf[p++] = 0x01;         // TYPE A
        buf[p++] = 0x00; buf[p++] = 0x01;         // CLASS IN
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C; // TTL 60
        buf[p++] = 0x00; buf[p++] = 0x04;         // RDLENGTH 4
        memcpy(&buf[p], &s_ap_ip_n, 4); p += 4;

        sendto(sock, buf, p, 0, (struct sockaddr *)&src, slen);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void captive_dns_start(uint32_t ap_ip_n) {
    if (s_dns_task) return;
    s_ap_ip_n = ap_ip_n;
    xTaskCreate(captive_dns_task, "captdns", 3072, NULL, tskIDLE_PRIORITY + 1, &s_dns_task);
}

static void captive_dns_stop(void) {
    int s = s_dns_sock;
    s_dns_sock = -1;                // signals the task loop to exit
    if (s >= 0) shutdown(s, SHUT_RDWR);
}

// ---- mode transitions ------------------------------------------------------

// Arm the "creds never worked -> AP setup" fallback, unless we've already had a
// good connection this boot (then transient drops should just keep retrying).
static void arm_fallback(void) {
    if (s_ever_connected) return;
    esp_timer_stop(s_fallback_timer);
    esp_timer_start_once(s_fallback_timer, (uint64_t)STA_FALLBACK_MS * 1000);
}

static void fallback_timer_cb(void *arg) {
    (void)arg;
    if (!s_up && s_mode == TALON_NET_STA && !s_ever_connected) {
        ESP_LOGW(TAG, "no connection in %d s — stored WiFi looks invalid, entering setup AP",
                 STA_FALLBACK_MS / 1000);
        wifi_net_enter_setup();
    }
}

static void begin_sta(void) {
    s_mode = TALON_NET_STA;
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    arm_fallback();
    ESP_LOGI(TAG, "joining ssid=%s", s_ssid);
}

void wifi_net_enter_setup(void) {
    if (s_wps_state == TALON_WPS_CONNECTING) {
        esp_wifi_wps_disable();
        s_wps_state = TALON_WPS_IDLE;
    }
    s_up = false;
    s_mode = TALON_NET_AP_SETUP;
    esp_wifi_disconnect();

    // APSTA (not bare AP) so the setup page's network scan still works.
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, TALON_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(TALON_AP_SSID);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(s_ap_netif, &ip);
    captive_dns_start(ip.ip.addr);
    start_mdns();
    ESP_LOGI(TAG, "setup AP '%s' up at " IPSTR, TALON_AP_SSID, IP2STR(&ip.ip));
}

static void leave_setup(void) {
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);   // drops the SoftAP
}

// Deferred credential apply, so the HTTP "saved" reply reaches the phone while
// the SoftAP is still up (the Kratos trick).
static void apply_timer_cb(void *arg) {
    (void)arg;
    if (s_mode == TALON_NET_AP_SETUP) leave_setup();
    begin_sta();
}

void wifi_net_save_creds(const char *ssid, const char *pass) {
    creds_store(ssid, pass);
    ESP_LOGI(TAG, "credentials saved (ssid=%s), joining in 1.5s", s_ssid);
    esp_timer_stop(s_apply_timer);
    esp_timer_start_once(s_apply_timer, 1500 * 1000);
}

void wifi_net_forget(void) {
    creds_erase();
    ESP_LOGI(TAG, "credentials erased — entering setup mode");
    if (s_mode == TALON_NET_AP_SETUP) return;
    wifi_net_enter_setup();
}

void wifi_net_start_wps(void) {
    if (s_wps_state == TALON_WPS_CONNECTING) return;
    if (s_mode == TALON_NET_AP_SETUP) leave_setup();
    s_mode = TALON_NET_WPS;
    s_up = false;
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_STA);

    esp_wps_config_t cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
    if (esp_wifi_wps_enable(&cfg) != ESP_OK || esp_wifi_wps_start(0) != ESP_OK) {
        ESP_LOGE(TAG, "WPS start failed");
        esp_wifi_wps_disable();
        s_wps_state = TALON_WPS_FAILED;
        if (wifi_net_has_creds()) begin_sta();
        else wifi_net_enter_setup();
        return;
    }
    s_wps_state = TALON_WPS_CONNECTING;
    s_wps_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WPS push-button negotiation started (%d s window)", WPS_TIMEOUT_S);
}

// ---- scan ------------------------------------------------------------------

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = (*in >= 0x20) ? *in : ' ';
    }
    out[o] = '\0';
}

int wifi_net_scan_json(char *out, size_t cap) {
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        strlcpy(out, "[]", cap);
        return 0;
    }
    uint16_t n = 20;
    static wifi_ap_record_t recs[20];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;

    size_t o = 0;
    o += snprintf(out + o, cap - o, "[");
    int written = 0;
    for (int i = 0; i < n && o + 96 < cap; i++) {
        if (recs[i].ssid[0] == '\0') continue;   // hidden
        char esc[80];
        json_escape((const char *)recs[i].ssid, esc, sizeof(esc));
        o += snprintf(out + o, cap - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%d}",
                      written ? "," : "", esc, recs[i].rssi,
                      recs[i].authmode == WIFI_AUTH_OPEN ? 1 : 0);
        written++;
    }
    snprintf(out + o, cap - o, "]");
    return written;
}

// ---- events ----------------------------------------------------------------

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_mode == TALON_NET_STA && wifi_net_has_creds()) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_up = false;
            // Reconnect only in plain STA mode: during WPS the supplicant owns
            // the connection, and in setup mode the STA is down on purpose.
            if (s_mode == TALON_NET_STA && wifi_net_has_creds()) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_WPS_ER_SUCCESS: {
            // WPS negotiated credentials. Multi-AP responses carry them in the
            // event; the single-AP flow leaves them in the STA config.
            wifi_event_sta_wps_er_success_t *evt = (wifi_event_sta_wps_er_success_t *)data;
            if (evt && evt->ap_cred_cnt > 0) {
                wifi_config_t wc = { 0 };
                memcpy(wc.sta.ssid, evt->ap_cred[0].ssid, sizeof(wc.sta.ssid));
                memcpy(wc.sta.password, evt->ap_cred[0].passphrase, sizeof(wc.sta.password));
                esp_wifi_set_config(WIFI_IF_STA, &wc);
            }
            esp_wifi_wps_disable();
            ESP_LOGI(TAG, "WPS success — connecting");
            s_mode = TALON_NET_STA;
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_STA_WPS_ER_FAILED:
        case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
            ESP_LOGW(TAG, "WPS %s", id == WIFI_EVENT_STA_WPS_ER_TIMEOUT ? "timed out" : "failed");
            esp_wifi_wps_disable();
            s_wps_state = TALON_WPS_FAILED;
            if (wifi_net_has_creds()) begin_sta();
            else wifi_net_enter_setup();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_up = true;
        s_ever_connected = true;
        esp_timer_stop(s_fallback_timer);       // creds are good; cancel AP fallback
        if (s_wps_state == TALON_WPS_CONNECTING) {
            // Persist what WPS negotiated so the next boot joins directly.
            wifi_config_t wc = { 0 };
            esp_wifi_get_config(WIFI_IF_STA, &wc);
            creds_store((const char *)wc.sta.ssid, (const char *)wc.sta.password);
            s_wps_state = TALON_WPS_CONNECTED;
            ESP_LOGI(TAG, "WPS credentials persisted (ssid=%s)", s_ssid);
        }
        s_mode = TALON_NET_STA;
        start_mdns();
        ESP_LOGI(TAG, "wifi up — web UI at http://%s/ (http://" TALON_HOSTNAME ".local/)", s_ip);
    }
}

bool wifi_net_up(char ip_out[16], int *rssi_out) {
    if (!s_up) return false;
    if (ip_out) strcpy(ip_out, s_ip);
    if (rssi_out) {
        wifi_ap_record_t ap;
        *rssi_out = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    }
    return true;
}

void wifi_net_start(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, TALON_HOSTNAME);   // router UI name

    const esp_timer_create_args_t ta = { .callback = apply_timer_cb, .name = "wifi_apply" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_apply_timer));
    const esp_timer_create_args_t tf = { .callback = fallback_timer_cb, .name = "wifi_fallback" };
    ESP_ERROR_CHECK(esp_timer_create(&tf, &s_fallback_timer));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);

    // No modem power-save: input latency matters more than the few mA saved,
    // and MIN_MODEM dozing has caused minute-scale stalls on sibling projects.
    creds_load();
    if (wifi_net_has_creds()) {
        s_mode = TALON_NET_STA;
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
    } else {
        ESP_LOGI(TAG, "no stored credentials — starting in setup mode");
        s_mode = TALON_NET_AP_SETUP;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());

    // STA connect happens on WIFI_EVENT_STA_START; the AP needs explicit setup.
    if (s_mode == TALON_NET_AP_SETUP) wifi_net_enter_setup();
    else arm_fallback();   // bad stored creds -> setup AP after STA_FALLBACK_MS
}
