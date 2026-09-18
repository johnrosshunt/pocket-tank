/* host stand-in for ESP-IDF's I2C master (imu_host.c): just what the IMU driver calls */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct i2c_bus *i2c_master_bus_handle_t;
typedef struct i2c_dev *i2c_master_dev_handle_t;
enum { I2C_ADDR_BIT_LEN_7 = 0 };
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t n, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, int timeout_ms);
