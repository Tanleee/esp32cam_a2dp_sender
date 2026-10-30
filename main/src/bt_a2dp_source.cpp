#include "BluetoothA2DPSource.h"
#include "bt_a2dp_source.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "BT_A2DP_SRC";
#define REMOTE_DEVICE_NAME "LE208"

static BluetoothA2DPSource a2dp_source;
static RingbufHandle_t     s_ringbuf = NULL;
static volatile bool       s_muted = false;
static volatile bool       s_started = false;   // da goi a2dp_source.start() chua
static bt_a2dp_status_cb_t s_status_cb = NULL;

// Bat/tat loa local (MAX98357A noi I2S voi ESP32-C3) qua chan PIN_SPEAKER_MUTE.
// GIA DINH (xem ghi chu chi tiet trong board_config.h): HIGH = loa hoat dong,
// LOW = loa tat. Dao lai 2 gia tri duoi day neu board thuc te nguoc lai.
static void apply_speaker_mute(bool bt_connected)
{
    gpio_set_level((gpio_num_t)PIN_SPEAKER_MUTE, bt_connected ? 0 : 1);
}

// Callback cua thu vien ESP32-A2DP khi trang thai ket noi Bluetooth thay doi.
// LUU Y: ten ham set_on_connection_state_changed() va chu ky callback nay dung
// voi cac ban pschatzmann/ESP32-A2DP pho bien hien nay - hay doi chieu lai voi
// dung phien ban thu vien ban dang dung (file BluetoothA2DPCommon.h/
// BluetoothA2DPSource.h trong thu muc components/), vi minh khong truy cap
// truc tiep duoc source code cua ban de kiem tra 100%.
static void on_connection_state_changed(esp_a2d_connection_state_t state, void *obj)
{
    bool connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
    ESP_LOGI(TAG, "Trang thai ket noi Bluetooth: %d (connected=%d)", (int)state, connected);

    apply_speaker_mute(connected);
    if (s_status_cb) s_status_cb(connected, connected ? REMOTE_DEVICE_NAME : "");
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

extern "C" esp_err_t bt_a2dp_source_init(RingbufHandle_t ringbuf)
{
    s_ringbuf = ringbuf;

    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = (1ULL << PIN_SPEAKER_MUTE);
    io_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_cfg);
    apply_speaker_mute(false);   // chua ket noi Bluetooth luc khoi dong -> loa local hoat dong binh thuong

    a2dp_source.set_local_name("ESP32-BT-Relay");
    a2dp_source.set_data_callback(get_sound_data);
    a2dp_source.set_on_connection_state_changed(on_connection_state_changed);

    // KHONG goi a2dp_source.start() o day nua - chi cau hinh san, cho lenh
    // BT_CONNECT tu ESP32-C3 (xem bt_a2dp_source_connect()).
    ESP_LOGI(TAG, "Da cau hinh A2DP source (chua quet/ket noi, cho lenh tu ESP32-C3)");
    return ESP_OK;
}

void bt_a2dp_source_connect(void)
{
    if (s_started) {
        ESP_LOGI(TAG, "A2DP da duoc khoi dong truoc do, bo qua");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "Bat dau quet + ket noi toi '%s' theo yeu cau tu ESP32-C3...", REMOTE_DEVICE_NAME);
    a2dp_source.start(REMOTE_DEVICE_NAME);
}

void bt_a2dp_source_disconnect(void)
{
    if (!s_started) return;
    ESP_LOGI(TAG, "Ngat ket noi Bluetooth theo yeu cau tu ESP32-C3");
    a2dp_source.disconnect();
}

void bt_a2dp_source_set_status_callback(bt_a2dp_status_cb_t cb)
{
    s_status_cb = cb;
}
