#include "ts_log_preview.h"
#include <stdatomic.h>
/**
 * @file ts_log.c
 * @brief TianShanOS Logging System Implementation
 *
 * 日志系统实现
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ts_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* PSRAM 优先分配宏 */
#define TS_LOG_CALLOC(n, size) ({ void *p = heap_caps_calloc((n), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : calloc((n), (size)); })

static const char *TAG = "ts_log";

/* ============================================================================
 * ANSI 颜色代码
 * ========================================================================== */

#define LOG_COLOR_BLACK   "30"
#define LOG_COLOR_RED     "31"
#define LOG_COLOR_GREEN   "32"
#define LOG_COLOR_BROWN   "33"
#define LOG_COLOR_BLUE    "34"
#define LOG_COLOR_PURPLE  "35"
#define LOG_COLOR_CYAN    "36"
#define LOG_COLOR_WHITE   "37"

#define LOG_COLOR(COLOR)  "\033[0;" COLOR "m"
#define LOG_BOLD(COLOR)   "\033[1;" COLOR "m"

/* 取消 esp_log_color.h 中的空定义，使用我们的彩色定义 */
#undef LOG_RESET_COLOR
#undef LOG_COLOR_E
#undef LOG_COLOR_W
#undef LOG_COLOR_I
#undef LOG_COLOR_D
#undef LOG_COLOR_V

#define LOG_RESET_COLOR   "\033[0m"
#define LOG_COLOR_E       LOG_BOLD(LOG_COLOR_RED)
#define LOG_COLOR_W       LOG_BOLD(LOG_COLOR_BROWN)
#define LOG_COLOR_I       LOG_COLOR(LOG_COLOR_GREEN)
#define LOG_COLOR_D       LOG_COLOR(LOG_COLOR_CYAN)
#define LOG_COLOR_V       LOG_COLOR(LOG_COLOR_WHITE)

/* ============================================================================
 * 私有类型定义
 * ========================================================================== */

/**
 * @brief 标签级别配置
 */
typedef struct ts_log_tag_level {
    char tag[TS_LOG_TAG_MAX_LEN];
    ts_log_level_t level;
    struct ts_log_tag_level *next;
} ts_log_tag_level_t;

/**
 * @brief 回调节点
 */
typedef struct ts_log_callback_node {
    unsigned readers;
    ts_log_callback_t callback;
    ts_log_level_t min_level;
    void *user_data;
    struct ts_log_callback_node *next;
} ts_log_callback_node_t;

/**
 * @brief 日志环形缓冲区
 */
typedef struct {
    ts_log_entry_t *entries;
    size_t capacity;
    size_t head;
    size_t count;
} ts_log_ring_buffer_t;

/**
 * @brief 日志上下文
 */
typedef struct {
    _Atomic bool initialized;
    SemaphoreHandle_t mutex;
    ts_log_level_t global_level;
    uint32_t output_mask;
    bool colors_enabled;
    bool timestamp_enabled;
    bool task_name_enabled;
    ts_log_tag_level_t *tag_levels;
    ts_log_callback_node_t *callbacks;
    ts_log_ring_buffer_t buffer;
    char file_path[128];
    FILE *log_file;
    size_t file_size;
    int file_index;
    _Atomic(vprintf_like_t) original_vprintf; /**< 原始 vprintf 函数指针 */
    _Atomic bool esp_log_capture_enabled;     /**< 是否启用 ESP_LOG 捕获 */
    uint32_t total_logs_captured;        /**< 总捕获日志数（含溢出） */
    _Atomic uint32_t capture_dropped;
    _Atomic uint32_t capture_truncated;
    unsigned active_writers;
    uint32_t logs_dropped;               /**< 因缓冲区满丢弃的日志数 */
} ts_log_context_t;

/* ============================================================================
 * 私有变量
 * ========================================================================== */

static ts_log_context_t s_log_ctx = {0};
static SemaphoreHandle_t s_log_io_mutex;
static portMUX_TYPE s_capture_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool busy;
    char text[512];
    ts_log_entry_t entry;
} s_capture[2];
_Static_assert(sizeof(s_capture) <= 2048, "log capture RAM budget");

/* ============================================================================
 * 私有函数声明
 * ========================================================================== */

static void log_output_console(const ts_log_entry_t *entry, const char *format, va_list args);
static void log_output_file(const ts_log_entry_t *entry);
static void log_output_buffer(const ts_log_entry_t *entry);
static void notify_callbacks(const ts_log_entry_t *entry);
static ts_log_level_t get_effective_level(const char *tag);
static void rotate_log_file(void);
static int ts_log_vprintf_hook(const char *fmt, va_list args);
static void parse_esp_log(const char *log_line, ts_log_entry_t *out, bool clipped);

/* ============================================================================
 * 初始化和反初始化
 * ========================================================================== */

static atomic_bool s_log_lifecycle_busy;

