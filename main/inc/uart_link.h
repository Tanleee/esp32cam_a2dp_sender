#pragma once
#include "esp_err.h"
#include <stdbool.h>

// --- Nhan tu ESP32-C3 ---
typedef void (*uart_link_rec_start_cb_t)(const char *playlist_csv);
typedef void (*uart_link_rec_stop_cb_t)(void);
typedef void (*uart_link_simple_cb_t)(void);   // BT_CONNECT / BT_DISCONNECT / WIFI_SCAN_REQ
typedef void (*uart_link_wifi_connect_cb_t)(const char *ssid, const char *password);

typedef struct {
    uart_link_rec_start_cb_t    on_rec_start;      // REC_START:<csv>
    uart_link_rec_stop_cb_t     on_rec_stop;        // REC_STOP
    uart_link_simple_cb_t       on_bt_connect;       // BT_CONNECT     - C3 yeu cau ket noi Bluetooth
    uart_link_simple_cb_t       on_bt_disconnect;    // BT_DISCONNECT  - C3 yeu cau ngat Bluetooth
    uart_link_simple_cb_t       on_wifi_scan_req;    // WIFI_SCAN_REQ  - C3 mo man hinh WiFi, can danh sach mang
    uart_link_wifi_connect_cb_t on_wifi_connect;     // WIFI_CONNECT:<ssid>:<password>
} uart_link_callbacks_t;

// Dang ky callback va khoi dong UART. CHI goi 1 LAN DUY NHAT trong toan bo
// chuong trinh (hien dang goi tu app_controller_init(), gom ca callback cua
// tinh nang AI lan cac callback he thong moi tro den system_ctrl.c).
esp_err_t uart_link_init(const uart_link_callbacks_t *callbacks);

// --- Gui ve ESP32-C3 (tinh nang AI, da co tu truoc) ---
void uart_link_send_ack(void);
void uart_link_send_result(const char *song_name);
void uart_link_send_error(const char *code);

// --- Gui ve ESP32-C3 (dieu khien am luong, da co tu truoc) ---
void uart_link_send_vol_up(void);
void uart_link_send_vol_down(void);

// --- Gui ve ESP32-C3 (tinh nang moi: dong ho + trang thai WiFi/Bluetooth) ---

// hhmm dang "HH:MM", date_str dang "Thu, 23 Oct 2026" (da dinh dang san, C3
// chi hien thi nguyen van, khong tu tinh toan lich).
void uart_link_send_time(const char *hhmm, const char *date_str);

// ssid co the rong ("") khi connected = false.
void uart_link_send_wifi_status(bool connected, const char *ssid);

// device_name co the rong ("") khi connected = false.
void uart_link_send_bt_status(bool connected, const char *device_name);

// networks_csv dang "ssid1|0,ssid2|1,..." (0 = mang mo, 1 = mang co mat khau).
// LUU Y: neu ban ten SSID chua dau phay hoac dau gach dung "|" thi dong nay se
// bi tach sai - truong hop hiem gap, chap nhan duoc trong pham vi du an nay.
void uart_link_send_wifi_list(const char *networks_csv);

// code vi du: "SCAN_FAIL", "CONNECT_FAIL"
void uart_link_send_wifi_err(const char *code);
