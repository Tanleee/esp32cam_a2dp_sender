#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Khoi tao A2DP source (dung thu vien ESP32-A2DP): tu dong quet, ket noi toi
// thiet bi ten "LE208", lay PCM mono tu ringbuf, nhan doi thanh stereo roi
// gui qua Bluetooth.
esp_err_t bt_a2dp_source_init(RingbufHandle_t ringbuf);

#ifdef __cplusplus
}
#endif