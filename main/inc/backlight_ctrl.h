#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// PWM (LEDC) dieu khien do sang man hinh qua GPIO14 (PIN_BACKLIGHT_PWM trong
// board_config.h). Man hinh vat ly do ESP32-C3 phu trach, nhung chan PWM dat
// tren board nay - nen do sang duoc dieu khien tu xa qua lenh BRIGHTNESS tu C3.
esp_err_t backlight_ctrl_init(void);

// percent: 0-100. Duoc goi tu uart_link khi nhan BRIGHTNESS:<percent> tu ESP32-C3
// (va tu web_server khi nguoi dung chinh do sang tren web).
// Luon GHI NHO gia tri; neu man hinh dang bi tat bang nut thi CHUA ap len den nen -
// se ap dung khi man hinh duoc bat lai.
void backlight_ctrl_handle_set(uint8_t percent);

// MOI: tat/bat man hinh bang nut (GPIO35). off = true -> den nen tat han (duty 0);
// off = false -> khoi phuc dung do sang da ghi nho. Chi dieu khien den nen; viec
// bao cho C3 (khoa cam ung + hien lockscreen) do volume_button.c / uart_link.c lam.
void backlight_ctrl_set_screen_off(bool off);

// MOI: true neu man hinh dang bi tat bang nut.
bool backlight_ctrl_is_screen_off(void);
