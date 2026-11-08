#include "BluetoothA2DPSource.h"
#include "bt_a2dp_source.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_bt_main.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

static const char *TAG = "BT_A2DP_SRC";

// ================== THAM SO ==================
#define BT_FOUND_TTL_MS          45000   // khong thay lai thiet bi trong 45s -> coi la roi khoi vung phu song
#define BT_CONNECT_WAIT_MS       40000   // tu luc nguoi dung chon den luc ket noi duoc (gom ca cho thay lai thiet bi)
#define BT_CONNECT_ATTEMPT_MS    15000   // sau khi bat dau noi that su, cho toi da 15s
#define BT_SCAN_IDLE_TIMEOUT_MS  120000  // dang quet ma 2 phut khong ai tuong tac -> tu tat BT
#define BT_LIST_MIN_INTERVAL_MS  300     // khoang cach toi thieu giua 2 lan day danh sach ve C3

#define BT_NVS_NAMESPACE         "bt_cfg"
#define BT_NVS_KEY_SAVED         "saved"

static BluetoothA2DPSource a2dp_source;
static RingbufHandle_t     s_ringbuf = NULL;
static volatile bool       s_muted = false;
static bt_a2dp_callbacks_t s_cbs = {};

// ================== TRANG THAI (bao ve boi s_mtx) ==================
typedef struct {
    uint8_t  bda[6];
    char     name[BT_NAME_MAX_LEN + 1];
    int8_t   rssi;
    uint32_t last_seen_ms;
    bool     used;
} found_t;

typedef struct {
    uint8_t bda[6];
    char    name[BT_NAME_MAX_LEN + 1];
} saved_entry_t;

typedef struct {
    uint8_t       count;
    saved_entry_t e[BT_MGR_MAX_SAVED];   // [0] = vua dung gan nhat
} saved_list_t;

static SemaphoreHandle_t s_mtx = NULL;       // bao ve toan bo bien trang thai ben duoi
static SemaphoreHandle_t s_nvs_mtx = NULL;   // chi de tuan tu hoa ghi NVS (khong giu cung s_mtx)
static QueueHandle_t     s_cmd_q = NULL;

static found_t       s_found[BT_MGR_MAX_FOUND];
static saved_list_t  s_saved;

static bool     s_target_valid = false;      // nguoi dung da chon thiet bi nay de ket noi
static bool     s_target_attempted = false;  // thu vien da bat dau noi toi no
static uint8_t  s_target_bda[6];
static char     s_target_name[BT_NAME_MAX_LEN + 1];
static uint32_t s_target_since_ms = 0;
static uint32_t s_target_limit_ms = 0;

static bool     s_connected = false;
static uint8_t  s_conn_bda[6];
static char     s_conn_name[BT_NAME_MAX_LEN + 1];
static bool     s_fail_pending = false;
// Thu vien KHONG tu quet lai sau khi ngat ket noi (log: chi con "fallback to scanning"
// moi 10s ma khong co "Starting device discovery") -> sau moi lan roi ve UNCONNECTED ta
// phai tu khoi dong lai A2DP de no quet lai (xem restart_scan()).
static bool     s_rescan_pending = false;
static uint32_t s_rescan_since_ms = 0;

static volatile bool     s_want_enabled = false;   // nguoi dung muon BT bat (dat ngay luc goi API)
static volatile bool     s_enabled = false;        // stack Bluedroid that su dang chay
static volatile uint32_t s_last_activity_ms = 0;

// Chi dung trong worker task
static bool     s_reported_connected = false;
static bool     s_list_dirty = false;
static uint32_t s_last_list_ms = 0;

enum : uint8_t {
    CMD_ENABLE = 1,
    CMD_DISABLE,
    CMD_LIST_DIRTY,
    CMD_SCAN_REQ,
    CMD_STATE,        // trang thai ket noi vua doi (worker tu doc va bao cao)
    CMD_REREPORT,     // buoc bao cao lai trang thai ket noi hien tai (dung khi nguoi dung chon lai dung thiet bi dang noi)
};

#define LOCK()    xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK()  xSemaphoreGive(s_mtx)

