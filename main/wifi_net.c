// Talon — WiFi station bring-up (credentials from the gitignored wifi_creds.h).
// Started only after the Xbox has enumerated the controller: Falcon (the sibling
// camera emulator) showed the radio's CPU/current load can disturb the Xbox's
// timing-strict USB enumeration, so USB always comes up first.
#include <string.h>
#include "wifi_net.h"
#include "wifi_creds.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "talon.wifi";

static volatile bool s_up;
static char s_ip[16];

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_up = true;
        ESP_LOGI(TAG, "wifi up — web UI at http://%s/", s_ip);
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
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, TALON_WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, TALON_WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    // No modem power-save: input latency matters more than the few mA saved,
    // and MIN_MODEM dozing has caused minute-scale stalls on sibling projects.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining ssid=%s", TALON_WIFI_SSID);
}
