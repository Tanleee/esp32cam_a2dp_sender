#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t mic_capture_init(void);
size_t mic_capture_record(uint8_t *out_buf, size_t max_bytes);

// Yeu cau dung ghi am som (vd: ESP32-C3 gui REC_STOP vi nguoi dung bam dung
// thu cong giua chung, khong doi VAD tu dong dung). Co hieu luc trong lan
// mic_capture_record() dang chay (hoac lan ke tiep neu goi truoc khi bat dau).
void mic_capture_request_stop(void);