static inline uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
static inline bool elapsed_ms(uint32_t since, uint32_t ms) { return (uint32_t)(now_ms() - since) >= ms; }
static inline void touch_activity(void) { s_last_activity_ms = now_ms(); }

static void cmd_post(uint8_t cmd)
{
    if (s_cmd_q) xQueueSend(s_cmd_q, &cmd, 0);   // day thi bo qua: cac lenh LIST/STATE deu "idempotent"
}

// ================== TIEN ICH ==================
static void bda_to_str(const uint8_t *bda, char *out /* >= 13 */)
{
    snprintf(out, BT_ADDR_STR_LEN, "%02X%02X%02X%02X%02X%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool str_to_bda(const char *s, uint8_t *bda)
{
    if (!s || strlen(s) != 12) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_val(s[2 * i]), lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        bda[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// Ten gui qua UART dang "addr|flags|name" ngan cach bang ',' nen phai loai ',' '|' va
// ky tu dieu khien; dong thoi khong de cat dut giua 1 ky tu UTF-8.
static void sanitize_name(char *dst, size_t dst_sz, const char *src)
{
    size_t n = 0;
    if (src) {
        for (size_t i = 0; src[i] && n < dst_sz - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c < 0x20 || c == 0x7F || c == ',' || c == '|') c = ' ';
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
    if (n > 0) {
        size_t p = n - 1;
        while (p > 0 && (((unsigned char)dst[p]) & 0xC0) == 0x80) p--;
        unsigned char lead = (unsigned char)dst[p];
        if (lead >= 0xC0) {
            size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : 2;
            if (n - p < need) n = p;
        }
        while (n > 0 && dst[n - 1] == ' ') n--;
        dst[n] = '\0';
    }
}

// Bat/tat loa local (MAX98357A noi I2S voi ESP32-C3) qua chan PIN_SPEAKER_MUTE.
// GIA DINH (xem ghi chu chi tiet trong board_config.h): HIGH = loa hoat dong,
// LOW = loa tat. Dao lai 2 gia tri duoi day neu board thuc te nguoc lai.
static void apply_speaker_mute(bool bt_connected)
{
    gpio_set_level((gpio_num_t)PIN_SPEAKER_MUTE, bt_connected ? 0 : 1);
}

// ================== DANH SACH DA LUU (NVS) ==================
static int saved_find(const uint8_t *bda)   // goi khi dang giu LOCK
{
    for (int i = 0; i < s_saved.count; i++) {
        if (memcmp(s_saved.e[i].bda, bda, 6) == 0) return i;
    }
    return -1;
}

// Dua thiet bi len dau danh sach (them moi neu chua co). Goi khi dang giu LOCK.
static void saved_promote(const uint8_t *bda, const char *name)
{
    saved_entry_t entry;
    memcpy(entry.bda, bda, 6);
    int idx = saved_find(bda);
    if (idx >= 0) {
        strncpy(entry.name, s_saved.e[idx].name, sizeof(entry.name));
        entry.name[sizeof(entry.name) - 1] = '\0';
        for (int i = idx; i < s_saved.count - 1; i++) s_saved.e[i] = s_saved.e[i + 1];
        s_saved.count--;
    } else {
        entry.name[0] = '\0';
        if (s_saved.count >= BT_MGR_MAX_SAVED) s_saved.count = BT_MGR_MAX_SAVED - 1;   // bo cai cu nhat
    }
    if (name && name[0]) {
        strncpy(entry.name, name, sizeof(entry.name));
        entry.name[sizeof(entry.name) - 1] = '\0';
    }
    for (int i = s_saved.count; i > 0; i--) s_saved.e[i] = s_saved.e[i - 1];
    s_saved.e[0] = entry;
    s_saved.count++;
}

static void persist_saved(void)
{
    saved_list_t snap;
    LOCK();
    snap = s_saved;
    UNLOCK();

    xSemaphoreTake(s_nvs_mtx, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t r = nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (r == ESP_OK) {
        r = nvs_set_blob(h, BT_NVS_KEY_SAVED, &snap, sizeof(snap));
        if (r == ESP_OK) r = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mtx);
    if (r != ESP_OK) ESP_LOGW(TAG, "Luu danh sach Bluetooth vao NVS that bai: %s", esp_err_to_name(r));
}

static void load_saved(void)
{
    memset(&s_saved, 0, sizeof(s_saved));
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;   // chua co gi -> danh sach rong
    saved_list_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, BT_NVS_KEY_SAVED, &tmp, &len) == ESP_OK &&
        len == sizeof(tmp) && tmp.count <= BT_MGR_MAX_SAVED) {
        s_saved = tmp;
        for (int i = 0; i < s_saved.count; i++) s_saved.e[i].name[BT_NAME_MAX_LEN] = '\0';
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Da nap %d thiet bi Bluetooth da luu", (int)s_saved.count);
}

// ================== DANH SACH THIET BI QUET DUOC ==================
// Goi khi dang giu LOCK. Tra ve true neu danh sach hien thi can cap nhat
// (thiet bi moi hoac doi ten) - doi RSSI don thuan khong tinh.
static bool found_update(const uint8_t *bda, const char *name, int rssi, bool *is_new)
{
    uint32_t now = now_ms();
    int free_idx = -1, oldest_idx = 0;
    *is_new = false;

    for (int i = 0; i < BT_MGR_MAX_FOUND; i++) {
        if (!s_found[i].used) { if (free_idx < 0) free_idx = i; continue; }
        if (memcmp(s_found[i].bda, bda, 6) == 0) {
            bool changed = (strcmp(s_found[i].name, name) != 0);
            if (changed) {
                strncpy(s_found[i].name, name, sizeof(s_found[i].name));
                s_found[i].name[sizeof(s_found[i].name) - 1] = '\0';
            }
            s_found[i].rssi = (int8_t)rssi;
            s_found[i].last_seen_ms = now;
            return changed;
        }
        if ((uint32_t)(now - s_found[i].last_seen_ms) > (uint32_t)(now - s_found[oldest_idx].last_seen_ms)) {
            oldest_idx = i;
        }
    }

    int idx = (free_idx >= 0) ? free_idx : oldest_idx;   // day thi de len thiet bi lau khong thay nhat
    memcpy(s_found[idx].bda, bda, 6);
    strncpy(s_found[idx].name, name, sizeof(s_found[idx].name));
    s_found[idx].name[sizeof(s_found[idx].name) - 1] = '\0';
    s_found[idx].rssi = (int8_t)rssi;
    s_found[idx].last_seen_ms = now;
    s_found[idx].used = true;
    *is_new = true;
    return true;
}

static int found_find(const uint8_t *bda)   // goi khi dang giu LOCK
{
    for (int i = 0; i < BT_MGR_MAX_FOUND; i++) {
        if (s_found[i].used && memcmp(s_found[i].bda, bda, 6) == 0) return i;
    }
    return -1;
}

// ================== CALLBACK CUA THU VIEN ESP32-A2DP ==================
// Duoc goi cho TUNG thiet bi A2DP sink (loa/tai nghe) tim thay trong luc quet.
// Tra ve true = ket noi toi thiet bi nay. Ta chi tra ve true cho thiet bi ma
// nguoi dung da chon (s_target). Ham chay trong ngu canh task cua Bluedroid
// nen giu that ngan: khong ghi UART/NVS o day.
static bool on_ssid_found(const char *ssid, esp_bd_addr_t address, int rssi)
{
    char name[BT_NAME_MAX_LEN + 1];
    sanitize_name(name, sizeof(name), ssid);
    char addr_str[BT_ADDR_STR_LEN];
    bda_to_str(address, addr_str);
    if (name[0] == '\0') {
        strncpy(name, addr_str, sizeof(name));
        name[sizeof(name) - 1] = '\0';
    }

    bool is_new = false, changed, connect_now = false;
    LOCK();
    changed = found_update(address, name, rssi, &is_new);
    // CHI tra ve true DUNG 1 LAN cho moi luot ket noi (s_target_attempted). Thiet bi hay
    // duoc bao 2 lan lien tiep trong cung dot quet; neu tra ve true ca 2 lan, thu vien
    // huy quet 2 lan, lan huy thu 2 sinh ra 1 luot quet "lac" ma no cu khoi dong lai
    // mai ("Device discovery failed, continue to discover...") ke ca sau khi da
    // CONNECTED - quet lien tuc trong luc dang phat A2DP se lam am thanh bi giat.
    if (s_target_valid && !s_target_attempted && !s_connected &&
        memcmp(address, s_target_bda, 6) == 0) {
        connect_now = true;
        s_target_attempted = true;
        s_target_since_ms = now_ms();
        s_target_limit_ms = BT_CONNECT_ATTEMPT_MS;
        if (s_target_name[0] == '\0') {
            strncpy(s_target_name, name, sizeof(s_target_name));
            s_target_name[sizeof(s_target_name) - 1] = '\0';
        }
    }
    UNLOCK();

    if (is_new) ESP_LOGI(TAG, "Tim thay: \"%s\" [%s] rssi=%d", name, addr_str, rssi);
    if (changed) cmd_post(CMD_LIST_DIRTY);
    if (connect_now) ESP_LOGI(TAG, "Dang ket noi toi \"%s\" [%s]...", name, addr_str);
    return connect_now;
}

// LUU Y: ten ham set_on_connection_state_changed() va chu ky callback nay dung
// voi cac ban pschatzmann/ESP32-A2DP pho bien hien nay - hay doi chieu lai voi
// dung phien ban thu vien ban dang dung (file BluetoothA2DPSource.h trong
// thu muc components/).
static void on_connection_state_changed(esp_a2d_connection_state_t state, void *obj)
{
    ESP_LOGI(TAG, "Trang thai ket noi A2DP: %d", (int)state);
    bool notify = false;

    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        LOCK();
        if (s_target_valid) {
            memcpy(s_conn_bda, s_target_bda, 6);
            strncpy(s_conn_name, s_target_name, sizeof(s_conn_name));
        } else {
            memset(s_conn_bda, 0, 6);
            strncpy(s_conn_name, "Bluetooth", sizeof(s_conn_name));
        }
        s_conn_name[sizeof(s_conn_name) - 1] = '\0';
        s_connected = true;
        s_target_valid = false;
        s_target_attempted = false;
        s_rescan_pending = false;
        UNLOCK();
        apply_speaker_mute(true);
        notify = true;
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        LOCK();
        if (s_connected) {
            s_connected = false;
            notify = true;
        } else if (s_target_valid && s_target_attempted) {
            // da bat dau noi toi thiet bi duoc chon nhung bi ngat -> that bai
            s_target_valid = false;
            s_target_attempted = false;
            s_fail_pending = true;
            notify = true;
        }
        if (notify) {   // da ve UNCONNECTED (ngat/that bai) -> can quet lai
            s_rescan_pending = true;
            s_rescan_since_ms = now_ms();
        }
        UNLOCK();
        if (notify) apply_speaker_mute(false);
    }
    if (notify) cmd_post(CMD_STATE);
}

// Dat/bo trang thai im lang: khi bat, A2DP van giu ket noi Bluetooth nhu binh thuong
// nhung se phat ra im lang thay vi du lieu that - dung khi can mic nghe ro giong noi.
void bt_a2dp_source_set_muted(bool muted)
{
    s_muted = muted;
}

// Lay PCM mono tu ring buffer, nhan doi thanh STEREO (L=R) - thu vien yeu cau
// dau vao la PCM stereo 16-bit interleaved, nguon cua ta la mono.
static int32_t get_sound_data(uint8_t *data, int32_t len)
{
    if (data == NULL || len <= 0) return 0;

    int32_t stereo_samples_needed = len / 4;          // 4 byte = 1 mau stereo (L+R, 16-bit)

    // Dang o che do im lang (vd: luc mic dang ghi am) -> tra ve toan so 0,
    // ket noi Bluetooth van song, chi la khong phat am thanh that.
    if (s_muted || s_ringbuf == NULL) {
        memset(data, 0, stereo_samples_needed * 4);
        return stereo_samples_needed * 4;
    }

    int32_t mono_bytes_needed = stereo_samples_needed * 2;

    static uint8_t mono_tmp[2048];
    if (mono_bytes_needed > (int32_t)sizeof(mono_tmp)) {
        mono_bytes_needed = sizeof(mono_tmp);
    }

    size_t got = 0;
    uint8_t *chunk = (uint8_t *)xRingbufferReceiveUpTo(s_ringbuf, &got, 0, mono_bytes_needed);

    int32_t mono_samples_got = 0;
    if (chunk) {
        memcpy(mono_tmp, chunk, got);
        vRingbufferReturnItem(s_ringbuf, chunk);
        mono_samples_got = got / 2;
    }

    int16_t *mono = (int16_t *)mono_tmp;
    int16_t *out  = (int16_t *)data;

    int32_t i = 0;
    for (; i < mono_samples_got && i < stereo_samples_needed; i++) {
        out[2 * i]     = mono[i];   // kenh trai
        out[2 * i + 1] = mono[i];   // kenh phai - trung voi trai vi nguon la mono
    }
    for (; i < stereo_samples_needed; i++) {
        out[2 * i]     = 0;
        out[2 * i + 1] = 0;
    }

    return stereo_samples_needed * 4;
}

// ================== WORKER TASK ==================
static void do_enable(void)
{
    if (s_enabled) {
        s_list_dirty = true;
        if (s_cbs.on_power) s_cbs.on_power(true);
        return;
    }

    LOCK();
    memset(s_found, 0, sizeof(s_found));
    UNLOCK();

    ESP_LOGI(TAG, "Bat Bluetooth + bat dau quet thiet bi xung quanh...");
    a2dp_source.start();

    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "Bluedroid khong len duoc (thieu RAM / loi khoi tao?)");
        a2dp_source.end(false);
        s_want_enabled = false;
        if (s_cbs.on_error) s_cbs.on_error("INIT_FAIL");
        if (s_cbs.on_power) s_cbs.on_power(false);
        return;
    }

    s_enabled = true;
    touch_activity();
    s_list_dirty = true;
    if (s_cbs.on_power) s_cbs.on_power(true);
}

static void sync_connection_state(void);

static void do_disable(void)
{
    s_want_enabled = false;
    if (!s_enabled) {
        if (s_cbs.on_power) s_cbs.on_power(false);
        return;
    }

    ESP_LOGI(TAG, "Tat Bluetooth");
    bool was_connected;
    LOCK();
    s_target_valid = false;
    s_target_attempted = false;
    s_rescan_pending = false;
    was_connected = s_connected;
    UNLOCK();

    if (was_connected) {
        a2dp_source.disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));   // de thu vien kip gui goi ngat cho thiet bi
    }

    s_enabled = false;
    // end(false): dung A2DP/AVRC nhung CHO PHEP bat lai sau. LUU Y: Bluedroid + controller
    // van nam trong RAM (log: heap noi giam ~24KB sau lan bat dau tien va khong tra lai).
    // KHONG dung end(true): no tra han bo nho BT ve heap nhung khong bat lai duoc cho den
    // khi khoi dong lai.
    a2dp_source.end(false);
    apply_speaker_mute(false);

    LOCK();
    s_connected = false;
    memset(s_found, 0, sizeof(s_found));
    UNLOCK();

    sync_connection_state();   // bao BT_STATUS:0 neu truoc do dang ket noi
    s_list_dirty = false;
    if (s_cbs.on_power) s_cbs.on_power(false);
}

