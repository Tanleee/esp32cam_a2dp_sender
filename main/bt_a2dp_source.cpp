#include "BluetoothA2DPSource.h"
#include "bt_a2dp_source.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "BT_A2DP_SRC";
#define REMOTE_DEVICE_NAME "LE208"

static BluetoothA2DPSource a2dp_source;
static RingbufHandle_t     s_ringbuf = NULL;

// Lay PCM mono tu ring buffer, nhan doi thanh STEREO (L=R) - thu vien yeu cau
// dau vao la PCM stereo 16-bit interleaved, nguon cua ta la mono.
static int32_t get_sound_data(uint8_t *data, int32_t len)
{
    if (data == NULL || len <= 0 || s_ringbuf == NULL) return 0;

    int32_t stereo_samples_needed = len / 4;          // 4 byte = 1 mau stereo (L+R, 16-bit)
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

    a2dp_source.set_local_name("ESP32-CAM-Relay");
    a2dp_source.set_data_callback(get_sound_data);
    a2dp_source.start(REMOTE_DEVICE_NAME);   // thu vien tu quet + ket noi + goi MEDIA_CTRL_START

    ESP_LOGI(TAG, "Da khoi dong A2DP source, dang tim '%s'...", REMOTE_DEVICE_NAME);
    return ESP_OK;
}