esp_err_t ts_log_init(void)
{
    if (atomic_exchange(&s_log_lifecycle_busy, true))
        return ESP_ERR_INVALID_STATE;
    if (s_log_ctx.initialized) {
        atomic_store(&s_log_lifecycle_busy, false);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing TianShanOS Logging System...");

    // 创建互斥锁
    if (!s_log_ctx.mutex)
        s_log_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_log_io_mutex)
        s_log_io_mutex = xSemaphoreCreateMutex();
    if (s_log_ctx.mutex == NULL || !s_log_io_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        atomic_store(&s_log_lifecycle_busy, false);
        return ESP_ERR_NO_MEM;
    }

    // 初始化默认配置
    s_log_ctx.global_level = (ts_log_level_t)CONFIG_TS_LOG_DEFAULT_LEVEL;
    s_log_ctx.output_mask = 0;

#ifdef CONFIG_TS_LOG_OUTPUT_CONSOLE
    s_log_ctx.output_mask |= TS_LOG_OUTPUT_CONSOLE;
#endif

#ifdef CONFIG_TS_LOG_OUTPUT_FILE
    s_log_ctx.output_mask |= TS_LOG_OUTPUT_FILE;
#endif

#ifdef CONFIG_TS_LOG_OUTPUT_BUFFER
    s_log_ctx.output_mask |= TS_LOG_OUTPUT_BUFFER;
#endif

#ifdef CONFIG_TS_LOG_COLORS
    s_log_ctx.colors_enabled = true;
#else
    s_log_ctx.colors_enabled = false;
#endif

#ifdef CONFIG_TS_LOG_TIMESTAMP
    s_log_ctx.timestamp_enabled = true;
#else
    s_log_ctx.timestamp_enabled = false;
#endif

#ifdef CONFIG_TS_LOG_TASK_NAME
    s_log_ctx.task_name_enabled = true;
#else
    s_log_ctx.task_name_enabled = false;
#endif

    // 初始化日志缓冲区（优先使用 PSRAM）
    if (s_log_ctx.output_mask & TS_LOG_OUTPUT_BUFFER) {
        s_log_ctx.buffer.capacity = TS_LOG_BUFFER_SIZE;
        s_log_ctx.buffer.entries = heap_caps_calloc(TS_LOG_BUFFER_SIZE, sizeof(ts_log_entry_t), 
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_log_ctx.buffer.entries == NULL) {
            // Fallback 到 DRAM
            ESP_LOGW(TAG, "PSRAM not available, using DRAM for log buffer");
            s_log_ctx.buffer.entries = calloc(TS_LOG_BUFFER_SIZE, sizeof(ts_log_entry_t));
            if (s_log_ctx.buffer.entries == NULL) {
                ESP_LOGW(TAG, "Failed to allocate log buffer");
                s_log_ctx.output_mask &= ~TS_LOG_OUTPUT_BUFFER;
            }
        } else {
            ESP_LOGI(TAG, "Log buffer allocated in PSRAM (%zu entries, %zu bytes)",
                     TS_LOG_BUFFER_SIZE, TS_LOG_BUFFER_SIZE * sizeof(ts_log_entry_t));
        }
        s_log_ctx.buffer.head = 0;
        s_log_ctx.buffer.count = 0;
    }

    // 初始化文件路径
#ifdef CONFIG_TS_LOG_FILE_PATH
    strncpy(s_log_ctx.file_path, CONFIG_TS_LOG_FILE_PATH, sizeof(s_log_ctx.file_path) - 1);
#else
    strcpy(s_log_ctx.file_path, "/sdcard/logs");
#endif
    s_log_ctx.log_file = NULL;
    s_log_ctx.file_size = 0;
    s_log_ctx.file_index = 0;

    s_log_ctx.tag_levels = NULL;
    s_log_ctx.callbacks = NULL;
    s_log_ctx.total_logs_captured = 0;
    s_log_ctx.logs_dropped = 0;

    portENTER_CRITICAL(&s_capture_lock);
    s_log_ctx.initialized = true;
    portEXIT_CRITICAL(&s_capture_lock);

    // 安装 ESP_LOG vprintf 钩子（捕获所有 ESP-IDF 日志）
#ifdef CONFIG_TS_LOG_CAPTURE_ESP_LOG
    ts_log_enable_esp_capture(true);
    ESP_LOGI(TAG, "ESP_LOG capture hook installed");
#else
    s_log_ctx.esp_log_capture_enabled = false;
    s_log_ctx.original_vprintf = NULL;
#endif

    ESP_LOGI(TAG, "Logging system initialized (level=%d, outputs=0x%02lx, buffer=%zu)",
             s_log_ctx.global_level, (unsigned long)s_log_ctx.output_mask,
             s_log_ctx.buffer.capacity);

    atomic_store(&s_log_lifecycle_busy, false);

    return ESP_OK;
}