// So sanh trang thai that voi trang thai da bao cao (muc, khong phai su kien)
// de khong bao gio lam mat 1 lan doi trang thai du nhieu su kien den don dap.
static void sync_connection_state(void)
{
    bool connected, fail;
    uint8_t bda[6];
    char name[BT_NAME_MAX_LEN + 1];

    LOCK();
    connected = s_connected;
    memcpy(bda, s_conn_bda, 6);
    strncpy(name, s_conn_name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    fail = s_fail_pending;
    s_fail_pending = false;
    UNLOCK();

    if (connected != s_reported_connected) {
        s_reported_connected = connected;
        if (connected) {
            // Ket noi thanh cong -> tu dong luu (giong WiFi luu mat khau khi noi duoc).
            LOCK();
            saved_promote(bda, name);
            UNLOCK();
            persist_saved();
            ESP_LOGI(TAG, "Da ket noi + luu thiet bi \"%s\"", name);
        }
        if (s_cbs.on_status) s_cbs.on_status(connected, connected ? name : "");
        s_list_dirty = true;
    }

    if (fail) {
        ESP_LOGW(TAG, "Ket noi that bai");
        if (s_cbs.on_error) s_cbs.on_error("CONNECT_FAIL");
        s_list_dirty = true;
    }
}

// Dung lai roi bat lai A2DP de thu vien bat dau quet tro lai. Da kiem chung tren
// board that: sau end(false)+start() thu vien tu quet lai sau ~10s (heartbeat dau tien).
// Danh sach thiet bi da thay (s_found) duoc GIU NGUYEN, chi het han theo TTL.
static void restart_scan(void)
{
    ESP_LOGI(TAG, "Khoi dong lai A2DP de quet lai (thu vien khong tu quet lai sau khi ngat ket noi)");
    a2dp_source.end(false);
    vTaskDelay(pdMS_TO_TICKS(300));
    a2dp_source.start();

    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "Bluedroid khong len lai duoc sau khi khoi dong lai A2DP");
        a2dp_source.end(false);
        s_enabled = false;
        s_want_enabled = false;
        if (s_cbs.on_error) s_cbs.on_error("INIT_FAIL");
        if (s_cbs.on_power) s_cbs.on_power(false);
    }
}

