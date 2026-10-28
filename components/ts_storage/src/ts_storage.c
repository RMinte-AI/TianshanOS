/**
 * @file ts_storage.c
 * @brief TianShanOS Storage Management Implementation
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_storage.h"
#include "ts_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "esp_heap_caps.h"

/* PSRAM 优先分配宏 */
#define TS_STG_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })

#define TAG "ts_storage"

/*===========================================================================*/
/*                          Private Data                                      */
/*===========================================================================*/

static struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    
    bool spiffs_mounted;
    char spiffs_mount_point[32];
    
    bool sd_mounted;
    char sd_mount_point[32];
} s_storage = {0};

/*===========================================================================*/
/*                          Core API                                          */
/*===========================================================================*/

esp_err_t ts_storage_init(void)
{
    if (s_storage.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_storage.mutex = xSemaphoreCreateMutex();
    if (s_storage.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    s_storage.initialized = true;
    
    TS_LOGI(TAG, "Storage subsystem initialized");
    
    return ESP_OK;
}

esp_err_t ts_storage_deinit(void)
{
    if (!s_storage.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ts_storage_unmount_auto();
    if (ret != ESP_OK) return ret;

    /* Unmount filesystems if mounted */
    if (s_storage.spiffs_mounted) {
        ts_storage_unmount_spiffs();
    }
    
    if (s_storage.sd_mounted) {
        ts_storage_unmount_sd();
    }
    
    vSemaphoreDelete(s_storage.mutex);
    s_storage.initialized = false;
    
    TS_LOGI(TAG, "Storage subsystem deinitialized");
    
    return ESP_OK;
}

/*===========================================================================*/
/*                           File Operations                                  */
/*===========================================================================*/

bool ts_storage_exists(const char *path)
{
    if (path == NULL) {
        return false;
    }
    
    struct stat st;
    return (stat(path, &st) == 0);
}

bool ts_storage_is_dir(const char *path)
{
    if (path == NULL) {
        return false;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    
    return S_ISDIR(st.st_mode);
}

esp_err_t ts_storage_stat(const char *path, ts_file_info_t *info)
{
    if (path == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Extract filename */
    const char *name = strrchr(path, '/');
    if (name) {
        name++;
    } else {
        name = path;
    }
    
    strncpy(info->name, name, TS_STORAGE_MAX_NAME - 1);
    info->name[TS_STORAGE_MAX_NAME - 1] = '\0';
    info->size = st.st_size;
    info->is_directory = S_ISDIR(st.st_mode);
    info->modified = st.st_mtime;
    
    return ESP_OK;
}

ssize_t ts_storage_size(const char *path)
{
    if (path == NULL) {
        return -1;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    
    return st.st_size;
}

ssize_t ts_storage_read_file(const char *path, void *buf, size_t max_size)
{
    if (path == NULL || buf == NULL || max_size == 0) {
        return -1;
    }
    
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    
    ssize_t bytes_read = fread(buf, 1, max_size, f);
    fclose(f);
    
    return bytes_read;
}

char *ts_storage_read_string(const char *path)
{
    ssize_t size = ts_storage_size(path);
    if (size < 0) {
        return NULL;
    }
    
    char *str = TS_STG_MALLOC(size + 1);
    if (str == NULL) {
        return NULL;
    }
    
    ssize_t read = ts_storage_read_file(path, str, size);
    if (read < 0) {
        free(str);
        return NULL;
    }
    
    str[read] = '\0';
    return str;
}

esp_err_t ts_storage_open_write(const char *path, FILE **file)
{
    if (!file) return ESP_ERR_INVALID_ARG;
    *file = NULL;
    ts_storage_sd_lock();
    esp_err_t ret = ts_storage_check_write_path(path, false);
    if (ret == ESP_OK) {
        *file = fopen(path, "wb");
        if (!*file) ret = ESP_FAIL;
    }
    ts_storage_sd_unlock();
    return ret;
}

static esp_err_t write_file_unchecked(const char *path, const void *data, size_t size)
{
    if (path == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        TS_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, size, f);
    int closed = fclose(f);
    
    if (written != size || closed != 0) {
        TS_LOGE(TAG, "Write error: %zu of %zu bytes", written, size);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_write_file(const char *path, const void *data, size_t size)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,false);
    if(ret==ESP_OK)ret=write_file_unchecked(path, data, size);
    ts_storage_sd_unlock();return ret;
}

esp_err_t ts_storage_write_string(const char *path, const char *str)
{
    if (str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ts_storage_write_file(path, str, strlen(str));
}

static esp_err_t append_unchecked(const char *path, const void *data, size_t size)
{
    if (path == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, size, f);
    int closed = fclose(f);
    
    if (written != size || closed != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_append(const char *path, const void *data, size_t size)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,false);
    if(ret==ESP_OK)ret=append_unchecked(path,data,size);
    ts_storage_sd_unlock();return ret;
}

static esp_err_t delete_unchecked(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (unlink(path) != 0) {
        TS_LOGE(TAG, "Failed to delete %s: %d", path, errno);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_delete(const char *path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,true);
    if(ret==ESP_OK)ret=delete_unchecked(path);
    ts_storage_sd_unlock();return ret;
}

static esp_err_t rename_unchecked(const char *old_path, const char *new_path)
{
    if (old_path == NULL || new_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (rename(old_path, new_path) != 0) {
        TS_LOGE(TAG, "Failed to rename %s to %s: %d", old_path, new_path, errno);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_rename(const char *old_path, const char *new_path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(old_path,true);
    if(ret==ESP_OK)ret=ts_storage_check_write_path(new_path,true);
    if(ret==ESP_OK)ret=rename_unchecked(old_path, new_path);
    ts_storage_sd_unlock();return ret;
}

static esp_err_t copy_unchecked(const char *src_path, const char *dst_path)
{
    if (src_path == NULL || dst_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *src = fopen(src_path, "rb");
    if (src == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    
    FILE *dst = fopen(dst_path, "wb");
    if (dst == NULL) {
        fclose(src);
        return ESP_FAIL;
    }
    
    char buf[512];
    size_t bytes;
    esp_err_t ret = ESP_OK;
    
    while ((bytes = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, bytes, dst) != bytes) {
            ret = ESP_FAIL;
            break;
        }
    }
    
    if (ferror(src)) ret = ESP_FAIL;
    fclose(src);
    if (fclose(dst) != 0) ret = ESP_FAIL;
    
    if (ret != ESP_OK) {
        unlink(dst_path);
    }
    
    return ret;
}

esp_err_t ts_storage_copy(const char *src_path, const char *dst_path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(dst_path,false);
    if(ret==ESP_OK)ret=copy_unchecked(src_path, dst_path);
    ts_storage_sd_unlock();return ret;
}

/*===========================================================================*/
/*                         Directory Operations                               */
/*===========================================================================*/

static esp_err_t mkdir_unchecked(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (mkdir(path, 0755) != 0) {
        if (errno == EEXIST) {
            return ESP_OK;  /* Already exists */
        }
        TS_LOGE(TAG, "Failed to create directory %s: %d", path, errno);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_mkdir(const char *path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,false);
    if(ret==ESP_OK)ret=mkdir_unchecked(path);
    ts_storage_sd_unlock();return ret;
}

static esp_err_t mkdir_p_unchecked(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char tmp[TS_STORAGE_MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    size_t len = strlen(tmp);
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Remove trailing slash */
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    
    /* Create each directory in path */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return ESP_FAIL;
            }
            *p = '/';
        }
    }
    
    /* Create final directory */
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_mkdir_p(const char *path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,false);
    if(ret==ESP_OK)ret=mkdir_p_unchecked(path);
    ts_storage_sd_unlock();return ret;
}

static esp_err_t rmdir_unchecked(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (rmdir(path) != 0) {
        TS_LOGE(TAG, "Failed to remove directory %s: %d", path, errno);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_rmdir(const char *path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,true);
    if(ret==ESP_OK)ret=rmdir_unchecked(path);
    ts_storage_sd_unlock();return ret;
}

static esp_err_t rmdir_r_unchecked(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_dir_iterator_t iter;
    esp_err_t ret = ts_storage_dir_open(&iter, path);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ts_file_info_t info;
    while (ts_storage_dir_next(&iter, &info) == ESP_OK) {
        char full_path[TS_STORAGE_MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, info.name);
        
        ret = info.is_directory ? ts_storage_rmdir_r(full_path) : ts_storage_delete(full_path);
        if (ret != ESP_OK) break;
    }
    
    ts_storage_dir_close(&iter);
    
    return ret == ESP_OK ? ts_storage_rmdir(path) : ret;
}

esp_err_t ts_storage_rmdir_r(const char *path)
{
    ts_storage_sd_lock();
    esp_err_t ret=ts_storage_check_write_path(path,true);
    if(ret==ESP_OK)ret=rmdir_r_unchecked(path);
    ts_storage_sd_unlock();return ret;
}

esp_err_t ts_storage_dir_open(ts_dir_iterator_t *iter, const char *path)
{
    if (iter == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    
    iter->handle = dir;
    strncpy(iter->base_path, path, TS_STORAGE_MAX_PATH - 1);
    iter->base_path[TS_STORAGE_MAX_PATH - 1] = '\0';
    
    return ESP_OK;
}

esp_err_t ts_storage_dir_next(ts_dir_iterator_t *iter, ts_file_info_t *info)
{
    if (iter == NULL || iter->handle == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    DIR *dir = (DIR *)iter->handle;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        strncpy(info->name, entry->d_name, TS_STORAGE_MAX_NAME - 1);
        info->name[TS_STORAGE_MAX_NAME - 1] = '\0';
        
        /* Get full info */
        char full_path[TS_STORAGE_MAX_PATH * 2];
        snprintf(full_path, sizeof(full_path), "%.127s/%.127s", iter->base_path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            info->size = st.st_size;
            info->is_directory = S_ISDIR(st.st_mode);
            info->modified = st.st_mtime;
        } else {
            info->size = 0;
            info->is_directory = (entry->d_type == DT_DIR);
            info->modified = 0;
        }
        
        return ESP_OK;
    }
    
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ts_storage_dir_close(ts_dir_iterator_t *iter)
{
    if (iter == NULL || iter->handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    closedir((DIR *)iter->handle);
    iter->handle = NULL;
    
    return ESP_OK;
}

/*===========================================================================*/
/*                              Utilities                                     */
/*===========================================================================*/

ts_storage_type_t ts_storage_get_type(const char *path)
{
    if (path == NULL) {
        return TS_STORAGE_TYPE_MAX;
    }
    
    if (s_storage.sd_mounted && 
        strncmp(path, s_storage.sd_mount_point, strlen(s_storage.sd_mount_point)) == 0) {
        return TS_STORAGE_TYPE_FATFS;
    }
    
    if (s_storage.spiffs_mounted && 
        strncmp(path, s_storage.spiffs_mount_point, strlen(s_storage.spiffs_mount_point)) == 0) {
        return TS_STORAGE_TYPE_SPIFFS;
    }
    
    return TS_STORAGE_TYPE_MAX;
}

const char *ts_storage_get_mount_point(ts_storage_type_t type)
{
    switch (type) {
        case TS_STORAGE_TYPE_SPIFFS:
            return s_storage.spiffs_mounted ? s_storage.spiffs_mount_point : NULL;
        case TS_STORAGE_TYPE_FATFS:
            return s_storage.sd_mounted ? s_storage.sd_mount_point : NULL;
        default:
            return NULL;
    }
}

esp_err_t ts_storage_build_path(char *buf, size_t buf_size, 
                                 ts_storage_type_t type, 
                                 const char *relative_path)
{
    if (buf == NULL || relative_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *mount = ts_storage_get_mount_point(type);
    if (mount == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Skip leading slash in relative path */
    if (relative_path[0] == '/') {
        relative_path++;
    }
    
    int ret = snprintf(buf, buf_size, "%s/%s", mount, relative_path);
    if (ret < 0 || (size_t)ret >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    return ESP_OK;
}

/* Getter/setter for mount state (used by spiffs/sd modules) */
void ts_storage_set_spiffs_mounted(bool mounted, const char *mount_point)
{
    s_storage.spiffs_mounted = mounted;
    if (mount_point) {
        strncpy(s_storage.spiffs_mount_point, mount_point, 
                sizeof(s_storage.spiffs_mount_point) - 1);
    }
}

void ts_storage_set_sd_mounted(bool mounted, const char *mount_point)
{
    s_storage.sd_mounted = mounted;
    if (mount_point) {
        strncpy(s_storage.sd_mount_point, mount_point, 
                sizeof(s_storage.sd_mount_point) - 1);
    }
}
