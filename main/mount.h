#include "esp_err.h"

#ifndef MOUNT_H
#define MOUNT_H

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mount_sd(const char *base_path);
esp_err_t mount_spiffs(const char *base_path);

#ifdef __cplusplus
}
#endif

#endif