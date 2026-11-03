#pragma once
#include "esp_err.h"

esp_err_t app_controller_init(void);

// 2 ham nay la noi THUC SU xu ly REC_START/REC_STOP tu ESP32-C3. uart_link_init()
// gio duoc goi TU main.c (gom callback cua ca app_controller/system_ctrl/
// backlight_ctrl), nen 2 ham nay duoc khai bao cong khai de main.c tro vao
// (truoc day la static, tu dang ky ngay trong app_controller_init()).
void app_controller_handle_rec_start(const char *playlist_csv);
void app_controller_handle_rec_stop(void);