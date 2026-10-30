#pragma once
#include "driver/uart.h"

// File gom cac chan GPIO/thong so phan cung cua board ESP32 classic (board
// relay Bluetooth + xu ly AI). Dung lai dung theo file ban da gui (da them
// PIN_SPEAKER_MUTE moi cho tinh nang tat loa local khi Bluetooth ket noi).
//
// LUU Y: cac file mic_capture.c / i2s_slave_rx.c / volume_button.c hien tai
// van TU KHAI BAO rieng cac macro PIN_MIC_*/PIN_I2S_*_IN/PIN_BTN_VOL_* (gia
// tri giong het o day) thay vi include file nay - minh giu nguyen nhu vay de
// khong dong vao code dang chay on dinh. File nay dung lam noi tham chieu
// chung + duoc cac file MOI (system_ctrl.c, bt_a2dp_source.cpp) include truc
// tiep. Neu ban muon gop hoan toan ve 1 noi duy nhat, noi minh se refactor
// lai 3 file kia.

// --- I2S0 slave RX: "nghe trom" bus I2S cua ESP32-C3 (toi MAX98357A cuc bo) ---
#define PIN_I2S_BCLK_IN   25
#define PIN_I2S_WS_IN     26
#define PIN_I2S_DIN_IN    27

// --- I2S1 master: mic INMP441 ---
#define PIN_MIC_SCK       32
#define PIN_MIC_WS        33
#define PIN_MIC_SD        34

// --- UART noi voi ESP32-C3 ---
#define UART_PORT         UART_NUM_2
#define UART_TX_PIN       17
#define UART_RX_PIN       16
#define UART_BAUD         115200

// --- 2 nut nhan tang/giam am luong (chuyen tu ESP32-C3 sang board nay) ---
#define PIN_BTN_VOL_UP     18
#define PIN_BTN_VOL_DOWN   19

// --- MOI: dieu khien tat/mo loa local (MAX98357A noi voi I2S cua ESP32-C3)
// qua chan SD (Shutdown) cua MAX98357A. Muc dich: khi ESP32 classic ket noi
// Bluetooth (A2DP) thanh cong voi loa ngoai "LE208", keo chan nay XUONG THAP
// de tat loa local, tranh phat chong 2 noi cung luc; khi Bluetooth ngat ket
// noi thi keo len lai de loa local tiep tuc phat.
// GIA DINH (hay kiem tra lai dung board MAX98357A ban dang dung): SD = HIGH
// (hoac tha noi, thuong duoc keo len san bang dien tro ngoai tren board) ->
// loa HOAT DONG; SD = LOW -> loa TAT. Neu board cua ban dung nguoc lai, chi
// can dao gia tri trong bt_a2dp_source.cpp (ham apply_speaker_mute()).
#define PIN_SPEAKER_MUTE   4
