#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "i2s_slave_rx.h"
#include "bt_a2dp_source.h"
#include "wifi_mgr.h"
#include "app_controller.h"
#include "system_ctrl.h"
#include "volume_button.h"
#include "backlight_ctrl.h"
#include "uart_link.h"
#include "net_check.h"
#include "remote_link.h"
#include "web_server.h"

#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "MAIN";

#define RINGBUF_SIZE   (16 * 1024)

void app_main(void)
{
    esp_log_level_set("BT_L2CAP", ESP_LOG_NONE);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    // --- UART phai san sang TRUOC TIEN de co the gui duoc INIT_DONE ve sau.
    // uart_link_init() CHI duoc goi 1 LAN DUY NHAT trong toan bo chuong trinh,
    // nay doi sang goi tu day (thay vi tu app_controller_init() nhu truoc) vi
    // can gom callback cua CA 3 module: app_controller (AI) / system_ctrl
    // (WiFi+Bluetooth+gio) / backlight_ctrl (do sang man hinh, MOI). ---
    uart_link_callbacks_t cbs = {
        .on_rec_start       = app_controller_handle_rec_start,
        .on_rec_stop        = app_controller_handle_rec_stop,
        .on_bt_enable       = system_ctrl_handle_bt_enable,
        .on_bt_disable      = system_ctrl_handle_bt_disable,
        .on_bt_scan_req     = system_ctrl_handle_bt_scan_req,
        .on_bt_connect      = system_ctrl_handle_bt_connect,
        .on_bt_disconnect   = system_ctrl_handle_bt_disconnect,
        .on_bt_forget       = system_ctrl_handle_bt_forget,
        .on_net_check       = net_check_handle_request,
        .on_wifi_scan_req   = system_ctrl_handle_wifi_scan_req,
        .on_wifi_connect    = system_ctrl_handle_wifi_connect,
        .on_wifi_enable     = system_ctrl_handle_wifi_enable,
        .on_wifi_pw_query   = system_ctrl_handle_wifi_pw_query,
        .on_wifi_forget     = system_ctrl_handle_wifi_forget,
        .on_brightness_set  = backlight_ctrl_handle_set,
    };
    // MOI: cau noi web <-> C3 phai san sang TRUOC khi UART bat dau nhan dong tu C3.
    if (remote_link_init() != ESP_OK) ESP_LOGE(TAG, "Khoi tao remote_link that bai");
    bool uart_ok = (uart_link_init(&cbs) == ESP_OK);
    if (!uart_ok) {
        // Khong co UART thi khong co cach nao bao trang thai ve C3 - dung lai.
        ESP_LOGE(TAG, "Khoi tao UART that bai, dung chuong trinh");
        return;
    }

    // --- Tu day ve sau: khoi tao TAT CA cac module, du thanh cong hay that
    // bai van tiep tuc sang module ke tiep (KHONG return som nhu truoc), de
    // luc nao cung gui duoc INIT_DONE voi trang thai cuoi cung cua tat ca. ---

    // app_controller_init() cap phat buffer ghi am AI (96000 byte) o day,
    // truoc Bluetooth/WiFi, luc heap con nguyen ven/it phan manh nhat.
    bool app_ctrl_ok = (app_controller_init() == ESP_OK);
    if (!app_ctrl_ok) ESP_LOGE(TAG, "Khoi tao app_controller that bai");

    RingbufHandle_t ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    bool ringbuf_ok = (ringbuf != NULL);
    if (!ringbuf_ok) ESP_LOGE(TAG, "Tao ring buffer that bai");

    bool i2s_rx_ok = ringbuf_ok && (i2s_slave_rx_init(ringbuf) == ESP_OK);
    if (ringbuf_ok && !i2s_rx_ok) ESP_LOGE(TAG, "Khoi tao I2S RX that bai");

    // Chi CAU HINH A2DP source o day (va nap danh sach thiet bi da luu tu NVS) -
    // Bluetooth mac dinh TAT. Chi bat + quet khi ESP32-C3 gui BT_ENABLE qua UART.
    bool bt_ok = ringbuf_ok && (bt_a2dp_source_init(ringbuf) == ESP_OK);
    if (ringbuf_ok && !bt_ok) ESP_LOGE(TAG, "Khoi tao Bluetooth A2DP that bai");

    // MOI: task kiem tra Internet that su (tra loi lenh NET_CHECK tu C3).
    bool net_chk_ok = (net_check_init() == ESP_OK);
    if (!net_chk_ok) ESP_LOGE(TAG, "Khoi tao net_check that bai");

    // Khoi tao driver WiFi (STA mode) nhung CHUA bat radio - WiFi mac dinh TAT.
    // Radio chi duoc bat khi nhan WIFI_ENABLE tu ESP32-C3, va chi ket noi khi
    // nhan WIFI_CONNECT (xem system_ctrl.c). wifi=1 trong INIT_DONE nghia la
    // driver da init thanh cong.
    bool wifi_ok = (wifi_mgr_init() == ESP_OK);
    if (!wifi_ok) ESP_LOGE(TAG, "Khoi tao WiFi driver that bai");

    // Dong bo gio qua SNTP (sau khi WiFi ket noi) + chuyen tiep trang thai
    // WiFi/Bluetooth qua UART cho ESP32-C3 + worker xu ly yeu cau ket noi WiFi.
    bool sys_ctrl_ok = (system_ctrl_init() == ESP_OK);
    if (!sys_ctrl_ok) ESP_LOGW(TAG, "Khoi tao system_ctrl that bai");

    // 2 nut nhan tang/giam am luong (chuyen tu ESP32-C3 sang board nay).
    bool vol_btn_ok = (volume_button_init() == ESP_OK);
    if (!vol_btn_ok) ESP_LOGE(TAG, "Khoi tao volume_button that bai");

    // MOI: web server (giao dien dieu khien + API) va mDNS "musicplayer.local". Chi lang nghe
    // khi WiFi co IP; WiFi mac dinh TAT nen web chi vao duoc sau khi bat WiFi tren man hinh C3.
    bool web_ok = (web_server_start() == ESP_OK);
    if (!web_ok) ESP_LOGE(TAG, "Khoi tao web server that bai");

    // MOI: PWM do sang man hinh (GPIO14), dieu khien qua lenh BRIGHTNESS tu C3.
    bool backlight_ok = (backlight_ctrl_init() == ESP_OK);
    if (!backlight_ok) ESP_LOGE(TAG, "Khoi tao backlight_ctrl that bai");

    // --- Tat ca da xong (thanh cong hay that bai) -> bao ve cho ESP32-C3. ---
    char status[160];
    snprintf(status, sizeof(status),
             "app_ctrl=%d,ringbuf=%d,i2s_rx=%d,bt=%d,wifi=%d,sys_ctrl=%d,vol_btn=%d,backlight=%d,net_chk=%d,web=%d",
             app_ctrl_ok, ringbuf_ok, i2s_rx_ok, bt_ok, wifi_ok, sys_ctrl_ok, vol_btn_ok, backlight_ok, net_chk_ok, web_ok);
    uart_link_send_init_status(status);

    // DEBUG: heap sau khi khoi tao xong (chua bat Bluetooth) - so sanh voi dong
    // "Heap truoc khi bat BT" trong system_ctrl.c.
	ESP_LOGI(TAG, "Heap sau khoi tao: internal free=%u largest=%u | psram free=%u",
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}