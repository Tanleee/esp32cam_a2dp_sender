#include "app_controller.h"
#include "uart_link.h"
#include "mic_capture.h"
#include "ai_client.h"
#include "wifi_mgr.h"
#include "bt_a2dp_source.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "APP_CTRL";

#define REC_SAMPLE_RATE   8000
#define REC_MAX_SECONDS   4
#define REC_MAX_BYTES     (REC_SAMPLE_RATE * REC_MAX_SECONDS * 2)

static QueueHandle_t s_cmd_queue;

// Cap phat 1 LAN DUY NHAT, trong app_controller_init() - CANG SOM CANG TOT
// (truoc khi Bluetooth/WiFi khoi dong) de heap con nguyen ven/it bi phan manh nhat.
static uint8_t *s_rec_buf = NULL;

void app_controller_handle_rec_start(const char *playlist_csv)
{
    char *copy = strdup(playlist_csv);
    if (copy) xQueueSend(s_cmd_queue, &copy, 0);
}

// ESP32-C3 gui REC_STOP khi nguoi dung bam dung ghi am thu cong giua chung.
void app_controller_handle_rec_stop(void)
{
    mic_capture_request_stop();
}

static void controller_task(void *arg)
{
    char *playlist;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &playlist, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGI(TAG, "Nhan REC_START, playlist: %s", playlist);
        uart_link_send_ack();

        bt_a2dp_source_set_muted(true);
        size_t rec_len = mic_capture_record(s_rec_buf, REC_MAX_BYTES);
        bt_a2dp_source_set_muted(false);

        if (rec_len == 0) {
            uart_link_send_error("NO_SPEECH");
            free(playlist);
            continue;
        }

        if (!wifi_mgr_is_connected()) {
            uart_link_send_error("WIFI_FAIL");
            free(playlist);
            continue;
        }

        char song[256] = {0};
        esp_err_t ai_ret = ai_client_recommend_song(s_rec_buf, rec_len, REC_SAMPLE_RATE,
                                                      playlist, song, sizeof(song));
        free(playlist);

        if (ai_ret == ESP_OK && song[0] != '\0') {
            uart_link_send_result(song);
        } else {
            uart_link_send_error("AI_FAIL");
        }
    }
}

esp_err_t app_controller_init(void)
{
    s_rec_buf = malloc(REC_MAX_BYTES);
    if (!s_rec_buf) {
        ESP_LOGE(TAG, "Khong cap phat duoc buffer ghi am (%d byte)", REC_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(2, sizeof(char *));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    // Khong con ESP_ERROR_CHECK o day nua (truoc day se abort() ca chuong
    // trinh neu mic loi) - tra ve loi de app_main tiep tuc khoi tao cac module
    // con lai va van bao duoc trang thai qua INIT_DONE.
    esp_err_t ret = mic_capture_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao mic_capture that bai: %s", esp_err_to_name(ret));
        return ret;
    }

    // uart_link_init() KHONG con duoc goi o day nua - da doi sang main.c de
    // gom chung voi callback cua system_ctrl/backlight_ctrl (xem main.c).
    BaseType_t ok = xTaskCreate(controller_task, "app_ctrl", 8192, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}