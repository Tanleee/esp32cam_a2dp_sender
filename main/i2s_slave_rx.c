#include "i2s_slave_rx.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "I2S_RX";
static i2s_chan_handle_t s_rx_chan = NULL;
static RingbufHandle_t   s_ringbuf = NULL;

// Chan I2S nhan - noi song song voi duong I2S hien tai cua ESP32-C3 den MAX98357A
// (truoc day la HS2_CLK/HS2_CMD/HS2_DATA3 - khong dung camera/SD onboard nen ranh)
#define PIN_I2S_BCLK_IN  14
#define PIN_I2S_WS_IN    15
#define PIN_I2S_DIN_IN   13

#define RX_READ_CHUNK   1024   // so byte doc moi lan tu I2S DMA

static void i2s_rx_task(void *arg)
{
    uint8_t buf[RX_READ_CHUNK];
    size_t bytes_read;

    while (1) {
        esp_err_t ret = i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        // Day vao ring buffer dung chung voi phia Bluetooth.
        // Neu ring buffer dang day (Bluetooth chua kip tieu thu kip), cho toi da 20ms
        // roi bo qua chunk nay - tranh task nay bi block vo han, vi ESP32-C3 van
        // dang phat lien tuc, khong the "doi" phia nhan duoc.
        xRingbufferSend(s_ringbuf, buf, bytes_read, pdMS_TO_TICKS(20));
    }
}

esp_err_t i2s_slave_rx_init(RingbufHandle_t ringbuf)
{
    s_ringbuf = ringbuf;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);   // chi tao kenh RX
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Tao kenh I2S that bai: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        // O che do SLAVE, clk_cfg khong quyet dinh toc do thuc (lay tu BCLK/WS
        // ben ngoai do ESP32-C3 phat ra), nhung van phai khai bao du truong.
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        // MONO - phai khop dung voi cau hinh TX ben ESP32-C3 (file WAV da chuan
        // hoa mono), vi mode nay chi co slot TRAI mang du lieu that tren bus.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK_IN,
            .ws   = PIN_I2S_WS_IN,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DIN_IN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao I2S std mode that bai: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) return ret;

    xTaskCreate(i2s_rx_task, "i2s_rx", 4096, NULL, 5, NULL);
    return ESP_OK;
}