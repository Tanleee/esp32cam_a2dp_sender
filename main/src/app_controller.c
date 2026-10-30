#include "app_controller.h"
#include "uart_link.h"
#include "system_ctrl.h"
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

// Cap phat 1 LAN DUY NHAT, trong app_controller_init() - CANG SOM CANG TOT (truoc
// khi Bluetooth/WiFi khoi dong) de heap con nguyen ven/it bi phan manh nhat,
// tranh loi "khong cap phat duoc buffer ghi am" tung gap khi malloc() nay bi
// goi SAU khi BT + WiFi da chiem/phan manh DRAM noi bo.
static uint8_t *s_rec_buf = NULL;

static void on_rec_start(const char *playlist_csv)
{
    char *copy = strdup(playlist_csv);
    if (copy) xQueueSend(s_cmd_queue, &copy, 0);
}

// ESP32-C3 gui REC_STOP khi nguoi dung bam dung ghi am thu cong giua chung
// (thay vi chi cho VAD tu dong phat hien im lang de dung).
static void on_rec_stop(void)
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

        // song co the chua nhieu ten bai hat cach nhau boi dau phay (toi da 4 bai)
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
    // Cap phat buffer ghi am NGAY, truoc het - xem ghi chu o khai bao s_rec_buf.
    s_rec_buf = malloc(REC_MAX_BYTES);
    if (!s_rec_buf) {
        ESP_LOGE(TAG, "Khong cap phat duoc buffer ghi am (%d byte)", REC_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(2, sizeof(char *));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(mic_capture_init());

    // uart_link_init() CHI duoc goi 1 LAN DUY NHAT trong toan bo chuong trinh -
    // gom ca callback cua tinh nang AI (on_rec_start/on_rec_stop) lan cac
    // callback he thong moi (BT_CONNECT/BT_DISCONNECT/WIFI_SCAN_REQ/
    // WIFI_CONNECT), tro ve system_ctrl.c.
    uart_link_callbacks_t cbs = {
        .on_rec_start     = on_rec_start,
        .on_rec_stop      = on_rec_stop,
        .on_bt_connect    = system_ctrl_handle_bt_connect,
        .on_bt_disconnect = system_ctrl_handle_bt_disconnect,
        .on_wifi_scan_req = system_ctrl_handle_wifi_scan_req,
        .on_wifi_connect  = system_ctrl_handle_wifi_connect,
    };
    ESP_ERROR_CHECK(uart_link_init(&cbs));

    xTaskCreate(controller_task, "app_ctrl", 8192, NULL, 5, NULL);
    return ESP_OK;
}
