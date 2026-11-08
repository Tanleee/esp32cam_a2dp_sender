#pragma once
#include "esp_err.h"

// Tao task kiem tra Internet. Goi 1 lan trong app_main.
esp_err_t net_check_init(void);

// Duoc goi tu uart_rx_task khi nhan lenh "NET_CHECK" tu ESP32-C3. KHONG chan:
// chi danh thuc task kiem tra roi tra ve ngay. Ket qua gui ve C3 qua UART:
//   NET_OK
//   NET_ERR:<WIFI_OFF|NO_WIFI|NO_INTERNET|NO_DNS|NO_SERVER|BUSY>
void net_check_handle_request(void);

// Kiem tra dong bo (chan toi da vai giay). Tra ve NULL neu co Internet that su,
// nguoc lai tra ve ma loi nhu tren.
const char *net_check_run(void);
