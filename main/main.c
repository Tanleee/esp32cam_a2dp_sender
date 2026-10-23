#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

#include "i2s_slave_rx.h"
#include "bt_a2dp_source.h"

static const char *TAG = "MAIN";

// Hap thu do lech (jitter) giua toc do doc I2S va toc do A2DP/SBC tieu thu du lieu
#define RINGBUF_SIZE   (16 * 1024)

void app_main(void)
{
	esp_log_level_set("BT_L2CAP", ESP_LOG_NONE);
	
    RingbufHandle_t ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!ringbuf) {
        ESP_LOGE(TAG, "Tao ring buffer that bai");
        return;
    }

    if (i2s_slave_rx_init(ringbuf) != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao I2S RX that bai");
        return;
    }

    if (bt_a2dp_source_init(ringbuf) != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao Bluetooth A2DP that bai");
        return;
    }
}