static void housekeeping(void)
{
    bool fail_now = false, idle_off = false, do_rescan = false;

    LOCK();
    // Thiet bi lau khong thay -> bo khoi danh sach "xung quanh"
    for (int i = 0; i < BT_MGR_MAX_FOUND; i++) {
        if (!s_found[i].used) continue;
        if (s_connected && memcmp(s_found[i].bda, s_conn_bda, 6) == 0) continue;
        if (elapsed_ms(s_found[i].last_seen_ms, BT_FOUND_TTL_MS)) {
            s_found[i].used = false;
            s_list_dirty = true;
        }
    }
    // Het thoi gian cho ket noi
    if (s_target_valid && !s_connected && elapsed_ms(s_target_since_ms, s_target_limit_ms)) {
        s_target_valid = false;
        s_target_attempted = false;
        fail_now = true;
    }
    // Da ngat >= 1s (cho ACL ngat han) va khong dang noi/dang thu noi -> quet lai.
    if (s_rescan_pending) {
        if (s_connected || !s_enabled) {
            s_rescan_pending = false;
        } else if (!s_target_attempted && elapsed_ms(s_rescan_since_ms, 1000)) {
            s_rescan_pending = false;
            do_rescan = true;
        }
    }
    // Dang quet ma khong ai dung toi -> tu tat de khoi chiem radio mai (anh huong WiFi)
    if (s_enabled && !s_connected && !s_target_valid &&
        elapsed_ms(s_last_activity_ms, BT_SCAN_IDLE_TIMEOUT_MS)) {
        idle_off = true;
    }
    UNLOCK();

    if (fail_now) {
        ESP_LOGW(TAG, "Het thoi gian cho ket noi");
        if (s_cbs.on_error) s_cbs.on_error("CONNECT_FAIL");
        s_list_dirty = true;
    }
    if (idle_off) {
        ESP_LOGW(TAG, "Quet qua %d giay khong co thao tac -> tu tat Bluetooth", BT_SCAN_IDLE_TIMEOUT_MS / 1000);
        do_disable();
    } else if (do_rescan) {
        restart_scan();
    }
}

