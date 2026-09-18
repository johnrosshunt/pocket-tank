#pragma once
#include <stdio.h>
extern int host_log;                             /* imu_host.c: print the driver's log when set */
#define ESP_LOGI(tag, fmt, ...) do { if (host_log) printf("  [%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGI
