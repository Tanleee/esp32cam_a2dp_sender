#pragma once
#include "driver/uart.h"

// File gom cac chan GPIO/thong so phan cung cua board ESP32 classic (board
// relay Bluetooth + xu ly AI). Dung lai dung theo file ban da gui (da them
// PIN_SPEAKER_MUTE moi cho tinh nang tat loa local khi Bluetooth ket noi).

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
#define UART_TX_PIN 	  22
#define UART_RX_PIN       23
#define UART_BAUD         2000000   // PHAI giong UART_BAUD ben C3 (uart_link.c). Neu loi duong truyen, ha xuong 460800 o CA 2 BEN.

// --- 2 nut nhan tang/giam am luong (chuyen tu ESP32-C3 sang board nay) ---
#define PIN_BTN_VOL_UP     18
#define PIN_BTN_VOL_DOWN   19

#define PIN_SPEAKER_MUTE   4

#define PIN_BACKLIGHT_PWM   14

// --- Nut nhan tat/bat man hinh (MOI) ---
// GPIO35 chi la chan VAO (khong co pull-up/pull-down noi) -> bat buoc dung dien tro keo
// xuong NGOAI. Nhan = muc HIGH. Xem volume_button.c (chung ISR + debounce voi 2 nut am luong).
#define PIN_BTN_SCREEN     35
