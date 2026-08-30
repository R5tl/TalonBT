// Talon — over-the-air firmware update. A POST of a raw talon.bin to /api/ota
// is streamed straight into the inactive OTA slot; on success Talon reboots into
// it, and the bootloader rolls back if the new image doesn't confirm itself.
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bt_host.h"
#include "led_status.h"

void xid_ota_quiesce(void);

static const char *TAG = "talon.ota";

// Mark the running image valid once we're up, so a good boot cancels the
// rollback the previous OTA armed. Called from app_main after services start.
void ota_mark_valid(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "running image confirmed valid (rollback cancelled)");
    }
}

// POST /api/ota — body is the raw firmware binary.
static esp_err_t ota_post(httpd_req_t *req) {
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA start -> %s (%d bytes incoming)", dst->label, req->content_len);

    // Quiesce everything that could call a blocking FreeRTOS primitive from an
    // ISR during a flash write (which disables cache + suspends the scheduler):
    //   * USB SOF — the confirmed offender: the dwc2 SOF ISR arms an endpoint
    //     via a mutex; a SOF in the flash-write window asserts.
    //   * LED RMT task and the BT controller — defensive, both do blocking waits.
    // The device reboots after OTA, so none of this needs restoring.
    xid_ota_quiesce();
    led_status_stop();
    bt_host_stop();

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }

    char buf[1460];
    int remaining = req->content_len, total = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            return ESP_FAIL;
        }
        total += r;
        remaining -= r;
    }

    if (esp_ota_end(h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image invalid (bad magic/size)");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(dst) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA done (%d bytes) -> booting %s", total, dst->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"rebooting into new firmware\"}");

    // Let the reply flush, then reboot into the new slot.
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

void ota_register(httpd_handle_t srv) {
    static const httpd_uri_t u = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(srv, &u);
}
