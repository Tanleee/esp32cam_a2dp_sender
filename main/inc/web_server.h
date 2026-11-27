#pragma once
#include "esp_err.h"

// HTTP server (cong 80) phuc vu giao dien web (nhung san trong firmware) + REST API
// /api/status, /api/tracks, /api/cmd, /api/upload/audio, /api/upload/wallpaper,
// va mDNS "musicplayer.local". Goi 1 lan sau wifi_mgr_init().
esp_err_t web_server_start(void);
