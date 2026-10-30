#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// pcm_data: PCM 16-bit mono o pcm_sample_rate Hz. playlist_csv: danh sach bai hat
// cach nhau boi dau phay (lay nguyen tu ESP32-C3). out_song phai cap phat san.
esp_err_t ai_client_recommend_song(const uint8_t *pcm_data, size_t pcm_len,
                                    int pcm_sample_rate,
                                    const char *playlist_csv,
                                    char *out_song, size_t out_song_sz);