esp_err_t ts_log_deinit(void)
{
    if (atomic_exchange(&s_log_lifecycle_busy, true))
        return ESP_ERR_INVALID_STATE;
    if (!s_log_ctx.initialized) {
        atomic_store(&s_log_lifecycle_busy, false);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_log_io_mutex, portMAX_DELAY);
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    if (s_log_ctx.active_writers) {
        xSemaphoreGive(s_log_ctx.mutex);
        xSemaphoreGive(s_log_io_mutex);
        atomic_store(&s_log_lifecycle_busy, false);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_capture_lock);
    s_log_ctx.initialized = false;
    if (s_log_ctx.esp_log_capture_enabled && s_log_ctx.original_vprintf)
        esp_log_set_vprintf(s_log_ctx.original_vprintf);
    s_log_ctx.esp_log_capture_enabled = false;
    portEXIT_CRITICAL(&s_capture_lock);
    ts_log_entry_t *entries = s_log_ctx.buffer.entries;
    ts_log_tag_level_t *tags = s_log_ctx.tag_levels;
    ts_log_callback_node_t *callbacks = s_log_ctx.callbacks;
    s_log_ctx.buffer.entries = NULL;
    s_log_ctx.buffer.count = 0;
    s_log_ctx.tag_levels = NULL;
    s_log_ctx.callbacks = NULL;
    FILE *file = s_log_ctx.log_file;
    s_log_ctx.log_file = NULL;
    xSemaphoreGive(s_log_ctx.mutex);
    if (file)
        fclose(file);
    xSemaphoreGive(s_log_io_mutex);
    free(entries);
    while (tags) {
        ts_log_tag_level_t *next = tags->next;
        free(tags);
        tags = next;
    }
    while (callbacks) {
        ts_log_callback_node_t *next = callbacks->next;
        free(callbacks);
        callbacks = next;
    }
    /* Mutex lifetime exceeds all hooks/readers that observed initialized=true. */
    atomic_store(&s_log_lifecycle_busy, false);
    return ESP_OK;
}

bool ts_log_is_initialized(void)
{
    return s_log_ctx.initialized;
}

/* ============================================================================
 * 日志输出 API
 * ========================================================================== */

void ts_log(ts_log_level_t level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ts_log_v(level, tag, format, args);
    va_end(args);
}

void ts_log_v(ts_log_level_t level, const char *tag, const char *format, va_list args)
{
    if (!s_log_ctx.initialized) {
        // 未初始化时回退到 ESP-IDF 日志
        esp_log_level_t esp_level;
        switch (level) {
            case TS_LOG_ERROR:   esp_level = ESP_LOG_ERROR; break;
            case TS_LOG_WARN:    esp_level = ESP_LOG_WARN; break;
            case TS_LOG_INFO:    esp_level = ESP_LOG_INFO; break;
            case TS_LOG_DEBUG:   esp_level = ESP_LOG_DEBUG; break;
            case TS_LOG_VERBOSE: esp_level = ESP_LOG_VERBOSE; break;
            default: return;
        }
        esp_log_writev(esp_level, tag, format, args);
        return;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    if (!s_log_ctx.initialized) {
        xSemaphoreGive(s_log_ctx.mutex);
        return;
    }
    ++s_log_ctx.active_writers;
    xSemaphoreGive(s_log_ctx.mutex);
    // 检查级别过滤
    ts_log_level_t effective_level = get_effective_level(tag);
    if (level > effective_level) {
        xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
        --s_log_ctx.active_writers;
        xSemaphoreGive(s_log_ctx.mutex);
        return;
    }

    // 构建日志条目
    ts_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    entry.level = level;

    if (tag != NULL) {
        strncpy(entry.tag, tag, TS_LOG_TAG_MAX_LEN - 1);
    }

    if (s_log_ctx.task_name_enabled) {
        TaskHandle_t task = xTaskGetCurrentTaskHandle();
        if (task != NULL) {
            strncpy(entry.task_name, pcTaskGetName(task), sizeof(entry.task_name) - 1);
        }
    }

    va_list preview;
    va_copy(preview, args);
    int needed = vsnprintf(entry.message, TS_LOG_MSG_MAX_LEN, format, preview);
    va_end(preview);
    if (ts_log_preview(entry.message, sizeof(entry.message), entry.message,
                       needed >= TS_LOG_MSG_MAX_LEN))
        atomic_fetch_add(&s_log_ctx.capture_truncated, 1);

    // 输出到各目标
    xSemaphoreTake(s_log_io_mutex, portMAX_DELAY);

    if (s_log_ctx.output_mask & TS_LOG_OUTPUT_CONSOLE) {
        log_output_console(&entry, format, args);
    }

    if (s_log_ctx.output_mask & TS_LOG_OUTPUT_FILE) {
        log_output_file(&entry);
    }

    xSemaphoreGive(s_log_io_mutex);
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    if (s_log_ctx.output_mask & TS_LOG_OUTPUT_BUFFER) {
        log_output_buffer(&entry);
    }

    xSemaphoreGive(s_log_ctx.mutex);

    // 通知回调（在锁外调用）
    notify_callbacks(&entry);
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    --s_log_ctx.active_writers;
    xSemaphoreGive(s_log_ctx.mutex);
}

void ts_log_hex(ts_log_level_t level, const char *tag, const void *data, size_t length)
{
    if (!s_log_ctx.initialized || data == NULL || length == 0) {
        return;
    }

    // 检查级别过滤
    ts_log_level_t effective_level = get_effective_level(tag);
    if (level > effective_level) {
        return;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    char line[80];
    size_t offset = 0;

    while (offset < length) {
        int pos = snprintf(line, sizeof(line), "%04zx: ", offset);

        // 十六进制部分
        for (size_t i = 0; i < 16 && offset + i < length; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", bytes[offset + i]);
        }

        // 填充
        for (size_t i = length - offset; i < 16 && length > offset; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "   ");
        }

        // ASCII 部分
        pos += snprintf(line + pos, sizeof(line) - pos, " |");
        for (size_t i = 0; i < 16 && offset + i < length; i++) {
            char c = bytes[offset + i];
            if (c >= 32 && c < 127) {
                pos += snprintf(line + pos, sizeof(line) - pos, "%c", c);
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, ".");
            }
        }
        snprintf(line + pos, sizeof(line) - pos, "|");

        ts_log(level, tag, "%s", line);
        offset += 16;
    }
}