static void flush_list(void)
{
    if (!s_list_dirty || !s_enabled) return;
    if (!elapsed_ms(s_last_list_ms, BT_LIST_MIN_INTERVAL_MS)) return;
    s_list_dirty = false;
    s_last_list_ms = now_ms();

    static bt_device_info_t devs[BT_LIST_MAX_ROWS];   // static: giu ngoai stack, chi worker dung
    int n = bt_a2dp_source_get_devices(devs, BT_LIST_MAX_ROWS);
    if (s_cbs.on_list) s_cbs.on_list(devs, n);
}

static void bt_worker_task(void *arg)
{
    for (;;) {
        uint8_t cmd = 0;
        TickType_t wait = pdMS_TO_TICKS(s_list_dirty ? 150 : 500);
        if (xQueueReceive(s_cmd_q, &cmd, wait) == pdTRUE) {
            switch (cmd) {
            case CMD_ENABLE:     do_enable();  break;
            case CMD_DISABLE:    do_disable(); break;
            case CMD_LIST_DIRTY: s_list_dirty = true; break;
            case CMD_REREPORT:   s_reported_connected = false; break;   // sync_connection_state() ngay ben duoi se bao lai neu dang noi
            case CMD_SCAN_REQ:
                if (s_enabled) {
                    s_list_dirty = true;
                    s_last_list_ms = now_ms() - BT_LIST_MIN_INTERVAL_MS;   // day ngay, khong debounce
                }
                break;
            default: break;
            }
        }
        sync_connection_state();
        housekeeping();
        flush_list();
    }
}

