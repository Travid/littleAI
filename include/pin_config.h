#pragma once

// Pin definitions for Waveshare ESP32-S3-Touch-AMOLED-1.8
// Display: SH8601 over QSPI
// Touch: FT3168 over I2C

// QSPI pins
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12

#define LCD_HRES 368
#define LCD_VRES 448

// I2C touch
#define I2C_SDA 15
#define I2C_SCL 14
#define TP_INT  21

// Optional (if you later want to use these peripherals)
#define I2S_MCK_IO 16
#define I2S_BCK_IO 9
#define I2S_DI_IO  10
#define I2S_WS_IO  45
#define I2S_DO_IO  8
#define PA_ENABLE  46
