#include "system_ctrl.h"
#include "uart_link.h"
#include "wifi_mgr.h"
#include "bt_a2dp_source.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "SYS_CTRL";

#define TIME_BROADCAST_INTERVAL_MS    30000
#define WIFI_SCAN_MAX_AP               16
#define WIFI_CONNECT_TOTAL_TIMEOUT_MS  15000
#define WIFI_CONNECT_POLL_SLICE_MS     300

// ================== CHONG XUNG DOT WIFI-DANG-KET-NOI <-> BLUETOOTH-DANG-KET-NOI ==================
// ESP32 dung chung radio cho WiFi + Bluetooth, de loi neu ca 2 cung xac thuc
// 1 luc - nen chi cho phep 1 trong 2 "dang trong qua trinh ket noi" tai 1
// thoi diem. Trang thai DA KET NOI on dinh (wifi + bt cung luc) khong bi anh
// huong boi co nay, chi chan luc DANG xac thuc.
typedef enum {
    RADIO_OP_NONE = 0,
    RADIO_OP_WIFI_CONNECTING,
    RADIO_OP_BT_CONNECTING,
} radio_op_t;
static volatile radio_op_t s_radio_op = RADIO_OP_NONE;

// ================== HANG DOI YEU CAU KET NOI WIFI (chi giu ban moi nhat) ==================
// xQueueOverwrite voi hang doi dai 1: yeu cau WIFI_CONNECT moi luon GHI DE
// yeu cau cu chua kip xu ly xong - network_worker_task tu phat hien o vong
// lap cho ben duoi va huy tien trinh dang xac thuc de chuyen sang mang moi.
typedef struct {
    char ssid[33];
    char password[65];
} wifi_connect_req_t;

static QueueHandle_t s_wifi_cmd_queue;

// ================== CAU HINH LAI uart_link ==================

// ================== BLUETOOTH (bat/tat + quet + chon thiet bi + quen) ==================

void system_ctrl_handle_bt_enable(void)
{
    if (s_radio_op == RADIO_OP_WIFI_CONNECTING) {
        ESP_LOGW(TAG, "Tu choi BT_ENABLE: WiFi dang xac thuc");
        uart_link_send_busy("WIFI_CONNECTING");
        uart_link_send_bt_power(false);   // de C3 keo cong tac Bluetooth ve TAT
        return;
    }
    // DEBUG: Bluedroid can nhieu RAM noi + 1 khoi lien tuc lon luc khoi dong. Neu
    // "largest" qua nho (vai chuc KB) thi loi "Start HCI Host Layer Failure" la
    // do thieu/phan manh bo nho, khong phai loi logic.
    ESP_LOGI(TAG, "Heap truoc khi bat BT: free=%u largest=%u min_ever=%u (WiFi %s)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             wifi_mgr_is_enabled() ? "BAT" : "TAT");

    // Bat dong bo: chi dat lenh vao hang doi cua bt_worker. Ket qua that su
    // (BT_POWER:1 hoac BT_ERR:INIT_FAIL + BT_POWER:0) se bao ve qua callback.
    if (bt_a2dp_source_enable() != ESP_OK) {
        uart_link_send_bt_err("INIT_FAIL");
        uart_link_send_bt_power(false);
    }
}

void system_ctrl_handle_bt_disable(void)
{
    if (s_radio_op == RADIO_OP_BT_CONNECTING) s_radio_op = RADIO_OP_NONE;
    bt_a2dp_source_disable();
}

void system_ctrl_handle_bt_scan_req(void)
{
    // BT dang tat (C3 va classic lech trang thai) -> bao lai trang thai that.
    if (!bt_a2dp_source_is_enabled()) {
        uart_link_send_bt_power(false);
        return;
    }
    bt_a2dp_source_request_list();
}

void system_ctrl_handle_bt_connect(const char *addr)
{
    if (s_radio_op == RADIO_OP_WIFI_CONNECTING) {
        ESP_LOGW(TAG, "Tu choi BT_CONNECT: WiFi dang xac thuc");
        uart_link_send_busy("WIFI_CONNECTING");
        uart_link_send_bt_err("CONNECT_FAIL");
        return;
    }
    esp_err_t ret = bt_a2dp_source_connect_to(addr);
    if (ret == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "BT_CONNECT dia chi sai dinh dang: %s", addr);
        uart_link_send_bt_err("BAD_ADDR");
    } else if (ret != ESP_OK) {
        uart_link_send_bt_err("NOT_ENABLED");
        uart_link_send_bt_power(false);
    } else {
        s_radio_op = RADIO_OP_BT_CONNECTING;   // duoc giai phong trong on_bt_status() / on_bt_error()
    }
}

