#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "i2s_slave_rx.h"
#include "bt_a2dp_source.h"
#include "wifi_mgr.h"
#include "app_controller.h"
#include "system_ctrl.h"
#include "volume_button.h"

static const char *TAG = "MAIN";

#define RINGBUF_SIZE   (16 * 1024)

#define WIFI_SSID   "C427"
#define WIFI_PASS   "64546743"

void app_main(void)
{
    esp_log_level_set("BT_L2CAP", ESP_LOG_NONE);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    // app_controller_init() cap phat buffer ghi am AI (96000 byte) NGAY TU DAU,
    // truoc Bluetooth/WiFi, luc heap con nguyen ven/it phan manh nhat - tranh
    // loi "khong cap phat duoc buffer ghi am" tung gap khi malloc() nay bi goi
    // sau khi BT + WiFi da chiem/phan manh DRAM noi bo.
    if (app_controller_init() != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao app_controller that bai");
        return;
    }

    RingbufHandle_t ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!ringbuf) {
        ESP_LOGE(TAG, "Tao ring buffer that bai");
        return;
    }

    if (i2s_slave_rx_init(ringbuf) != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao I2S RX that bai");
        return;
    }

    // Chi CAU HINH A2DP source o day - KHONG tu quet/ket noi Bluetooth nua.
    // Viec quet/ket noi chi bat dau khi ESP32-C3 gui lenh BT_CONNECT qua UART
    // (xem system_ctrl.c / bt_a2dp_source_connect()).
    if (bt_a2dp_source_init(ringbuf) != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao Bluetooth A2DP that bai");
        return;
    }

    wifi_mgr_init_and_connect(WIFI_SSID, WIFI_PASS);

    // Dong bo gio qua SNTP (sau khi WiFi ket noi) + chuyen tiep trang thai
    // WiFi/Bluetooth qua UART cho ESP32-C3.
    if (system_ctrl_init() != ESP_OK) {
        ESP_LOGW(TAG, "Khoi tao system_ctrl that bai");
    }

    // 2 nut nhan tang/giam am luong (chuyen tu ESP32-C3 sang board nay),
    // gui lenh VOL_UP/VOL_DOWN qua UART cho C3 tu dieu chinh volume.
    if (volume_button_init() != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao volume_button that bai");
    }
}
