#include "wifi_mgr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static EventGroupHandle_t s_eg;
static bool s_initialized = false;
static volatile bool s_enabled = false;   // radio dang bat (esp_wifi_start) hay tat
static StaticSemaphore_t s_nvs_mtx_buf;
static SemaphoreHandle_t s_nvs_mtx = NULL;   // bao ve doc/ghi danh sach mat khau trong NVS
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISC_BIT      BIT1   // da nhan DISCONNECTED do CHINH MINH chu dong ngat (xem connect_start)

// s_switching = true trong luc wifi_mgr_connect_start() dang ngat mang cu de
// chuyen sang mang moi: wifi_evt() KHONG bao WIFI_STATUS:0 va KHONG tu
// esp_wifi_connect() lai trong luc nay (truoc day no tu connect lai song song
// voi lenh connect cua connect_start -> esp_wifi_connect() tra ESP_ERR_WIFI_CONN
// -> bi bao CONNECT_FAIL sai).
// s_connecting = dang trong qua trinh xac thuc (da goi esp_wifi_connect nhung
// chua co GOT_IP/DISCONNECTED).
static volatile bool s_switching  = false;
static volatile bool s_connecting = false;

static wifi_mgr_status_cb_t s_status_cb = NULL;
static char s_current_ssid[33] = "";

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID  "ssid"   // KIEU CU (1 mang) - chi doc de nhap sang danh sach moi
#define WIFI_NVS_KEY_PASS  "pass"
#define WIFI_NVS_KEY_LIST  "list"   // blob saved_list_t - nhieu mang

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // MOI: log ly do ngat de biet vi sao WiFi rot (201=NO_AP_FOUND, 15/204=sai
        // mat khau/handshake, 200=BEACON_TIMEOUT, 8=ASSOC_LEAVE, 2=AUTH_EXPIRE...).
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA_DISCONNECTED reason=%d rssi=%d switching=%d ssid=\"%s\"",
                 d ? (int)d->reason : -1, d ? (int)d->rssi : 0, (int)s_switching, s_current_ssid);
        xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
        s_connecting = false;
        if (s_switching) {
            // Ngat chu dong de chuyen mang: chi bao cho connect_start() biet, khong
            // bao trang thai ra ngoai va khong tu ket noi lai.
            xEventGroupSetBits(s_eg, WIFI_DISC_BIT);
            return;
        }
        if (s_status_cb) s_status_cb(false, "");
        // Chi tu dong thu ket noi lai (de giu ket noi on dinh sau 1 lan rot
        // mang tam thoi) khi TRUOC DO da tung co 1 lan ket noi thuc su (co
        // ssid) - KHONG tu ket noi mang nao luc moi khoi dong khi s_current_ssid
        // con rong, dung yeu cau "chi ket noi khi co lenh qua UART".
        if (s_current_ssid[0] != '\0') {
            if (esp_wifi_connect() == ESP_OK) s_connecting = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connecting = false;
        ESP_LOGI(TAG, "Da co IP, ssid=\"%s\"", s_current_ssid);
        xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
        if (s_status_cb) s_status_cb(true, s_current_ssid);
    }
}

esp_err_t wifi_mgr_init(void)
{
    if (s_initialized) return ESP_OK;

    s_eg = xEventGroupCreate();
    if (!s_eg) return ESP_ERR_NO_MEM;
    s_nvs_mtx = xSemaphoreCreateMutexStatic(&s_nvs_mtx_buf);

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) return ret;
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL);
    if (ret != ESP_OK) return ret;
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    // Driver da init xong nhung CHUA esp_wifi_start(): WiFi mac dinh TAT (khong
    // dung antena) cho den khi ESP32-C3 gui WIFI_ENABLE. KHONG dat cau hinh
    // SSID/password nao, KHONG goi esp_wifi_connect() o day.
    s_initialized = true;
    s_enabled = false;
    return ESP_OK;
}

esp_err_t wifi_mgr_enable(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_enabled) return ESP_OK;
    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK) {
        s_enabled = true;
        ESP_LOGI(TAG, "Da bat radio WiFi");
    } else {
        ESP_LOGW(TAG, "Bat radio WiFi that bai: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wifi_mgr_disable(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!s_enabled) return ESP_OK;

    bool was_connected = wifi_mgr_is_connected();

    // Xoa ssid TRUOC de wifi_evt() khong tu esp_wifi_connect() lai khi nhan
    // su kien DISCONNECTED do chinh lenh ngat nay gay ra.
    s_current_ssid[0] = '\0';
    s_enabled = false;
    s_connecting = false;
    xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);

    esp_wifi_disconnect();   // co the tra loi loi neu chua tung ket noi - bo qua
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Tat radio WiFi that bai: %s", esp_err_to_name(ret));
    else ESP_LOGI(TAG, "Da tat radio WiFi (driver van duoc giu khoi tao)");

    if (was_connected && s_status_cb) s_status_cb(false, "");
    return ret;
}

