#pragma once
#include "esp_err.h"
#include <stdint.h>

// PWM (LEDC) dieu khien do sang man hinh qua GPIO14 (PIN_BACKLIGHT_PWM trong
// board_config.h). Man hinh vat ly do ESP32-C3 phu trach, nhung chan PWM dat
// tren board nay - nen do sang duoc dieu khien tu xa qua lenh BRIGHTNESS tu C3.
esp_err_t backlight_ctrl_init(void);

// percent: 0-100. Duoc goi tu uart_link khi nhan BRIGHTNESS:<percent> tu ESP32-C3.
void backlight_ctrl_handle_set(uint8_t percent);