/* ============================================================================
 * 日志级别控制
 * ========================================================================== */

void ts_log_set_level(ts_log_level_t level)
{
    s_log_ctx.global_level = level;
    ESP_LOGI(TAG, "Global log level set to %s", ts_log_level_to_string(level));
}

ts_log_level_t ts_log_get_level(void)
{
    return s_log_ctx.global_level;
}

esp_err_t ts_log_set_tag_level(const char *tag, ts_log_level_t level)
{
    if (tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);

    // 查找是否已存在
    ts_log_tag_level_t *node = s_log_ctx.tag_levels;
    while (node != NULL) {
        if (strcmp(node->tag, tag) == 0) {
            node->level = level;
            xSemaphoreGive(s_log_ctx.mutex);
            return ESP_OK;
        }
        node = node->next;
    }

    // 创建新节点（优先使用 PSRAM）
    node = TS_LOG_CALLOC(1, sizeof(ts_log_tag_level_t));
    if (node == NULL) {
        xSemaphoreGive(s_log_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }

    strncpy(node->tag, tag, TS_LOG_TAG_MAX_LEN - 1);
    node->level = level;
    node->next = s_log_ctx.tag_levels;
    s_log_ctx.tag_levels = node;

    xSemaphoreGive(s_log_ctx.mutex);
    return ESP_OK;
}

ts_log_level_t ts_log_get_tag_level(const char *tag)
{
    if (tag == NULL) {
        return s_log_ctx.global_level;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);

    ts_log_tag_level_t *node = s_log_ctx.tag_levels;
    while (node != NULL) {
        if (strcmp(node->tag, tag) == 0) {
            ts_log_level_t level = node->level;
            xSemaphoreGive(s_log_ctx.mutex);
            return level;
        }
        node = node->next;
    }

    xSemaphoreGive(s_log_ctx.mutex);
    return s_log_ctx.global_level;
}

void ts_log_reset_tag_levels(void)
{
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);

    ts_log_tag_level_t *node = s_log_ctx.tag_levels;
    while (node != NULL) {
        ts_log_tag_level_t *next = node->next;
        free(node);
        node = next;
    }
    s_log_ctx.tag_levels = NULL;

    xSemaphoreGive(s_log_ctx.mutex);
}

/* ============================================================================
 * 输出控制
 * ========================================================================== */

void ts_log_enable_output(ts_log_output_t output)
{
    s_log_ctx.output_mask |= output;
}

void ts_log_disable_output(ts_log_output_t output)
{
    s_log_ctx.output_mask &= ~output;
}

uint32_t ts_log_get_outputs(void)
{
    return s_log_ctx.output_mask;
}

