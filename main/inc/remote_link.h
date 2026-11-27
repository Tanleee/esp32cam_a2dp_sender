#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Cau noi giua web server (WROVER) va ESP32-C3: luu trang thai C3 gui ve (PL_STAT),
// gui lenh RC:..., lay danh sach bai, va day file xuong C3 qua UART.

esp_err_t remote_link_init(void);

// Duoc uart_link goi cho moi dong nhan duoc. true = da xu ly (dong cua remote_link).
bool remote_link_handle_line(const char *line);

// uart_link goi khi co thay doi trang thai Bluetooth (de web biet).
void remote_link_note_bt(bool connected, const char *device_name);

// ---- Trang thai cho /api/status ----
// Ghi JSON vao buf; tra ve do dai, hoac -1 neu C3 khong phan hoi (khong nhan PL_STAT > 6 giay).
int remote_link_build_status_json(char *buf, size_t buf_size);

// ---- /api/tracks ----
// Hoi C3 danh sach bai, ghi JSON {"tracks":[...]} vao buf. -1 neu loi/timeout.
int remote_link_build_tracks_json(char *buf, size_t buf_size);

// ---- /api/cmd ----
// cmd_line vd "play", "volume:60", "play_track:abc.wav". Cho C3 tra loi.
// Tra ve true neu RC_OK; nguoc lai err_out chua ma loi.
bool remote_link_rc(const char *cmd_line, char *err_out, size_t err_sz);

// ---- /api/upload/* : truyen file xuong C3 (moi lan CHI 1 file) ----
// type = 'A' (audio) hoac 'W' (wallpaper). Tra ve false neu dang ban hoac C3 tu choi.
bool remote_link_xfer_begin(char type, const char *name, uint32_t size, char *err_out, size_t err_sz);
// Gui 1 khoi (<= 2048 byte) va doi C3 xac nhan (tu gui lai neu loi). false neu that bai han.
bool remote_link_xfer_send(const uint8_t *data, size_t len);
// Doi C3 bao FILE_DONE (da ghi xong, doi ten, cap nhat thu vien/hinh nen).
bool remote_link_xfer_finish(char *err_out, size_t err_sz);
// Huy giua chung (bao C3 xoa file tam).
void remote_link_xfer_abort(void);