void system_ctrl_handle_bt_disconnect(void)
{
    if (s_radio_op == RADIO_OP_BT_CONNECTING) s_radio_op = RADIO_OP_NONE;   // huy cho ket noi dang do
    bt_a2dp_source_disconnect();
}

void system_ctrl_handle_bt_forget(const char *addr)
{
    if (s_radio_op == RADIO_OP_BT_CONNECTING) s_radio_op = RADIO_OP_NONE;   // co the dang huy chinh thiet bi dang cho noi
    esp_err_t ret = bt_a2dp_source_forget(addr);
    if (ret == ESP_ERR_INVALID_ARG) uart_link_send_bt_err("BAD_ADDR");
    else if (ret != ESP_OK) ESP_LOGW(TAG, "BT_FORGET that bai: %s", esp_err_to_name(ret));
}

void system_ctrl_handle_wifi_scan_req(void)
{
    // WiFi dang tat (C3 va classic lech trang thai, vd C3 vua reset) -> khong
    // quet, bao lai trang thai that de C3 keo cong tac ve TAT.
    if (!wifi_mgr_is_enabled()) {
        uart_link_send_wifi_power(false);
        return;
    }

    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * WIFI_SCAN_MAX_AP);
    if (!records) {
        uart_link_send_wifi_err("SCAN_FAIL");
        return;
    }

    int n = wifi_mgr_scan(records, WIFI_SCAN_MAX_AP);
    if (n <= 0) {
        free(records);
        uart_link_send_wifi_err("SCAN_FAIL");
        return;
    }

    size_t csv_cap = (size_t)n * 40 + 8;
    char *csv = malloc(csv_cap);
    if (!csv) {
        free(records);
        uart_link_send_wifi_err("SCAN_FAIL");
        return;
    }

    size_t off = 0;
    csv[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (records[i].ssid[0] == '\0') continue;
        int secure = (records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
        int w = snprintf(csv + off, csv_cap - off, "%s%s|%d",
                          (off > 0) ? "," : "", (const char *)records[i].ssid, secure);
        if (w < 0 || (size_t)w >= csv_cap - off) break;
        off += (size_t)w;
    }

    uart_link_send_wifi_list(csv);
    free(csv);
    free(records);
}

void system_ctrl_handle_wifi_connect(const char *ssid, const char *password)
{
    if (!wifi_mgr_is_enabled()) {
        ESP_LOGW(TAG, "Tu choi WIFI_CONNECT: WiFi dang tat");
        uart_link_send_wifi_power(false);
        return;
    }

    if (s_radio_op == RADIO_OP_BT_CONNECTING) {
        ESP_LOGW(TAG, "Tu choi WIFI_CONNECT: Bluetooth dang ket noi");
        uart_link_send_busy("BT_CONNECTING");
        return;
    }

    wifi_connect_req_t req;
    strncpy(req.ssid, ssid, sizeof(req.ssid) - 1);
    req.ssid[sizeof(req.ssid) - 1] = '\0';
    strncpy(req.password, password, sizeof(req.password) - 1);
    req.password[sizeof(req.password) - 1] = '\0';

    // Khong CHAN o day: chi ghi (de) yeu cau vao hang doi roi tra ve ngay, de
    // uart_rx_task van ranh doc cac lenh UART tiep theo. Viec ket noi/huy
    // thuc su xay ra trong network_worker_task.
    xQueueOverwrite(s_wifi_cmd_queue, &req);
}

// ================== MOI: BAT/TAT WIFI + MAT KHAU DA LUU ==================

