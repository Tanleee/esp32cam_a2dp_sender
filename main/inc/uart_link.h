#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// --- Nhan tu ESP32-C3 ---
typedef void (*uart_link_rec_start_cb_t)(const char *playlist_csv);
typedef void (*uart_link_rec_stop_cb_t)(void);
typedef void (*uart_link_simple_cb_t)(void);   // BT_ENABLE / BT_DISABLE / BT_SCAN_REQ / BT_DISCONNECT / WIFI_SCAN_REQ
typedef void (*uart_link_wifi_connect_cb_t)(const char *ssid, const char *password);
typedef void (*uart_link_brightness_cb_t)(uint8_t percent);   // BRIGHTNESS:<0-100>, MOI
typedef void (*uart_link_wifi_enable_cb_t)(bool enable);       // WIFI_ENABLE / WIFI_DISABLE, MOI
typedef void (*uart_link_ssid_cb_t)(const char *ssid);         // WIFI_PW_QUERY:<ssid> / WIFI_FORGET:<ssid>, MOI
typedef void (*uart_link_bt_addr_cb_t)(const char *addr);      // BT_CONNECT:<addr> / BT_FORGET:<addr>, MOI (addr = 12 ky tu HEX)

typedef struct {
    uart_link_rec_start_cb_t    on_rec_start;      // REC_START:<csv>
    uart_link_rec_stop_cb_t     on_rec_stop;        // REC_STOP
    uart_link_simple_cb_t       on_bt_enable;        // BT_ENABLE (hoac "BT_CONNECT" tran, ban cu) - bat BT + quet
    uart_link_simple_cb_t       on_bt_disable;       // BT_DISABLE - tat han BT
    uart_link_simple_cb_t       on_bt_scan_req;      // BT_SCAN_REQ - yeu cau day lai danh sach
    uart_link_bt_addr_cb_t      on_bt_connect;       // BT_CONNECT:<addr> - noi toi thiet bi da chon
    uart_link_simple_cb_t       on_bt_disconnect;    // BT_DISCONNECT - chi ngat thiet bi hien tai, BT van bat
    uart_link_bt_addr_cb_t      on_bt_forget;        // BT_FORGET:<addr> - quen thiet bi da luu
    uart_link_simple_cb_t       on_net_check;        // NET_CHECK - MOI
    uart_link_simple_cb_t       on_wifi_scan_req;    // WIFI_SCAN_REQ
    uart_link_wifi_connect_cb_t on_wifi_connect;     // WIFI_CONNECT:<ssid>:<password>
    uart_link_brightness_cb_t   on_brightness_set;   // BRIGHTNESS:<0-100> - MOI
    uart_link_wifi_enable_cb_t  on_wifi_enable;      // WIFI_ENABLE / WIFI_DISABLE - MOI
    uart_link_ssid_cb_t         on_wifi_pw_query;    // WIFI_PW_QUERY:<ssid> - MOI
    uart_link_ssid_cb_t         on_wifi_forget;      // WIFI_FORGET:<ssid> - MOI
} uart_link_callbacks_t;

// Dang ky callback va khoi dong UART. CHI goi 1 LAN DUY NHAT trong toan bo
// chuong trinh - nay goi tu main.c (app_main), gom callback cua ca 3 module
// app_controller / system_ctrl / backlight_ctrl.
esp_err_t uart_link_init(const uart_link_callbacks_t *callbacks);

// --- Gui ve ESP32-C3 (tinh nang AI, da co tu truoc) ---
void uart_link_send_ack(void);
void uart_link_send_result(const char *song_name);
void uart_link_send_error(const char *code);

// MOI: ket qua kiem tra Internet (tra loi cho NET_CHECK tu C3).
//   NET_OK  /  NET_ERR:<WIFI_OFF|NO_WIFI|NO_INTERNET|NO_DNS|NO_SERVER|BUSY>
void uart_link_send_net_ok(void);
void uart_link_send_net_err(const char *code);

// --- Gui ve ESP32-C3 (dieu khien am luong, da co tu truoc) ---
void uart_link_send_vol_up(void);
void uart_link_send_vol_down(void);

// --- Gui ve ESP32-C3 (tinh nang he thong, da co tu truoc) ---
void uart_link_send_time(const char *hhmm, const char *date_str);
void uart_link_send_wifi_status(bool connected, const char *ssid);
void uart_link_send_bt_status(bool connected, const char *device_name);   // BT_STATUS:<0|1>:<ten>
void uart_link_send_wifi_list(const char *networks_csv);
void uart_link_send_wifi_err(const char *code);

// --- Gui ve ESP32-C3 (MOI) ---

// Bao hoan tat khoi tao toan bo he thong (goi 1 lan, cuoi app_main) kem trang
// thai tung module, dang "key1=0|1,key2=0|1,...". Vi du:
// "app_ctrl=1,ringbuf=1,i2s_rx=1,bt=1,wifi=1,sys_ctrl=1,vol_btn=1,backlight=1"
void uart_link_send_init_status(const char *status_csv);

// Bao ket qua xu ly yeu cau WIFI_CONNECT khi dang co 1 lan xac thuc wifi KHAC
// con dang cho: switched_immediately=true neu da huy duoc tien trinh cu bang
// code va chuyen ngay sang mang moi (hien tai luon la true, xem system_ctrl.c).
void uart_link_send_wifi_switch(bool switched_immediately);

// Tu choi 1 yeu cau vi dang co thao tac ket noi KHAC (WiFi hoac Bluetooth)
// dang xac thuc do xung dot radio. reason vi du "WIFI_CONNECTING"/"BT_CONNECTING".
void uart_link_send_busy(const char *reason);

// --- Gui ve ESP32-C3 (MOI: bat/tat WiFi + mat khau da luu) ---

// Tra loi WIFI_PW_QUERY: NVS CO mat khau cho ssid -> "WIFI_PW:<ssid>:<password>".
void uart_link_send_wifi_pw(const char *ssid, const char *password);

// Tra loi WIFI_PW_QUERY: NVS KHONG co mat khau cho ssid -> "WIFI_PW_NONE:<ssid>".
void uart_link_send_wifi_pw_none(const char *ssid);

// Bao trang thai radio WiFi that su -> "WIFI_POWER:<0|1>" (de C3 keo cong tac ve
// dung vi tri; cung la phan hoi cho WIFI_ENABLE / WIFI_DISABLE).
void uart_link_send_wifi_power(bool on);

// --- Gui ve ESP32-C3 (MOI: Bluetooth quet/luu/quen giong WiFi) ---

// "BT_POWER:<0|1>" - Bluetooth that su dang BAT/TAT (phan hoi cho BT_ENABLE /
// BT_DISABLE, cung duoc gui khi BT tu tat do het gio hoac loi).
void uart_link_send_bt_power(bool on);

// "BT_LIST:<addr>|<flags>|<ten>,<addr>|<flags>|<ten>,..." (rong neu chua co thiet bi)
//   addr  = 12 ky tu HEX viet hoa (dung lam dinh danh cho BT_CONNECT / BT_FORGET)
//   flags = bitmask: 1 = da luu, 2 = dang trong vung phu song, 4 = dang ket noi
void uart_link_send_bt_list(const char *devices_csv);

// "BT_ERR:<CONNECT_FAIL|INIT_FAIL|BAD_ADDR|NOT_ENABLED>"
void uart_link_send_bt_err(const char *code);
