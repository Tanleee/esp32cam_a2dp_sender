#include "system_ctrl.h"
#include "uart_link.h"
#include "wifi_mgr.h"
#include "bt_a2dp_source.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "SYS_CTRL";

#define TIME_BROADCAST_INTERVAL_MS   30000   // chi can do "dong ho tuong doi", khong can tung giay
#define WIFI_SCAN_MAX_AP              16

// ================== CAU HINH LAI uart_link ==================

void system_ctrl_handle_bt_connect(void)
{
    bt_a2dp_source_connect();
}

void system_ctrl_handle_bt_disconnect(void)
{
    bt_a2dp_source_disconnect();
}

void system_ctrl_handle_wifi_scan_req(void)
{
    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * WIFI_SCAN_MAX_AP);
    if (!records) {
        uart_link_send_wifi_err("SCAN_FAIL");
        return;
    }

    int n = wifi_mgr_scan(records, WIFI_SCAN_MAX_AP);
    if (n <= 0) {
        free(records);
        uart_link_send_wifi_err("SCAN_FAIL");
        return;
    }

    // "ssid1|0,ssid2|1,..." (0 = mang mo, 1 = mang co mat khau)
    size_t csv_cap = (size_t)n * 40 + 8;
    char *csv = malloc(csv_cap);
    if (!csv) {
        free(records);
        uart_link_send_wifi_err("SCAN_FAIL");
        return;
    }

    size_t off = 0;
    csv[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (records[i].ssid[0] == '\0') continue;   // bo qua SSID an/rong
        int secure = (records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
        int w = snprintf(csv + off, csv_cap - off, "%s%s|%d",
                          (off > 0) ? "," : "", (const char *)records[i].ssid, secure);
        if (w < 0 || (size_t)w >= csv_cap - off) break;
        off += (size_t)w;
    }

    uart_link_send_wifi_list(csv);
    free(csv);
    free(records);
}

void system_ctrl_handle_wifi_connect(const char *ssid, const char *password)
{
    esp_err_t ret = wifi_mgr_connect(ssid, password);
    if (ret != ESP_OK) {
        uart_link_send_wifi_err("CONNECT_FAIL");
    }
    // Ket noi thanh cong thi wifi_mgr tu bao qua callback trang thai (da dang
    // ky trong system_ctrl_init() ben duoi) -> uart_link_send_wifi_status(true, ssid).
}

// ================== TRANG THAI WIFI/BLUETOOTH -> UART ==================

static void on_wifi_status(bool connected, const char *ssid)
{
    uart_link_send_wifi_status(connected, ssid);
}

static void on_bt_status(bool connected, const char *device_name)
{
    uart_link_send_bt_status(connected, device_name);
}

// ================== DONG BO THOI GIAN (SNTP) + DINH KY GUI UART ==================

static void time_sync_task(void *arg)
{
    // SNTP can mang de phan giai DNS + gui/nhan goi NTP -> cho WiFi ket noi truoc.
    while (!wifi_mgr_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // UTC+7, Viet Nam, khong co gio mua he. Dinh dang POSIX TZ: dau "-" vi quy
    // uoc TZ lay dau NGUOC voi huong Dong/Tay thuc te.
    setenv("TZ", "ICT-7", 1);
    tzset();

	esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
	esp_netif_sntp_init(&cfg);

    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "Dong bo thoi gian (SNTP) that bai, thu lai sau 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "Da dong bo thoi gian tu may chu NTP");

    // Tu day ve sau, dinh ky gui "gio:phut" + "ngay thang" cho ESP32-C3. KHONG
    // can tu dong bo lai thu cong - esp_netif_sntp da tu dinh ky dong bo lai
    // ngam trong nen (chi can doc time(NULL) moi vong lap).
    while (1) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char hhmm[8];
        char date_str[24];
        strftime(hhmm, sizeof(hhmm), "%H:%M", &tm_info);
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y", &tm_info);

        uart_link_send_time(hhmm, date_str);
        vTaskDelay(pdMS_TO_TICKS(TIME_BROADCAST_INTERVAL_MS));
    }
}

esp_err_t system_ctrl_init(void)
{
    wifi_mgr_set_status_callback(on_wifi_status);
    bt_a2dp_source_set_status_callback(on_bt_status);

    BaseType_t ok = xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
