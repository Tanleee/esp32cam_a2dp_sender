#pragma once
#include "esp_err.h"

// 2 nut nhan tang/giam am luong duoc chuyen tu board ESP32-C3 sang board
// ESP32 classic nay. Board nay khong phat am thanh truc tiep nen nut nhan
// KHONG dieu chinh volume cuc bo, ma gui lenh VOL_UP / VOL_DOWN qua UART
// sang ESP32-C3 de ben do tu dieu chinh.
esp_err_t volume_button_init(void);
