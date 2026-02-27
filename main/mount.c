/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server Example, SD card / FATFS mount functions.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_partition.h"
#include "mount.h"
#include "MMROFS.h"

static const char *TAG = "MOUNT";

esp_err_t mount_sd(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing SD card");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    esp_err_t ret;
    sdmmc_card_t* card;

    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 3,
        .miso_io_num = 1,
        .sclk_io_num = 0,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return ret;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = 2;
    slot_config.host_id = host.slot;
    ret = esp_vfs_fat_sdspi_mount(base_path, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK){
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

/* Function to initialize SPIFFS */
esp_err_t mount_spiffs(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");
    const char* partition_label = "storage";

    esp_vfs_spiffs_conf_t conf = {
        .base_path = base_path,
        .partition_label = partition_label,
        .max_files = 5,   // This sets the maximum number of files that can be open at the same time
        .format_if_mount_failed = true
    };
    
        // First, try to find and erase the SPIFFS partition if something's wrong
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 
        partition_label
    );
    
    if (part != NULL) {
        ESP_LOGI(TAG, "Partition '%s' - Address: 0x%x, Length: 0x%x (%u bytes)", 
                    part->label, part->address, part->size, part->size);
    }

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

/* Function to initialize FATFS */
esp_err_t mount_fatfs(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing FATFS");
    const char* partition_label = "storage";

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 4096
    };
    
    // First, try to find and erase the FATFS partition if something's wrong
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_FAT, 
        partition_label
    );
    
    if (part != NULL) {
        ESP_LOGI(TAG, "Partition '%s' - Address: 0x%x, Length: 0x%x (%u bytes)", 
                    part->label, part->address, part->size, part->size);
    }

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(base_path, partition_label, &mount_config, &wl_handle);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format FATFS filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find FATFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize FATFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "FATFS mounted successfully at %s", base_path);
    return ESP_OK;
}

/* Function to initialize MMROFS */
esp_err_t mount_mmrofs(const char* base_path)
{
    ESP_LOGI(TAG, "Initializing MMROFS");
    const char* partition_label = "storage";

    mmrofs_mount_cfg_t cfg = {
        .base_path = base_path,
        .partition_label = partition_label,
        .max_files = 8,
    };

    ESP_ERROR_CHECK(mmrofs_register_vfs(&cfg));

    ESP_LOGI(TAG, "MMROFS mounted successfully at %s", base_path);
    return ESP_OK;
}

