/**
 * @file ts_storage.h
 * @brief TianShanOS Storage Management
 * 
 * Unified storage interface supporting SD card (FAT32) and 
 * internal SPIFFS filesystem.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#ifndef TS_STORAGE_H
#define TS_STORAGE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_STORAGE_VERSION_MAJOR    1
#define TS_STORAGE_VERSION_MINOR    0
#define TS_STORAGE_VERSION_PATCH    0

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define TS_STORAGE_MAX_PATH         256
#define TS_STORAGE_MAX_NAME         64

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief Storage type
 */
typedef enum {
    TS_STORAGE_TYPE_SPIFFS = 0,   /**< Internal SPIFFS */
    TS_STORAGE_TYPE_FATFS,        /**< FAT filesystem (SD card) */
    TS_STORAGE_TYPE_MAX
} ts_storage_type_t;

/**
 * @brief SD card mode
 */
typedef enum {
    TS_SD_MODE_SPI = 0,           /**< SPI mode */
    TS_SD_MODE_SDIO_1BIT,         /**< SDIO 1-bit mode */
    TS_SD_MODE_SDIO_4BIT          /**< SDIO 4-bit mode */
} ts_sd_mode_t;

/**
 * @brief File info structure
 */
typedef struct {
    char name[TS_STORAGE_MAX_NAME];
    size_t size;
    bool is_directory;
    time_t modified;
} ts_file_info_t;

/**
 * @brief Storage stats
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
} ts_storage_stats_t;

/**
 * @brief SPIFFS configuration
 */
typedef struct {
    const char *mount_point;      /**< Mount point (e.g., "/spiffs") */
    const char *partition_label;  /**< Partition label */
    size_t max_files;             /**< Maximum open files */
    bool format_if_mount_failed;  /**< Format if mount fails */
} ts_spiffs_config_t;

/**
 * @brief SD card configuration
 */
typedef struct {
    const char *mount_point;      /**< Mount point (e.g., "/sdcard") */
    ts_sd_mode_t mode;            /**< SD mode */
    int max_freq_khz;             /**< Maximum frequency (kHz) */
    bool format_if_mount_failed;  /**< Format if mount fails */
    
    /* GPIO pins (for SPI mode) */
    int pin_mosi;
    int pin_miso;
    int pin_clk;
    int pin_cs;
    
    /* GPIO pins (for SDIO mode) */
    int pin_cmd;
    int pin_d0;
    int pin_d1;
    int pin_d2;
    int pin_d3;
} ts_sd_config_t;

/**
 * @brief Directory iterator
 */
typedef struct {
    void *handle;
    char base_path[TS_STORAGE_MAX_PATH];
} ts_dir_iterator_t;

/*===========================================================================*/
/*                         Default Configuration                              */
/*===========================================================================*/

#define TS_SPIFFS_DEFAULT_CONFIG() { \
    .mount_point = "/spiffs", \
    .partition_label = "storage", \
    .max_files = 5, \
    .format_if_mount_failed = true \
}

#define TS_SD_DEFAULT_CONFIG() { \
    .mount_point = "/sdcard", \
    .mode = TS_SD_MODE_SDIO_4BIT, \
    .max_freq_khz = 20000, \
    .format_if_mount_failed = false, \
    .pin_mosi = -1, \
    .pin_miso = -1, \
    .pin_clk = -1, \
    .pin_cs = -1, \
    .pin_cmd = -1, \
    .pin_d0 = -1, \
    .pin_d1 = -1, \
    .pin_d2 = -1, \
    .pin_d3 = -1 \
}

/*===========================================================================*/
/*                              Core API                                      */
/*===========================================================================*/

/**
 * @brief Initialize storage subsystem
 * 
 * @return ESP_OK on success
 */
/* Dedicated automation copy region. Independent of storage/www mounts. */
esp_err_t ts_storage_mount_auto(void);
esp_err_t ts_storage_unmount_auto(void);
bool ts_storage_auto_mounted(void);
esp_err_t ts_storage_auto_info(size_t *total, size_t *used);

esp_err_t ts_storage_init(void);

/**
 * @brief Deinitialize storage subsystem
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_storage_deinit(void);

/*===========================================================================*/
/*                           SPIFFS Operations                                */
/*===========================================================================*/

/**
 * @brief Mount SPIFFS filesystem
 * 
 * @param config SPIFFS configuration, or NULL for defaults
 * @return ESP_OK on success
 */
esp_err_t ts_storage_mount_spiffs(const ts_spiffs_config_t *config);

/**
 * @brief Unmount SPIFFS filesystem
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_storage_unmount_spiffs(void);

/**
 * @brief Check if SPIFFS is mounted
 * 
 * @return true if mounted
 */
bool ts_storage_spiffs_mounted(void);

/**
 * @brief Get SPIFFS storage stats
 * 
 * @param stats Output stats structure
 * @return ESP_OK on success
 */
esp_err_t ts_storage_spiffs_stats(ts_storage_stats_t *stats);

/**
 * @brief Format SPIFFS filesystem
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_storage_format_spiffs(void);

/*===========================================================================*/
/*                           SD Card Operations                               */
/*===========================================================================*/

/* Mount, unmount, format and automation commits share this recursive gate.
 * Acquire the automation writer before this gate when both are needed. */
void ts_storage_sd_lock(void);
void ts_storage_sd_unlock(void);
uint64_t ts_storage_sd_generation(void);
/* Explicit one-shot driver check / mount. No background card polling. */
esp_err_t ts_storage_sd_refresh(void);