esp_err_t ts_log_set_file_path(const char *path)
{
    if (path == NULL || strlen(path) >= sizeof(s_log_ctx.file_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_log_io_mutex, portMAX_DELAY);

    // 关闭当前文件
    if (s_log_ctx.log_file != NULL) {
        fclose(s_log_ctx.log_file);
        s_log_ctx.log_file = NULL;
    }

    strncpy(s_log_ctx.file_path, path, sizeof(s_log_ctx.file_path) - 1);
    s_log_ctx.file_index = 0;
    s_log_ctx.file_size = 0;

    xSemaphoreGive(s_log_io_mutex);
    return ESP_OK;
}

esp_err_t ts_log_flush(void)
{
    xSemaphoreTake(s_log_io_mutex, portMAX_DELAY);

    if (s_log_ctx.log_file != NULL) {
        fflush(s_log_ctx.log_file);
    }

    xSemaphoreGive(s_log_io_mutex);
    return ESP_OK;
}

/* ============================================================================
 * 日志缓冲区操作
 * ========================================================================== */

size_t ts_log_buffer_count(void)
{
    return s_log_ctx.buffer.count;
}

size_t ts_log_buffer_get(ts_log_entry_t *entries, size_t max_count, size_t start_index)
{
    if (entries == NULL || max_count == 0 || s_log_ctx.buffer.entries == NULL) {
        return 0;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    if (!s_log_ctx.initialized || !s_log_ctx.buffer.entries) {
        xSemaphoreGive(s_log_ctx.mutex);
        return 0;
    }

    size_t count = s_log_ctx.buffer.count;
    if (start_index >= count) {
        xSemaphoreGive(s_log_ctx.mutex);
        return 0;
    }

    size_t available = count - start_index;
    size_t to_copy = (available < max_count) ? available : max_count;

    // 计算起始位置
    size_t start = (s_log_ctx.buffer.head + s_log_ctx.buffer.capacity - count + start_index) 
                   % s_log_ctx.buffer.capacity;

    for (size_t i = 0; i < to_copy; i++) {
        size_t idx = (start + i) % s_log_ctx.buffer.capacity;
        memcpy(&entries[i], &s_log_ctx.buffer.entries[idx], sizeof(ts_log_entry_t));
    }

    xSemaphoreGive(s_log_ctx.mutex);
    return to_copy;
}

void ts_log_buffer_clear(void)
{
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    s_log_ctx.buffer.head = 0;
    s_log_ctx.buffer.count = 0;
    xSemaphoreGive(s_log_ctx.mutex);
}

/* ============================================================================
 * 日志回调
 * ========================================================================== */

esp_err_t ts_log_add_callback(ts_log_callback_t callback,
                               ts_log_level_t min_level,
                               void *user_data,
                               ts_log_callback_handle_t *handle)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_log_callback_node_t *node = TS_LOG_CALLOC(1, sizeof(ts_log_callback_node_t));
    if (node == NULL) {
        return ESP_ERR_NO_MEM;
    }

    node->callback = callback;
    node->min_level = min_level;
    node->user_data = user_data;

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    unsigned count = 0;
    for (ts_log_callback_node_t *n = s_log_ctx.callbacks; n; n = n->next)
        ++count;
    if (!s_log_ctx.initialized || count == 8) {
        xSemaphoreGive(s_log_ctx.mutex);
        free(node);
        return ESP_ERR_NO_MEM;
    }
    node->next = s_log_ctx.callbacks;
    s_log_ctx.callbacks = node;
    xSemaphoreGive(s_log_ctx.mutex);

    if (handle != NULL) {
        *handle = (ts_log_callback_handle_t)node;
    }

    return ESP_OK;
}

esp_err_t ts_log_remove_callback(ts_log_callback_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);

    ts_log_callback_node_t *prev = NULL;
    ts_log_callback_node_t *node = s_log_ctx.callbacks;
    ts_log_callback_node_t *target = (ts_log_callback_node_t *)handle;

    while (node != NULL) {
        if (node == target) {
            if (node->readers) {
                xSemaphoreGive(s_log_ctx.mutex);
                return ESP_ERR_INVALID_STATE;
            }
            if (prev == NULL) {
                s_log_ctx.callbacks = node->next;
            } else {
                prev->next = node->next;
            }
            xSemaphoreGive(s_log_ctx.mutex);
            free(node);
            return ESP_OK;
        }
        prev = node;
        node = node->next;
    }

    xSemaphoreGive(s_log_ctx.mutex);
    return ESP_ERR_NOT_FOUND;
}

/* ============================================================================
 * 工具函数
 * ========================================================================== */

const char *ts_log_level_to_string(ts_log_level_t level)
{
    static const char *level_names[] = {
        "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"
    };

    if (level >= TS_LOG_MAX) {
        return "UNKNOWN";
    }
    return level_names[level];
}

ts_log_level_t ts_log_level_from_string(const char *str)
{
    if (str == NULL) {
        return TS_LOG_INFO;
    }

    if (strcasecmp(str, "none") == 0 || strcmp(str, "0") == 0) {
        return TS_LOG_NONE;
    }
    if (strcasecmp(str, "error") == 0 || strcasecmp(str, "e") == 0 || strcmp(str, "1") == 0) {
        return TS_LOG_ERROR;
    }
    if (strcasecmp(str, "warn") == 0 || strcasecmp(str, "warning") == 0 || 
        strcasecmp(str, "w") == 0 || strcmp(str, "2") == 0) {
        return TS_LOG_WARN;
    }
    if (strcasecmp(str, "info") == 0 || strcasecmp(str, "i") == 0 || strcmp(str, "3") == 0) {
        return TS_LOG_INFO;
    }
    if (strcasecmp(str, "debug") == 0 || strcasecmp(str, "d") == 0 || strcmp(str, "4") == 0) {
        return TS_LOG_DEBUG;
    }
    if (strcasecmp(str, "verbose") == 0 || strcasecmp(str, "v") == 0 || strcmp(str, "5") == 0) {
        return TS_LOG_VERBOSE;
    }

    return TS_LOG_INFO;  // 默认
}