// ================== API ==================
extern "C" esp_err_t bt_a2dp_source_init(RingbufHandle_t ringbuf)
{
    s_ringbuf = ringbuf;

    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = (1ULL << PIN_SPEAKER_MUTE);
    io_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_cfg);
    apply_speaker_mute(false);   // chua ket noi Bluetooth luc khoi dong -> loa local hoat dong binh thuong

    s_mtx = xSemaphoreCreateMutex();
    s_nvs_mtx = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(16, sizeof(uint8_t));
    if (!s_mtx || !s_nvs_mtx || !s_cmd_q) return ESP_ERR_NO_MEM;

    load_saved();

    a2dp_source.set_local_name("ESP32-BT-Relay");
    a2dp_source.set_data_callback(get_sound_data);
    a2dp_source.set_on_connection_state_changed(on_connection_state_changed);
    a2dp_source.set_ssid_callback(on_ssid_found);   // nhan MOI thiet bi tim thay, tu quyet dinh co noi hay khong
    a2dp_source.set_auto_reconnect(false);          // ta tu quan ly ket noi, khong de thu vien tu noi lai thiet bi cu

    // Tao worker TRUOC khi bat Bluetooth (luc heap con nguyen ven nhat).
    if (xTaskCreate(bt_worker_task, "bt_worker", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Da cau hinh A2DP source (Bluetooth dang TAT, cho lenh tu ESP32-C3)");
    return ESP_OK;
}

extern "C" void bt_a2dp_source_set_callbacks(const bt_a2dp_callbacks_t *cbs)
{
    if (cbs) s_cbs = *cbs;
}

extern "C" esp_err_t bt_a2dp_source_enable(void)
{
    if (!s_cmd_q) return ESP_ERR_INVALID_STATE;
    s_want_enabled = true;
    touch_activity();
    cmd_post(CMD_ENABLE);
    return ESP_OK;
}

extern "C" void bt_a2dp_source_disable(void)
{
    s_want_enabled = false;
    cmd_post(CMD_DISABLE);
}

extern "C" bool bt_a2dp_source_is_enabled(void)
{
    return s_want_enabled;
}

extern "C" void bt_a2dp_source_request_list(void)
{
    touch_activity();
    cmd_post(CMD_SCAN_REQ);
}

extern "C" esp_err_t bt_a2dp_source_connect_to(const char *addr_hex)
{
    uint8_t bda[6];
    if (!str_to_bda(addr_hex, bda)) return ESP_ERR_INVALID_ARG;
    if (!s_want_enabled) return ESP_ERR_INVALID_STATE;

    bool disconnect_current = false;
    LOCK();
    if (s_connected && memcmp(s_conn_bda, bda, 6) == 0) {
        UNLOCK();
        ESP_LOGI(TAG, "Da ket noi san toi thiet bi nay");
        cmd_post(CMD_REREPORT);
        return ESP_OK;
    }

    s_target_valid = true;
    s_target_attempted = false;
    memcpy(s_target_bda, bda, 6);
    s_target_name[0] = '\0';
    int fi = found_find(bda);
    if (fi >= 0) {
        strncpy(s_target_name, s_found[fi].name, sizeof(s_target_name));
    } else {
        int si = saved_find(bda);
        if (si >= 0) strncpy(s_target_name, s_saved.e[si].name, sizeof(s_target_name));
    }
    s_target_name[sizeof(s_target_name) - 1] = '\0';
    s_target_since_ms = now_ms();
    s_target_limit_ms = BT_CONNECT_WAIT_MS;
    disconnect_current = s_connected;   // dang noi thiet bi KHAC -> ngat truoc roi moi noi cai moi
    UNLOCK();

    touch_activity();
    ESP_LOGI(TAG, "Yeu cau ket noi toi [%s] (cho thay lai thiet bi trong luc quet)", addr_hex);
    if (disconnect_current) a2dp_source.disconnect();
    return ESP_OK;
}

extern "C" void bt_a2dp_source_disconnect(void)
{
    bool was_connected;
    LOCK();
    s_target_valid = false;
    s_target_attempted = false;
    was_connected = s_connected;
    UNLOCK();

    touch_activity();
    if (was_connected && s_enabled) {
        ESP_LOGI(TAG, "Ngat ket noi Bluetooth theo yeu cau tu ESP32-C3");
        a2dp_source.disconnect();
    }
}

extern "C" esp_err_t bt_a2dp_source_forget(const char *addr_hex)
{
    uint8_t bda[6];
    if (!str_to_bda(addr_hex, bda)) return ESP_ERR_INVALID_ARG;

    bool was_connected = false, removed = false;
    LOCK();
    int idx = saved_find(bda);
    if (idx >= 0) {
        for (int i = idx; i < s_saved.count - 1; i++) s_saved.e[i] = s_saved.e[i + 1];
        s_saved.count--;
        removed = true;
    }
    if (s_target_valid && memcmp(s_target_bda, bda, 6) == 0) {
        s_target_valid = false;
        s_target_attempted = false;
    }
    was_connected = s_connected && memcmp(s_conn_bda, bda, 6) == 0;
    UNLOCK();

    if (removed) persist_saved();
    if (was_connected && s_enabled) a2dp_source.disconnect();
    touch_activity();
    cmd_post(CMD_LIST_DIRTY);
    ESP_LOGI(TAG, "Quen thiet bi [%s] (%s)", addr_hex, removed ? "da xoa" : "khong co trong danh sach");
    return ESP_OK;
}

extern "C" int bt_a2dp_source_get_devices(bt_device_info_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;

    LOCK();
    // 1) Thiet bi da luu (dung gan nhat truoc)
    for (int i = 0; i < s_saved.count && n < max; i++) {
        const saved_entry_t *s = &s_saved.e[i];
        bt_device_info_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        bda_to_str(s->bda, d->addr);
        strncpy(d->name, s->name, sizeof(d->name) - 1);
        d->saved = true;
        d->connected = s_connected && memcmp(s_conn_bda, s->bda, 6) == 0;
        int fi = found_find(s->bda);
        if (fi >= 0) {
            d->in_range = true;
            d->rssi = s_found[fi].rssi;
            if (s_found[fi].name[0]) {   // ten moi nhat tu lan quet (neu thiet bi da doi ten)
                strncpy(d->name, s_found[fi].name, sizeof(d->name) - 1);
            }
        }
        if (d->connected) d->in_range = true;
        if (d->name[0] == '\0') strncpy(d->name, d->addr, sizeof(d->name) - 1);
    }

    // 2) Thiet bi xung quanh CHUA luu, sap theo RSSI giam dan
    int order[BT_MGR_MAX_FOUND];
    int cnt = 0;
    for (int i = 0; i < BT_MGR_MAX_FOUND; i++) {
        if (!s_found[i].used || saved_find(s_found[i].bda) >= 0) continue;
        int j = cnt++;
        while (j > 0 && s_found[order[j - 1]].rssi < s_found[i].rssi) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    for (int k = 0; k < cnt && n < max; k++) {
        const found_t *f = &s_found[order[k]];
        bt_device_info_t *d = &out[n++];
        memset(d, 0, sizeof(*d));
        bda_to_str(f->bda, d->addr);
        strncpy(d->name, f->name, sizeof(d->name) - 1);
        d->rssi = f->rssi;
        d->in_range = true;
    }
    UNLOCK();
    return n;
}
