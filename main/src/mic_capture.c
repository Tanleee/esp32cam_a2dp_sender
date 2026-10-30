#include "mic_capture.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <math.h>
#include <stdbool.h>

static const char *TAG = "MIC_CAP";

#define PIN_MIC_SCK  32
#define PIN_MIC_WS   33
#define PIN_MIC_SD   34

#define MIC_SAMPLE_RATE   8000
#define MIC_FRAME_MS      32
#define MIC_FRAME_BYTES   (MIC_SAMPLE_RATE * MIC_FRAME_MS / 1000 * 2)

#define VAD_SILENCE_MS_TO_STOP           1200
#define VAD_ENERGY_THRESHOLD             400
#define VAD_MAX_FRAMES_WAIT_FOR_SPEECH   150

static i2s_chan_handle_t s_rx_chan = NULL;

// Co yeu cau dung ghi am som, set tu uart_link khi nhan duoc REC_STOP tu C3.
static volatile bool s_stop_requested = false;

esp_err_t mic_capture_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) return ret;

    return i2s_channel_enable(s_rx_chan);
}

void mic_capture_request_stop(void)
{
    s_stop_requested = true;
}

static int32_t frame_energy(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += (int32_t)samples[i] * (int32_t)samples[i];
    }
    return (int32_t)sqrtf((float)(sum / (int64_t)count));
}

size_t mic_capture_record(uint8_t *out_buf, size_t max_bytes)
{
    size_t total = 0;
    bool speech_started = false;
    int silence_frames = 0;
    int frames_waited = 0;
    uint8_t frame[MIC_FRAME_BYTES];
    size_t dummy;

    // Reset co dung som tu lan ghi am truoc, tranh dinh co cu.
    s_stop_requested = false;

    i2s_channel_read(s_rx_chan, frame, sizeof(frame), &dummy, pdMS_TO_TICKS(200));

    while (total + MIC_FRAME_BYTES <= max_bytes) {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(s_rx_chan, frame, sizeof(frame), &bytes_read, pdMS_TO_TICKS(500));
        if (r != ESP_OK || bytes_read == 0) continue;

        int32_t energy = frame_energy((int16_t *)frame, bytes_read / 2);

        // Nguoi dung (qua ESP32-C3) yeu cau dung ghi am thu cong giua chung.
        if (s_stop_requested) {
            if (!speech_started) {
                ESP_LOGW(TAG, "Nhan REC_STOP truoc khi phat hien giong noi, huy ghi am");
                return 0;
            }
            ESP_LOGI(TAG, "Nhan REC_STOP, ket thuc ghi am som");
            break;
        }

        if (!speech_started) {
            if (energy > VAD_ENERGY_THRESHOLD) {
                speech_started = true;
            } else if (++frames_waited > VAD_MAX_FRAMES_WAIT_FOR_SPEECH) {
                ESP_LOGW(TAG, "Khong phat hien giong noi, huy ghi am");
                return 0;
            }
        }

        memcpy(out_buf + total, frame, bytes_read);
        total += bytes_read;

        if (speech_started) {
            if (energy < VAD_ENERGY_THRESHOLD) {
                silence_frames++;
                if (silence_frames * MIC_FRAME_MS >= VAD_SILENCE_MS_TO_STOP) break;
            } else {
                silence_frames = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Ghi am xong: %u byte (~%.1fs)", (unsigned)total, total / 2.0 / MIC_SAMPLE_RATE);
    return total;
}
