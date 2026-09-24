// wM-Buster ADV — hardware configuration
// M5Stack Cardputer-ADV + Cap LoRa-1262 (SX1262) / Hydra RF (CC1101) — GPL-3.0
#pragma once

#include <stdint.h>

// ---- SX1262 radio (Cap LoRa-1262) ----
#define PIN_LORA_NSS    5
#define PIN_LORA_IRQ    4       // DIO1
#define PIN_LORA_RESET  3
#define PIN_LORA_BUSY   6

// ---- CC1101 radio (Hydra RF) ----
#define PIN_CC1101_CS   13
#define PIN_CC1101_IRQ  5       // GDO0

// ---- Shared SPI bus (radio + microSD) ----
#define PIN_SPI_SCK     40
#define PIN_SPI_MOSI    14
#define PIN_SPI_MISO    39
#define PIN_SD_CS       12

// ---- GNSS on the Cap LoRa-1262 (AT6668 / ATGM336H) ----
#define PIN_GNSS_RX     15      // ESP32 RX <- module TX
#define PIN_GNSS_TX     13      // ESP32 TX -> module RX
#define GNSS_BAUD       115200  // 9600 is tried automatically

// ---- Internal I2C (keyboard TCA8418, RF switch PI4IOE5V6408) ----
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9

// PI4IOE5V6408 expander on the Cap LoRa-1262: P0 must be driven high or the
// antenna path loses ~30 dB. Registers: 0x03 direction (1 = output),
// 0x05 output state, 0x07 output high impedance (1 = hi-Z).
#define PI4IOE_I2C_ADDR     0x43
#define PI4IOE_REG_IO_DIR   0x03
#define PI4IOE_REG_OUTPUT   0x05
#define PI4IOE_PIN_RF_SW    0
