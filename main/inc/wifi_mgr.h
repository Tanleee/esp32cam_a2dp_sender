#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stddef.h>

// Khoi tao driver WiFi (esp_wifi_init + STA mode + event handler) nhung KHONG
// bat radio (khong goi esp_wifi_start) va KHONG ket noi mang nao - goi 1 lan
// trong app_main. Trang thai ban dau cua WiFi la TAT. Radio chi duoc bat khi
// ESP32-C3 gui WIFI_ENABLE (wifi_mgr_enable), ket noi chi khi co WIFI_CONNECT.
esp_err_t wifi_mgr_init(void);

// Bat radio WiFi (esp_wifi_start) - idempotent. Driver da duoc init tu truoc
// nen day chi la "dung antena", khong khoi tao lai.
esp_err_t wifi_mgr_enable(void);

// Tat radio WiFi: ngat ket noi hien tai (neu co), esp_wifi_stop() - KHONG
// deinit, nen bat lai duoc ngay bang wifi_mgr_enable(). Bao WIFI_STATUS:0 qua
// status callback neu truoc do dang ket noi.
esp_err_t wifi_mgr_disable(void);

bool wifi_mgr_is_enabled(void);

bool wifi_mgr_is_connected(void);

typedef void (*wifi_mgr_status_cb_t)(bool connected, const char *ssid);
void wifi_mgr_set_status_callback(wifi_mgr_status_cb_t cb);

int wifi_mgr_scan(wifi_ap_record_t *out_records, int max_count);

// Bat dau ket noi, KHONG CHAN - dung trong vong lap co the ngat giua chung
// khi co yeu cau ket noi mang KHAC den (xem network_worker_task trong
// system_ctrl.c).
esp_err_t wifi_mgr_connect_start(const char *ssid, const char *password);

// Cho ket noi thanh cong toi da timeout_ms. Goi lap lai nhieu lan voi
// timeout_ms NHO (vd 300ms) de nguoi goi co the kiem tra yeu cau moi giua
// cac lan cho, thay vi 1 lan cho dai khong ngat duoc.
bool wifi_mgr_wait_connected(uint32_t timeout_ms);

// Huy tien trinh ket noi dang cho (dung khi co yeu cau ket noi mang KHAC den
// giua luc dang xac thuc mang nay).
void wifi_mgr_abort_connect(void);

// ================== MAT KHAU WIFI DA LUU (NVS, NHIEU MANG) ==================
// Luu toi da WIFI_MGR_MAX_SAVED mang (mang dung gan nhat o dau, mang cu nhat bi
// day ra khi day). Tat ca nam trong 1 blob "list" cua namespace "wifi_cfg"; neu
// chua co blob nay ma con cap ssid/pass kieu cu (1 mang) thi tu dong nhap vao.
#define WIFI_MGR_MAX_SAVED  8

// Duoc goi tu network_worker_task ngay sau khi ket noi THANH CONG. Mang da co
// thi cap nhat mat khau va dua len dau danh sach.
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password);

// true neu NVS co mat khau (KHAC RONG) cho ssid nay, mat khau copy vao pass_out.
bool wifi_mgr_get_saved_password(const char *ssid, char *pass_out, size_t pass_sz);

// Xoa ssid + mat khau khoi NVS. Neu dang ket noi/dang thu ket noi dung mang
// nay thi ngat luon va dung tu ket noi lai.
esp_err_t wifi_mgr_forget(const char *ssid);