void system_ctrl_handle_wifi_enable(bool enable)
{
    if (enable) {
        esp_err_t ret = wifi_mgr_enable();
        // Luon phan hoi trang thai THAT SU (ke ca khi that bai) de C3 dong bo cong tac.
        uart_link_send_wifi_power(ret == ESP_OK);
    } else {
        // Bo yeu cau ket noi dang cho trong hang doi; network_worker_task tu
        // phat hien radio da tat va dung cho (xem vong lap ben duoi).
        if (s_wifi_cmd_queue) xQueueReset(s_wifi_cmd_queue);
        wifi_mgr_disable();   // tu gui WIFI_STATUS:0 neu dang ket noi
        uart_link_send_wifi_power(false);
    }
}

void system_ctrl_handle_wifi_pw_query(const char *ssid)
{
    char password[65];
    if (wifi_mgr_get_saved_password(ssid, password, sizeof(password))) {
        uart_link_send_wifi_pw(ssid, password);
    } else {
        uart_link_send_wifi_pw_none(ssid);
    }
    memset(password, 0, sizeof(password));   // khong de mat khau nam lai tren stack
}

void system_ctrl_handle_wifi_forget(const char *ssid)
{
    esp_err_t ret = wifi_mgr_forget(ssid);
    if (ret != ESP_OK) ESP_LOGW(TAG, "WIFI_FORGET that bai: %s", esp_err_to_name(ret));
}

// ================== TRANG THAI WIFI/BLUETOOTH -> UART ==================

static void on_wifi_status(bool connected, const char *ssid)
{
    uart_link_send_wifi_status(connected, ssid);
}

static void on_bt_status(bool connected, const char *device_name)
{
    if (s_radio_op == RADIO_OP_BT_CONNECTING) s_radio_op = RADIO_OP_NONE;
    uart_link_send_bt_status(connected, device_name);
}

static void on_bt_power(bool on)
{
    if (!on && s_radio_op == RADIO_OP_BT_CONNECTING) s_radio_op = RADIO_OP_NONE;
    uart_link_send_bt_power(on);
}

static void on_bt_error(const char *code)
{
    if (s_radio_op == RADIO_OP_BT_CONNECTING) s_radio_op = RADIO_OP_NONE;
    uart_link_send_bt_err(code);
}

// Danh sach -> "addr|flags|ten,addr|flags|ten,..." (flags: 1=da luu, 2=trong vung, 4=dang noi).
// Duoc goi tu bt_worker (khong phai task Bluedroid) nen malloc + ghi UART an toan.
static void on_bt_list(const bt_device_info_t *devs, int count)
{
    // moi dong toi da: 12 (addr) + 1 ('|') + 1 (flags) + 1 ('|') + 32 (ten) + 1 (',') = 48
    size_t csv_cap = (size_t)count * 52 + 8;
    char *csv = malloc(csv_cap);
    if (!csv) {
        ESP_LOGE(TAG, "Khong du RAM de tao BT_LIST");
        return;
    }

    size_t off = 0;
    csv[0] = '\0';
    for (int i = 0; i < count; i++) {
        int flags = (devs[i].saved ? 1 : 0) | (devs[i].in_range ? 2 : 0) | (devs[i].connected ? 4 : 0);
        int w = snprintf(csv + off, csv_cap - off, "%s%s|%d|%s",
                         (off > 0) ? "," : "", devs[i].addr, flags, devs[i].name);
        if (w < 0 || (size_t)w >= csv_cap - off) break;
        off += (size_t)w;
    }

    uart_link_send_bt_list(csv);
    free(csv);
}