const char *ts_log_level_color(ts_log_level_t level)
{
    switch (level) {
        case TS_LOG_ERROR:   return LOG_COLOR_E;
        case TS_LOG_WARN:    return LOG_COLOR_W;
        case TS_LOG_INFO:    return LOG_COLOR_I;
        case TS_LOG_DEBUG:   return LOG_COLOR_D;
        case TS_LOG_VERBOSE: return LOG_COLOR_V;
        default:             return "";
    }
}

/* ============================================================================
 * 私有函数实现
 * ========================================================================== */

static void log_output_console(const ts_log_entry_t *entry, const char *format, va_list args) {
    static const char levels[] = "NEWIDV";
    char level = entry->level >= 0 && entry->level < TS_LOG_MAX ? levels[entry->level] : '?';
    const char *color = s_log_ctx.colors_enabled ? ts_log_level_color(entry->level) : "";
    if (s_log_ctx.timestamp_enabled)
        printf("%s%c (%lu) %s: ", color, level, (unsigned long)entry->timestamp_ms, entry->tag);
    else
        printf("%s%c %s: ", color, level, entry->tag);
    va_list output;
    va_copy(output, args);
    vprintf(format, output);
    va_end(output);
    printf("%s\n", s_log_ctx.colors_enabled ? LOG_RESET_COLOR : "");
}

static void log_output_file(const ts_log_entry_t *entry)
{
    // 打开文件（如果需要）
    if (s_log_ctx.log_file == NULL) {
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/tianshan_%d.log",
                 s_log_ctx.file_path, s_log_ctx.file_index);
        
        s_log_ctx.log_file = fopen(filepath, "a");
        if (s_log_ctx.log_file == NULL) {
            // 无法打开文件，禁用文件输出
            s_log_ctx.output_mask &= ~TS_LOG_OUTPUT_FILE;
            return;
        }
    }

    // 写入日志
    static const char level_chars[] = { 'N', 'E', 'W', 'I', 'D', 'V' };
    char level_char = (entry->level < TS_LOG_MAX) ? level_chars[entry->level] : '?';

    int written = fprintf(s_log_ctx.log_file, "%c %lu %s: %s\n",
                          level_char,
                          (unsigned long)entry->timestamp_ms,
                          entry->tag,
                          entry->message);

    if (written > 0) {
        s_log_ctx.file_size += written;
    }

    // 检查是否需要轮转
#ifdef CONFIG_TS_LOG_FILE_MAX_SIZE
    if (s_log_ctx.file_size >= CONFIG_TS_LOG_FILE_MAX_SIZE * 1024) {
        rotate_log_file();
    }
#endif
}

static void log_output_buffer(const ts_log_entry_t *entry)
{
    if (s_log_ctx.buffer.entries == NULL) {
        return;
    }

    // 复制到环形缓冲区
    memcpy(&s_log_ctx.buffer.entries[s_log_ctx.buffer.head], entry, sizeof(ts_log_entry_t));
    s_log_ctx.buffer.head = (s_log_ctx.buffer.head + 1) % s_log_ctx.buffer.capacity;

    if (s_log_ctx.buffer.count < s_log_ctx.buffer.capacity) {
        s_log_ctx.buffer.count++;
    } else {
        ++s_log_ctx.logs_dropped;
    }
}

