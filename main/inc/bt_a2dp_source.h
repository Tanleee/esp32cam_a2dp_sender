#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Bluetooth A2DP source - giong cach WiFi hoat dong:
//   * BAT  Bluetooth  -> tu dong QUET cac loa/tai nghe xung quanh (khong con
//                        ten thiet bi co dinh nao nua).
//   * Danh sach thiet bi (da luu + xung quanh) duoc day ve C3 qua callback.
//   * Chon 1 thiet bi (theo dia chi) -> ket noi; ket noi thanh cong thi tu
//     dong LUU vao NVS. "Quen" = xoa khoi danh sach da luu.
// Moi callback duoc goi tu task rieng cua module nay (KHONG phai task Bluedroid)
// nen co the an toan ghi UART / dung mutex trong callback.
// ============================================================================

#define BT_MGR_MAX_SAVED     8     // so thiet bi toi da luu trong NVS
#define BT_MGR_MAX_FOUND     12    // so thiet bi toi da nho tu lan quet
#define BT_LIST_MAX_ROWS     12    // so dong toi da gui ve C3 (giu dong UART < 1024 byte)
#define BT_ADDR_STR_LEN      13    // 12 ky tu hex + NUL, vd "A4CF1251B3D6"
#define BT_NAME_MAX_LEN      32    // ten toi da (chua tinh NUL)

typedef struct {
    char addr[BT_ADDR_STR_LEN];        // dia chi MAC dang HEX viet hoa, khong dau ':'
    char name[BT_NAME_MAX_LEN + 1];    // da loai bo ky tu ',' '|' va ky tu dieu khien
    int  rssi;                         // 0 neu hien khong thay trong lan quet
    bool saved;                        // da luu trong NVS
    bool in_range;                     // dang thay trong vung phu song (hoac dang ket noi)
    bool connected;                    // dang ket noi A2DP
} bt_device_info_t;

typedef struct {
    // Doi trang thai ket noi (connected=false thi device_name = "").
    void (*on_status)(bool connected, const char *device_name);
    // Bluetooth that su BAT/TAT (de C3 keo cong tac ve dung vi tri).
    void (*on_power)(bool on);
    // Danh sach thiet bi thay doi (da sap xep: da luu truoc, roi theo RSSI giam dan).
    void (*on_list)(const bt_device_info_t *devs, int count);
    // Loi: "CONNECT_FAIL" | "INIT_FAIL"
    void (*on_error)(const char *code);
} bt_a2dp_callbacks_t;

// Chi CAU HINH (ten local, data callback, chan mute loa, nap danh sach da luu tu
// NVS, tao task quan ly) - KHONG bat Bluetooth. nvs_flash_init() phai duoc goi truoc.
esp_err_t bt_a2dp_source_init(RingbufHandle_t ringbuf);

void bt_a2dp_source_set_callbacks(const bt_a2dp_callbacks_t *cbs);

void bt_a2dp_source_set_muted(bool muted);

// --- Bat / tat (bat dong bo: tra ve ngay, ket qua bao qua on_power) ---
esp_err_t bt_a2dp_source_enable(void);    // khoi dong stack + bat dau quet
void      bt_a2dp_source_disable(void);   // ngat ket noi + dung A2DP (Bluedroid van nam trong RAM, bat lai duoc)
bool      bt_a2dp_source_is_enabled(void);

// Yeu cau day lai danh sach thiet bi ngay (cung gia han bo dem tu tat khi nhan roi).
void bt_a2dp_source_request_list(void);

// --- Ket noi / ngat / quen theo dia chi HEX (12 ky tu) ---
// ESP_ERR_INVALID_ARG: dia chi sai dinh dang; ESP_ERR_INVALID_STATE: BT dang tat.
esp_err_t bt_a2dp_source_connect_to(const char *addr_hex);
void      bt_a2dp_source_disconnect(void);              // chi ngat, BT van bat va tiep tuc quet
esp_err_t bt_a2dp_source_forget(const char *addr_hex);  // xoa khoi danh sach da luu (ngat neu dang noi)

// Lay danh sach hop nhat (da luu + xung quanh), tra ve so dong da dien.
int bt_a2dp_source_get_devices(bt_device_info_t *out, int max);

#ifdef __cplusplus
}
#endif
