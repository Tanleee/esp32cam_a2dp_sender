#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

// Khoi tao I2S0 o che do SLAVE, chi nhan (RX) - doc du lieu PCM mono 16-bit
// 44100Hz tu duong I2S dang noi song song voi MAX98357A tren ESP32-C3.
// Du lieu doc duoc se duoc day lien tuc vao ringbuf (do noi khoi tao truyen vao).
esp_err_t i2s_slave_rx_init(RingbufHandle_t ringbuf);