/**
 * @brief Mount SD card
 * 
 * @param config SD card configuration, or NULL for defaults
 * @return ESP_OK on success
 */
esp_err_t ts_storage_mount_sd(const ts_sd_config_t *config);

/**
 * @brief Unmount SD card
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_storage_unmount_sd(void);

/**
 * @brief Check if SD card is mounted
 * 
 * @return true if mounted
 */
bool ts_storage_sd_mounted(void);

/**
 * @brief Get SD card storage stats
 * 
 * @param stats Output stats structure
 * @return ESP_OK on success
 */
esp_err_t ts_storage_sd_stats(ts_storage_stats_t *stats);

/**
 * @brief Format SD card
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_storage_format_sd(void);

/**
 * @brief Get SD card info
 * 
 * @param capacity Output capacity in bytes
 * @param sector_size Output sector size in bytes
 * @return ESP_OK on success
 */
esp_err_t ts_storage_sd_info(uint64_t *capacity, size_t *sector_size);

/*===========================================================================*/
/*                           File Operations                                  */
/*===========================================================================*/

/**
 * @brief Check if file exists
 * 
 * @param path File path
 * @return true if exists
 */
bool ts_storage_exists(const char *path);

/**
 * @brief Check if path is directory
 * 
 * @param path Path
 * @return true if directory
 */
bool ts_storage_is_dir(const char *path);

/**
 * @brief Get file info
 * 
 * @param path File path
 * @param info Output info structure
 * @return ESP_OK on success
 */
esp_err_t ts_storage_stat(const char *path, ts_file_info_t *info);

/**
 * @brief Get file size
 * 
 * @param path File path
 * @return File size, or -1 on error
 */
ssize_t ts_storage_size(const char *path);

/**
 * @brief Read entire file into buffer
 * 
 * @param path File path
 * @param buf Output buffer
 * @param max_size Maximum size to read
 * @return Number of bytes read, or -1 on error
 */
ssize_t ts_storage_read_file(const char *path, void *buf, size_t max_size);

/**
 * @brief Read file as string
 * 
 * @param path File path
 * @return Allocated string (must be freed), or NULL on error
 */
char *ts_storage_read_string(const char *path);

/* Generic file mutations cannot replace the automation owner's files or roots.
 * include_children also checks directory moves/removals of their parents. */
esp_err_t ts_storage_check_write_path(const char *path, bool include_children);

/* Open a streamed download destination under the same ownership check.
 * The caller writes/closes the stream; network transfer does not hold the SD gate. */
esp_err_t ts_storage_open_write(const char *path, FILE **file);

/** Write a complete buffer to a file. */
esp_err_t ts_storage_write_file(const char *path, const void *data, size_t size);

/**
 * @brief Write string to file
 * 
 * @param path File path
 * @param str String to write
 * @return ESP_OK on success
 */
esp_err_t ts_storage_write_string(const char *path, const char *str);

/**
 * @brief Append data to file
 * 
 * @param path File path
 * @param data Data to append
 * @param size Size of data
 * @return ESP_OK on success
 */
esp_err_t ts_storage_append(const char *path, const void *data, size_t size);

/**
 * @brief Delete file
 * 
 * @param path File path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_delete(const char *path);

/**
 * @brief Rename/move file
 * 
 * @param old_path Current path
 * @param new_path New path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_rename(const char *old_path, const char *new_path);

/**
 * @brief Copy file
 * 
 * @param src_path Source path
 * @param dst_path Destination path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_copy(const char *src_path, const char *dst_path);

/*===========================================================================*/
/*                         Directory Operations                               */
/*===========================================================================*/

/**
 * @brief Create directory
 * 
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_mkdir(const char *path);

/**
 * @brief Create directory and all parents
 * 
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_mkdir_p(const char *path);

/**
 * @brief Remove directory
 * 
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_rmdir(const char *path);

/**
 * @brief Remove directory recursively
 * 
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_rmdir_r(const char *path);

/**
 * @brief Start directory iteration
 * 
 * @param iter Iterator structure
 * @param path Directory path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_dir_open(ts_dir_iterator_t *iter, const char *path);

/**
 * @brief Get next directory entry
 * 
 * @param iter Iterator structure
 * @param info Output file info
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND at end
 */
esp_err_t ts_storage_dir_next(ts_dir_iterator_t *iter, ts_file_info_t *info);

/**
 * @brief Close directory iterator
 * 
 * @param iter Iterator structure
 * @return ESP_OK on success
 */
esp_err_t ts_storage_dir_close(ts_dir_iterator_t *iter);

/*===========================================================================*/
/*                              Utilities                                     */
/*===========================================================================*/

/**
 * @brief Get storage type for path
 * 
 * @param path File path
 * @return Storage type
 */
ts_storage_type_t ts_storage_get_type(const char *path);

/**
 * @brief Get mount point for storage type
 * 
 * @param type Storage type
 * @return Mount point string
 */
const char *ts_storage_get_mount_point(ts_storage_type_t type);

/**
 * @brief Build full path from mount point and relative path
 * 
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @param type Storage type
 * @param relative_path Relative path
 * @return ESP_OK on success
 */
esp_err_t ts_storage_build_path(char *buf, size_t buf_size, 
                                 ts_storage_type_t type, 
                                 const char *relative_path);

#ifdef __cplusplus
}
#endif

#endif /* TS_STORAGE_H */