static TaskHandle_t callback_tasks[8];
static void notify_callbacks(const ts_log_entry_t *entry)
{
    /* Bounded snapshot; removal returns BUSY while its user_data is borrowed. */
    ts_log_callback_node_t *snapshot[8];
    unsigned count = 0;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    int slot = -1;
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    for (unsigned i = 0; i < 8; i++) {
        if (callback_tasks[i] == task) {
            xSemaphoreGive(s_log_ctx.mutex);
            return;
        }
        if (!callback_tasks[i] && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        xSemaphoreGive(s_log_ctx.mutex);
        return;
    }
    callback_tasks[slot] = task;
    for (ts_log_callback_node_t *n = s_log_ctx.callbacks; n && count < 8; n = n->next) {
        if (entry->level <= n->min_level && n->callback) {
            ++n->readers;
            snapshot[count++] = n;
        }
    }
    xSemaphoreGive(s_log_ctx.mutex);
    for (unsigned i = 0; i < count; ++i) {
        snapshot[i]->callback(entry, snapshot[i]->user_data);
        xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
        --snapshot[i]->readers;
        xSemaphoreGive(s_log_ctx.mutex);
    }
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    callback_tasks[slot] = NULL;
    xSemaphoreGive(s_log_ctx.mutex);
}

static ts_log_level_t get_effective_level(const char *tag)
{
    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    ts_log_level_t level = s_log_ctx.global_level;
    for (ts_log_tag_level_t *n = s_log_ctx.tag_levels; tag && n; n = n->next) {
        if (!strcmp(n->tag, tag)) {
            level = n->level;
            break;
        }
    }
    xSemaphoreGive(s_log_ctx.mutex);
    return level;
}

/**
 * @brief Rotate log file when size limit reached
 * @note Reserved for log file rotation feature
 */
__attribute__((unused))
static void rotate_log_file(void)
{
    if (s_log_ctx.log_file != NULL) {
        fclose(s_log_ctx.log_file);
        s_log_ctx.log_file = NULL;
    }

    s_log_ctx.file_index++;

#ifdef CONFIG_TS_LOG_FILE_MAX_FILES
    if (s_log_ctx.file_index >= CONFIG_TS_LOG_FILE_MAX_FILES) {
        s_log_ctx.file_index = 0;
    }
#endif

    s_log_ctx.file_size = 0;

    // 删除旧文件（如果存在）
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/tianshan_%d.log",
             s_log_ctx.file_path, s_log_ctx.file_index);
    remove(filepath);
}

/* ============================================================================
 * ESP_LOG 捕获钩子
 * ========================================================================== */

/**
 * @brief 解析 ESP_LOG 格式的日志行并存入缓冲区
 *
 * ESP_LOG 格式: "\033[0;32mI (12345) tag: message\033[0m"
 * 或无颜色格式: "I (12345) tag: message"
 */
static void parse_esp_log(const char *log_line, ts_log_entry_t *out, bool clipped) {
    if (log_line == NULL || log_line[0] == '\0') {
        return;
    }

    ts_log_entry_t *entry = out;
    memset(entry, 0, sizeof(*entry));

    const char *p = log_line;

    // 跳过 ANSI 颜色代码 (如 \033[0;32m)
    while (*p == '\033') {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }

    // 解析日志级别字符 (E/W/I/D/V)
    char level_char = *p;
    switch (level_char) {
    case 'E':
        entry->level = TS_LOG_ERROR;
        break;
    case 'W':
        entry->level = TS_LOG_WARN;
        break;
    case 'I':
        entry->level = TS_LOG_INFO;
        break;
    case 'D':
        entry->level = TS_LOG_DEBUG;
        break;
    case 'V':
        entry->level = TS_LOG_VERBOSE;
        break;
    default:
        // 非标准日志格式，作为 INFO 级别存储
        entry->level = TS_LOG_INFO;
        strncpy(entry->tag, "system", TS_LOG_TAG_MAX_LEN - 1);
        if (ts_log_preview(entry->message, sizeof(entry->message), log_line, clipped))
            atomic_fetch_add(&s_log_ctx.capture_truncated, 1);
        entry->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        goto store_entry;
    }
    p++;

    // 跳过空格
    while (*p == ' ') p++;

    // 解析时间戳 (12345)
    if (*p == '(') {
        p++;
        entry->timestamp_ms = (uint32_t)strtoul(p, (char **)&p, 10);
        if (*p == ')') p++;
    } else {
        entry->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }

    // 跳过空格
    while (*p == ' ') p++;

    // 解析 TAG (到冒号为止)
    const char *tag_start = p;
    while (*p && *p != ':') p++;
    size_t tag_len = p - tag_start;
    if (tag_len > 0 && tag_len < TS_LOG_TAG_MAX_LEN) {
        strncpy(entry->tag, tag_start, tag_len);
        entry->tag[tag_len] = '\0';
    }

    // 跳过 ": "
    if (*p == ':') p++;
    while (*p == ' ') p++;

    // 剩余部分是消息
    const char *msg_start = p;

    if (ts_log_preview(entry->message, sizeof(entry->message), msg_start, clipped))
        atomic_fetch_add(&s_log_ctx.capture_truncated, 1);

    // 获取当前任务名
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (task != NULL) {
        strncpy(entry->task_name, pcTaskGetName(task), sizeof(entry->task_name) - 1);
    }

store_entry:
    return;
}

/**
 * @brief vprintf 钩子函数，拦截所有 ESP_LOG 输出
 */
