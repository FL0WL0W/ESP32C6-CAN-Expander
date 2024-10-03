#include "esp_err.h"

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t start_http_server(const char *base_path);

#ifdef __cplusplus
}
#endif

#endif