// ================== WORKER XU LY KET NOI WIFI (co the bi ngat giua chung) ==================
// Day la phan FreeRTOS "dung" cho yeu cau 3: thay vi cho CHAN 15s ngay trong
// uart_rx_task (se khoa UART, khong doc duoc lenh moi trong luc cho - loi
// kien truc cu), task rieng nay cho theo tung lat NHO (300ms) va kiem tra
// hang doi giua moi lat. Neu phat hien yeu cau WIFI_CONNECT MOI (SSID khac)
// trong luc dang cho, no HUY NGAY tien trinh hien tai (wifi_mgr_abort_connect)
// roi chuyen sang xu ly yeu cau moi - dung yeu cau "huy xac thuc dang do,
// chuyen sang mang moi + bao ket qua switch co thanh cong hay khong".
static void network_worker_task(void *arg)
{
    wifi_connect_req_t req;

    for (;;) {
        xQueueReceive(s_wifi_cmd_queue, &req, portMAX_DELAY);

    process_request:
        ESP_LOGI(TAG, "Bat dau ket noi WiFi: %s", req.ssid);
        s_radio_op = RADIO_OP_WIFI_CONNECTING;
        esp_err_t start_ret = wifi_mgr_connect_start(req.ssid, req.password);
        if (start_ret != ESP_OK) {
            ESP_LOGW(TAG, "Bat dau ket noi %s that bai: %s", req.ssid, esp_err_to_name(start_ret));
            s_radio_op = RADIO_OP_NONE;
            uart_link_send_wifi_err("CONNECT_FAIL");
            continue;
        }

        bool connected = false;
        uint32_t waited_ms = 0;
        while (waited_ms < WIFI_CONNECT_TOTAL_TIMEOUT_MS) {
            if (wifi_mgr_wait_connected(WIFI_CONNECT_POLL_SLICE_MS)) {
                connected = true;
                break;
            }
            waited_ms += WIFI_CONNECT_POLL_SLICE_MS;

            // Nguoi dung tat WiFi giua luc dang xac thuc -> dung cho ngay.
            if (!wifi_mgr_is_enabled()) break;

            wifi_connect_req_t newer;
            if (xQueuePeek(s_wifi_cmd_queue, &newer, 0) == pdTRUE &&
                strcmp(newer.ssid, req.ssid) != 0) {
                xQueueReceive(s_wifi_cmd_queue, &newer, 0);   // lay ra khoi hang doi
                ESP_LOGI(TAG, "Co yeu cau ket noi mang khac (%s) - huy mang hien tai (%s)",
                         newer.ssid, req.ssid);
                wifi_mgr_abort_connect();
                uart_link_send_wifi_switch(true);
                req = newer;
                goto process_request;
            }
        }

        s_radio_op = RADIO_OP_NONE;
        if (!wifi_mgr_is_enabled()) {
            continue;   // WiFi bi tat giua chung: khong bao CONNECT_FAIL, khong luu mat khau
        }
        if (connected) {
            wifi_mgr_save_credentials(req.ssid, req.password);
            // on_wifi_status() da tu gui WIFI_STATUS roi (qua event WiFi cua
            // ESP-IDF, khong can gui lai o day).
        } else {
            // Dung vong tu thu ket noi lai cua wifi_evt() (neu khong thi sai mat
            // khau se bi thu mai va lien tuc gui WIFI_STATUS:0).
            wifi_mgr_abort_connect();
            uart_link_send_wifi_err("CONNECT_FAIL");
        }
    }
}

// ================== DONG BO THOI GIAN (SNTP) + DINH KY GUI UART ==================

static void time_sync_task(void *arg)
{
    while (!wifi_mgr_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);

    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "Dong bo thoi gian (SNTP) that bai, thu lai sau 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "Da dong bo thoi gian tu may chu NTP");

    while (1) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char hhmm[8];
        char date_str[24];
        strftime(hhmm, sizeof(hhmm), "%H:%M", &tm_info);
        strftime(date_str, sizeof(date_str), "%a, %d %b %Y", &tm_info);

        uart_link_send_time(hhmm, date_str);
        vTaskDelay(pdMS_TO_TICKS(TIME_BROADCAST_INTERVAL_MS));
    }
}

esp_err_t system_ctrl_init(void)
{
    wifi_mgr_set_status_callback(on_wifi_status);
    bt_a2dp_callbacks_t bt_cbs = {
        .on_status = on_bt_status,
        .on_power  = on_bt_power,
        .on_list   = on_bt_list,
        .on_error  = on_bt_error,
    };
    bt_a2dp_source_set_callbacks(&bt_cbs);

    s_wifi_cmd_queue = xQueueCreate(1, sizeof(wifi_connect_req_t));
    if (!s_wifi_cmd_queue) return ESP_ERR_NO_MEM;

    BaseType_t ok1 = xTaskCreate(network_worker_task, "net_worker", 4096, NULL, 4, NULL);
    BaseType_t ok2 = xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 3, NULL);
    return (ok1 == pdPASS && ok2 == pdPASS) ? ESP_OK : ESP_FAIL;
}