bool wifi_mgr_is_enabled(void)
{
    return s_enabled;
}

bool wifi_mgr_is_connected(void)
{
    return s_eg && (xEventGroupGetBits(s_eg) & WIFI_CONNECTED_BIT);
}

void wifi_mgr_set_status_callback(wifi_mgr_status_cb_t cb)
{
    s_status_cb = cb;
}

int wifi_mgr_scan(wifi_ap_record_t *out_records, int max_count)
{
    if (!s_initialized || !s_enabled || max_count <= 0) return 0;

    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        // Vua bat radio xong co the can them chut thoi gian truoc khi quet duoc.
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_wifi_scan_start(&scan_cfg, true);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Quet WiFi that bai: %s", esp_err_to_name(ret));
        return 0;
    }

    uint16_t count = (uint16_t)max_count;
    ret = esp_wifi_scan_get_ap_records(&count, out_records);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Lay danh sach mang WiFi that bai: %s", esp_err_to_name(ret));
        return 0;
    }
    return (int)count;
}

esp_err_t wifi_mgr_connect_start(const char *ssid, const char *password)
{
    if (!s_initialized || !s_enabled) return ESP_ERR_INVALID_STATE;

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    }

    // 1) Ngat mang cu (neu co) va CHO su kien DISCONNECTED tuong ung truoc khi
    //    dat cau hinh moi + connect. Trong luc nay wifi_evt() bi chan khong cho
    //    tu connect lai / bao trang thai (s_switching).
    s_current_ssid[0] = '\0';
    s_switching = true;
    bool expect_evt = wifi_mgr_is_connected() || s_connecting;
    xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT | WIFI_DISC_BIT);
    esp_wifi_disconnect();   // loi (chua tung ket noi) thi bo qua
    if (expect_evt) {
        xEventGroupWaitBits(s_eg, WIFI_DISC_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    }

    // 2) Cau hinh mang moi roi connect.
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        s_switching = false;
        ESP_LOGW(TAG, "esp_wifi_set_config loi: %s", esp_err_to_name(ret));
        return ret;
    }

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';
    s_switching = false;

    ret = esp_wifi_connect();
    if (ret == ESP_ERR_WIFI_CONN) ret = ESP_OK;   // da dang trong qua trinh ket noi -> khong phai loi
    if (ret == ESP_OK) {
        s_connecting = true;
    } else {
        ESP_LOGW(TAG, "esp_wifi_connect loi: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool wifi_mgr_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_mgr_abort_connect(void)
{
    // Xoa ssid de wifi_evt() khong tu thu ket noi lai (neu khong thi lenh
    // disconnect nay se bi chinh no "huy" bang 1 lan esp_wifi_connect() moi).
    // wifi_mgr_connect_start() se dat lai ssid neu sau do co mang moi.
    s_current_ssid[0] = '\0';
    esp_wifi_disconnect();
}

// ================== MAT KHAU DA LUU (NVS, NHIEU MANG) ==================

typedef struct {
    char ssid[33];
    char pass[65];
} saved_entry_t;

typedef struct {
    uint8_t       count;
    saved_entry_t e[WIFI_MGR_MAX_SAVED];
} saved_list_t;

static bool nvs_lock(void)
{
    return s_nvs_mtx && xSemaphoreTake(s_nvs_mtx, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void nvs_unlock(void)
{
    xSemaphoreGive(s_nvs_mtx);
}

// Doc danh sach tu NVS (h da mo). Chua co blob thi thu nhap cap ssid/pass kieu
// cu (1 mang) vao - CHI trong bo nho, se duoc ghi xuong o lan store_list() ke tiep.
static void load_list(nvs_handle_t h, saved_list_t *l)
{
    memset(l, 0, sizeof(*l));

    size_t sz = sizeof(*l);
    esp_err_t ret = nvs_get_blob(h, WIFI_NVS_KEY_LIST, l, &sz);
    if (ret == ESP_OK && sz == sizeof(*l) && l->count <= WIFI_MGR_MAX_SAVED) {
        for (int i = 0; i < l->count; i++) {   // phong du lieu hong: luon co NUL
            l->e[i].ssid[sizeof(l->e[i].ssid) - 1] = '\0';
            l->e[i].pass[sizeof(l->e[i].pass) - 1] = '\0';
        }
        return;
    }
    memset(l, 0, sizeof(*l));

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        char old_ssid[33], old_pass[65];
        size_t ssz = sizeof(old_ssid), psz = sizeof(old_pass);
        if (nvs_get_str(h, WIFI_NVS_KEY_SSID, old_ssid, &ssz) == ESP_OK && old_ssid[0] != '\0') {
            if (nvs_get_str(h, WIFI_NVS_KEY_PASS, old_pass, &psz) != ESP_OK) old_pass[0] = '\0';
            strncpy(l->e[0].ssid, old_ssid, sizeof(l->e[0].ssid) - 1);
            strncpy(l->e[0].pass, old_pass, sizeof(l->e[0].pass) - 1);
            l->count = 1;
            ESP_LOGI(TAG, "Nhap mang WiFi kieu cu vao danh sach moi: %s", old_ssid);
        }
    }
}

static esp_err_t store_list(nvs_handle_t h, const saved_list_t *l)
{
    esp_err_t ret = nvs_set_blob(h, WIFI_NVS_KEY_LIST, l, sizeof(*l));
    if (ret == ESP_OK) ret = nvs_commit(h);
    return ret;
}

static int find_entry(const saved_list_t *l, const char *ssid)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->e[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!nvs_lock()) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mo NVS de luu wifi that bai: %s", esp_err_to_name(ret));
        nvs_unlock();
        return ret;
    }

    saved_list_t list;
    load_list(h, &list);

    // idx = vi tri slot se bi ghi de: mang da co / slot trong / mang cu nhat (khi day).
    int idx = find_entry(&list, ssid);
    if (idx < 0) {
        if (list.count >= WIFI_MGR_MAX_SAVED) {
            idx = WIFI_MGR_MAX_SAVED - 1;
        } else {
            idx = list.count++;
        }
    }
    // Dich cac muc phia truoc xuong 1 cho, dat mang nay len DAU danh sach.
    memmove(&list.e[1], &list.e[0], (size_t)idx * sizeof(saved_entry_t));
    memset(&list.e[0], 0, sizeof(saved_entry_t));
    strncpy(list.e[0].ssid, ssid, sizeof(list.e[0].ssid) - 1);
    if (password) strncpy(list.e[0].pass, password, sizeof(list.e[0].pass) - 1);

    ret = store_list(h, &list);
    nvs_close(h);
    nvs_unlock();
    if (ret == ESP_OK) ESP_LOGI(TAG, "Da luu ssid/password vao NVS: %s", ssid);
    else ESP_LOGW(TAG, "Luu wifi vao NVS that bai: %s", esp_err_to_name(ret));
    return ret;
}

bool wifi_mgr_get_saved_password(const char *ssid, char *pass_out, size_t pass_sz)
{
    if (!ssid || !pass_out || pass_sz == 0) return false;
    pass_out[0] = '\0';
    if (!nvs_lock()) return false;

    bool found = false;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret == ESP_OK) {
        saved_list_t list;
        load_list(h, &list);
        int idx = find_entry(&list, ssid);
        if (idx >= 0 && list.e[idx].pass[0] != '\0') {
            strncpy(pass_out, list.e[idx].pass, pass_sz - 1);
            pass_out[pass_sz - 1] = '\0';
            found = true;
        }
        nvs_close(h);
    }
    // ESP_ERR_NVS_NOT_FOUND (namespace chua ton tai) = chua luu mang nao.
    nvs_unlock();
    return found;
}

esp_err_t wifi_mgr_forget(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    // Dang ket noi / dang thu ket noi dung mang nay -> ngat va dung tu ket noi lai.
    if (strcmp(s_current_ssid, ssid) == 0) {
        s_current_ssid[0] = '\0';
        xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
        if (s_enabled) esp_wifi_disconnect();
    }

    if (!nvs_lock()) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        nvs_unlock();
        return ret;
    }

    saved_list_t list;
    load_list(h, &list);
    int idx = find_entry(&list, ssid);
    if (idx >= 0) {
        memmove(&list.e[idx], &list.e[idx + 1],
                (size_t)(list.count - idx - 1) * sizeof(saved_entry_t));
        list.count--;
        memset(&list.e[list.count], 0, sizeof(saved_entry_t));
        // Ghi lai danh sach moi (ke ca khi rong): co blob roi thi cap ssid/pass
        // kieu cu khong con duoc nhap lai nua.
        ret = store_list(h, &list);
        if (ret == ESP_OK) ESP_LOGI(TAG, "Da xoa mang WiFi khoi NVS: %s", ssid);
    }
    nvs_close(h);
    nvs_unlock();
    return ret;
}