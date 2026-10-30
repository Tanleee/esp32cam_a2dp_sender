#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Chi CAU HINH (ten local, data callback, chan mute loa) - KHONG tu dong quet/
// ket noi Bluetooth nua. Goi bt_a2dp_source_connect() de thuc su bat dau.
esp_err_t bt_a2dp_source_init(RingbufHandle_t ringbuf);

void bt_a2dp_source_set_muted(bool muted);

// Bat dau quet + ket noi toi loa Bluetooth "LE208". CHI duoc goi khi nhan lenh
// BT_CONNECT tu ESP32-C3 (xem system_ctrl.c), KHONG tu dong goi luc khoi dong.
void bt_a2dp_source_connect(void);

// Ngat ket noi Bluetooth hien tai (neu dang ket noi).
void bt_a2dp_source_disconnect(void);

// Goi moi khi trang thai ket noi Bluetooth thay doi (vua ket noi thanh cong /
// vua ngat) - dung de system_ctrl.c day thong tin nay qua UART cho ESP32-C3.
// device_name truyen vao callback rong ("") khi connected = false.
typedef void (*bt_a2dp_status_cb_t)(bool connected, const char *device_name);
void bt_a2dp_source_set_status_callback(bt_a2dp_status_cb_t cb);

#ifdef __cplusplus
}
#endif
