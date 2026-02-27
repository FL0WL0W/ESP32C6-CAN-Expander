#ifndef MMROFS_H
#define MMROFS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_path;
    const char *partition_label;
    int max_files;
} mmrofs_mount_cfg_t;

esp_err_t mmrofs_register_vfs(const mmrofs_mount_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif