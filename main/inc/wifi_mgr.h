#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

esp_err_t wifi_mgr_init_and_connect(const char *ssid, const char *password);
bool wifi_mgr_is_connected(void);

// Goi moi khi trang thai ket noi WiFi thay doi (vua ket noi thanh cong / vua
// mat ket noi) - dung de system_ctrl.c day thong tin nay qua UART cho ESP32-C3.
// ssid truyen vao callback rong ("") khi connected = false.
typedef void (*wifi_mgr_status_cb_t)(bool connected, const char *ssid);
void wifi_mgr_set_status_callback(wifi_mgr_status_cb_t cb);

// Quet cac mang WiFi xung quanh (CHAN cho den khi xong, toi da vai giay). Ghi
// toi da max_count mang vao out_records, tra ve so mang tim duoc thuc su.
int wifi_mgr_scan(wifi_ap_record_t *out_records, int max_count);

// Ket noi (hoac doi sang) 1 mang WiFi khac, dung khi nguoi dung chon mang tren
// man hinh WiFi cua ESP32-C3. password co the rong ("") cho mang mo. CHAN cho
// den khi ket noi xong hoac het thoi gian cho (xem timeout trong wifi_mgr.c).
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);
