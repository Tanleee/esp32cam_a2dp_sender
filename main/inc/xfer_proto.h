#pragma once
// Giao thuc truyen file WROVER -> C3 qua UART (DUNG CHUNG, file nay phai GIONG HET o 2 ben).
//
// Dong text (ket thuc '\n'):
//   WROVER -> C3 : FILE_BEGIN:<A|W>:<name>:<size>   (A = audio WAV, W = wallpaper RGB565)
//                  FILE_ABORT
//   C3 -> WROVER : FILE_READY | FILE_ERR:<code>
//                  FILE_ACK:<offset_sau_khung> | FILE_NAK:<offset_mong_doi>
//                  FILE_DONE | FILE_ERR:<code>
// Khung nhi phan WROVER -> C3 (sau FILE_READY, stop-and-wait: moi khung doi ACK):
//   [0xA5][0x5A][offset u32 LE][len u16 LE][payload len byte][crc16 u16 LE]
//   crc16 = CRC-16/CCITT-FALSE tinh tren (offset, len, payload).
#include <stdint.h>
#include <stddef.h>

#define XFER_MAGIC0        0xA5
#define XFER_MAGIC1        0x5A
#define XFER_MAX_PAYLOAD   2048
#define XFER_HDR_LEN       8       // magic(2) + offset(4) + len(2)
#define XFER_FRAME_MAX     (XFER_HDR_LEN + XFER_MAX_PAYLOAD + 2)
#define WALLPAPER_BYTES    153600u // 240 * 320 * 2

static inline uint16_t xfer_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