static int ts_log_vprintf_hook(const char *fmt, va_list args)
{
    vprintf_like_t output = atomic_load(&s_log_ctx.original_vprintf);
    if (!output || output == ts_log_vprintf_hook)
        output = vprintf;
    va_list console_args;
    va_copy(console_args, args);
    int result = output(fmt, console_args);
    va_end(console_args);
    if (!atomic_load(&s_log_ctx.esp_log_capture_enabled))
        return result;
    int slot = -1;
    portENTER_CRITICAL(&s_capture_lock);
    for (unsigned i = 0; i < 2; ++i)
        if (!s_capture[i].busy) {
            s_capture[i].busy = true;
            slot = (int)i;
            break;
        }
    portEXIT_CRITICAL(&s_capture_lock);
    if (slot < 0) {
        atomic_fetch_add(&s_log_ctx.capture_dropped, 1);
        return result;
    }
    va_list preview_args;
    va_copy(preview_args, args);
    int len = vsnprintf(s_capture[slot].text, sizeof(s_capture[slot].text), fmt, preview_args);
    va_end(preview_args);
    if (len > 0) {
        parse_esp_log(s_capture[slot].text, &s_capture[slot].entry,
                      (size_t)len >= sizeof(s_capture[slot].text));
        if (s_log_ctx.mutex && xSemaphoreTake(s_log_ctx.mutex, 0) == pdTRUE) {
            if (s_log_ctx.initialized && s_log_ctx.esp_log_capture_enabled) {
                log_output_buffer(&s_capture[slot].entry);
                ++s_log_ctx.total_logs_captured;
            }
            xSemaphoreGive(s_log_ctx.mutex);
        } else
            atomic_fetch_add(&s_log_ctx.capture_dropped, 1);
    }
    portENTER_CRITICAL(&s_capture_lock);
    s_capture[slot].busy = false;
    portEXIT_CRITICAL(&s_capture_lock);
    return result;
}

/* ============================================================================
 * 日志统计和查询 API
 * ========================================================================== */

esp_err_t ts_log_get_stats(ts_log_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_log_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);

    stats->buffer_capacity = s_log_ctx.buffer.capacity;
    stats->buffer_count = s_log_ctx.buffer.count;
    stats->total_captured = s_log_ctx.total_logs_captured;
    stats->dropped = s_log_ctx.logs_dropped;
    stats->capture_dropped = atomic_load(&s_log_ctx.capture_dropped);
    stats->capture_truncated = atomic_load(&s_log_ctx.capture_truncated);
    stats->esp_log_capture_enabled = s_log_ctx.esp_log_capture_enabled;

    xSemaphoreGive(s_log_ctx.mutex);
    return ESP_OK;
}

size_t ts_log_buffer_search(ts_log_entry_t *entries, size_t max_count,
                            ts_log_level_t min_level, ts_log_level_t max_level,
                            const char *tag_filter, const char *keyword)
{
    if (entries == NULL || max_count == 0 || s_log_ctx.buffer.entries == NULL) {
        return 0;
    }

    xSemaphoreTake(s_log_ctx.mutex, portMAX_DELAY);
    if (!s_log_ctx.buffer.entries) {
        xSemaphoreGive(s_log_ctx.mutex);
        return 0;
    }

    size_t found = 0;
    size_t count = s_log_ctx.buffer.count;

    // 从最旧的日志开始遍历
    size_t start = (s_log_ctx.buffer.head + s_log_ctx.buffer.capacity - count)
                   % s_log_ctx.buffer.capacity;

    for (size_t i = 0; i < count && found < max_count; i++) {
        size_t idx = (start + i) % s_log_ctx.buffer.capacity;
        const ts_log_entry_t *e = &s_log_ctx.buffer.entries[idx];

        // 级别过滤
        if (e->level < min_level || e->level > max_level) {
            continue;
        }

        // TAG 过滤（支持子字符串匹配）
        if (tag_filter != NULL && tag_filter[0] != '\0') {
            if (strcasestr(e->tag, tag_filter) == NULL) {
                continue;
            }
        }

        // 关键字过滤（在消息和 TAG 中搜索）
        if (keyword != NULL && keyword[0] != '\0') {
            if (strcasestr(e->message, keyword) == NULL &&
                strcasestr(e->tag, keyword) == NULL) {
                continue;
            }
        }

        // 匹配，复制到结果
        memcpy(&entries[found], e, sizeof(ts_log_entry_t));
        found++;
    }

    xSemaphoreGive(s_log_ctx.mutex);
    return found;
}

void ts_log_enable_esp_capture(bool enable)
{
    if (!s_log_ctx.initialized) {
        return;
    }

    portENTER_CRITICAL(&s_capture_lock);
    if (enable && s_log_ctx.initialized && !s_log_ctx.esp_log_capture_enabled) {
        // 安装钩子
        s_log_ctx.original_vprintf = esp_log_set_vprintf(ts_log_vprintf_hook);
        s_log_ctx.esp_log_capture_enabled = true;
    } else if (!enable && s_log_ctx.esp_log_capture_enabled) {
        // 恢复原始 vprintf
        if (s_log_ctx.original_vprintf) {
            esp_log_set_vprintf(s_log_ctx.original_vprintf);
        }
        s_log_ctx.esp_log_capture_enabled = false;
    }
    portEXIT_CRITICAL(&s_capture_lock);
}
