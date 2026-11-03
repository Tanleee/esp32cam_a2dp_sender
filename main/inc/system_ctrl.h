#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Module dieu phoi cac tinh nang "he thong" moi, tach rieng khoi app_controller.c
// (chi lo tinh nang AI) de de doc/bao tri:
//   - Dong bo thoi gian qua SNTP (sau khi WiFi ket noi) va dinh ky gui
//     TIME:<hh:mm>|<ngay thang> qua UART cho ESP32-C3.
//   - Dang ky nhan trang thai WiFi/Bluetooth thay doi (tu wifi_mgr.c /
//     bt_a2dp_source.cpp) va chuyen tiep qua UART cho ESP32-C3.
//
// Goi system_ctrl_init() 1 lan trong app_main (thu tu khong quan trong lam,
// chi can goi sau khi wifi_mgr/bt_a2dp_source da duoc KHAI BAO - khong can
// doi chung ket noi xong, system_ctrl tu cho ngam ben trong).
esp_err_t system_ctrl_init(void);

// 4 ham nay la noi THUC SU xu ly cac lenh moi tu ESP32-C3 qua UART. Chung
// KHONG duoc goi truc tiep tu uart_link.c - app_controller.c la noi DUY NHAT
// goi uart_link_init(), nen no se tro 4 callback nay vao struct
// uart_link_callbacks_t cua minh.
void system_ctrl_handle_bt_connect(void);
void system_ctrl_handle_bt_disconnect(void);
void system_ctrl_handle_wifi_scan_req(void);
void system_ctrl_handle_wifi_connect(const char *ssid, const char *password);

// MOI: WIFI_ENABLE / WIFI_DISABLE - bat/tat radio WiFi (driver van giu khoi tao).
void system_ctrl_handle_wifi_enable(bool enable);

// MOI: WIFI_PW_QUERY:<ssid> - tra loi WIFI_PW / WIFI_PW_NONE tuy NVS co mat khau chua.
void system_ctrl_handle_wifi_pw_query(const char *ssid);

// MOI: WIFI_FORGET:<ssid> - xoa ssid + mat khau khoi NVS.
void system_ctrl_handle_wifi_forget(const char *ssid);
