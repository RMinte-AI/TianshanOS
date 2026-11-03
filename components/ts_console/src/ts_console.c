/**
 * @file ts_console.c
 * @brief TianShanOS Console System Implementation
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_console.h"
#include "ts_i18n.h"
#include "ts_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "ts_console"

/* 保存当前执行的原始命令行，用于需要原始 UTF-8 文本的命令 */
static char s_raw_cmdline[512] = {0};
static SemaphoreHandle_t s_exec_mutex;
static _Atomic(TaskHandle_t) s_exec_owner;
static const ts_console_exec_context_t *s_exec_context;
const ts_console_exec_context_t *ts_console_current_context(void)
{
    return atomic_load(&s_exec_owner) == xTaskGetCurrentTaskHandle() ? s_exec_context : NULL;
}


/*===========================================================================*/
/*                          Category Names                                    */
/*===========================================================================*/

static const char *s_category_names[] = {
    [TS_CMD_CAT_SYSTEM]  = "System",
    [TS_CMD_CAT_CONFIG]  = "Configuration",
    [TS_CMD_CAT_HAL]     = "Hardware",
    [TS_CMD_CAT_LED]     = "LED",
    [TS_CMD_CAT_FAN]     = "Fan",
    [TS_CMD_CAT_POWER]   = "Power",
    [TS_CMD_CAT_NETWORK] = "Network",
    [TS_CMD_CAT_DEVICE]  = "Device",
    [TS_CMD_CAT_DEBUG]   = "Debug",
    [TS_CMD_CAT_USER]    = "User"
};

/*===========================================================================*/
/*                          Command Registry                                  */
/*===========================================================================*/

typedef struct cmd_entry {
    char name[TS_CONSOLE_MAX_CMD_NAME];
    ts_cmd_category_t category;
    struct cmd_entry *next;
} cmd_entry_t;

/*===========================================================================*/
/*                          Private Data                                      */
/*===========================================================================*/

static struct {
    bool initialized;
    atomic_bool running;
    ts_console_config_t config;
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t exit_done;
    TaskHandle_t control_owner;
    bool command_active;
    cmd_entry_t *cmd_list;
    size_t cmd_count;
    ts_console_output_cb_t output_cb;
    void *output_user_data;
    char prompt[TS_CONSOLE_MAX_PROMPT_LENGTH];
    /* 中断处理相关 */
    volatile bool interrupt_requested;
    volatile bool interruptible_mode;
} s_console = {0};

/*===========================================================================*/
/*                       Interrupt Handling                                   */
/*===========================================================================*/

bool ts_console_interrupted(void)
{
    const ts_console_exec_context_t *ctx = ts_console_current_context();
    return ctx && ctx->cancelled ? ctx->cancelled(ctx->user_data) : s_console.interrupt_requested;
}

void ts_console_clear_interrupt(void)
{
    if (!ts_console_current_context()) s_console.interrupt_requested = false;
}

void ts_console_request_interrupt(void)
{
    s_console.interrupt_requested = true;
}

void ts_console_begin_interruptible(void)
{
    ts_console_clear_interrupt();
    s_console.interruptible_mode = true;
}

void ts_console_end_interruptible(void)
{
    s_console.interruptible_mode = false;
    ts_console_clear_interrupt();
}

/**
 * @brief 检查是否有 Ctrl+C 输入（非阻塞）
 * 
 * 在命令执行期间调用，检测用户是否按下 Ctrl+C
 * @note Reserved for interruptible command feature
 */
__attribute__((unused))
static void check_for_interrupt(void)
{
    if (!s_console.interruptible_mode) {
        return;
    }
    
    /* 非阻塞检查 UART 输入 */
    uint8_t ch;
    int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, 0);
    if (len > 0 && ch == 0x03) {  /* Ctrl+C = ASCII 0x03 */
        s_console.interrupt_requested = true;
        ts_console_printf("\n^C\n");
    }
}

/*===========================================================================*/
/*                          Private Functions                                 */
/*===========================================================================*/

static ssize_t console_read(int fd, void *buffer, size_t size)
{
    (void)fd;
    while (s_console.running) {
        int count = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buffer, size, pdMS_TO_TICKS(50));
        if (!s_console.running) return -1;
        if (count > 0) {
            for (int i = 0; i < count; i++) {
                if (((char *)buffer)[i] == '\r') ((char *)buffer)[i] = '\n';
            }
            return count;
        }
    }
    return -1;
}

static void console_task(void *arg)
{
    TS_LOGI(TAG, "Console task started");
    
    /* Configure linenoise */
    linenoiseSetMultiLine(0);  /* Single line mode works better in dumb mode */
    linenoiseSetDumbMode(1);   /* Use dumb mode for serial monitor compatibility */
    if (s_console.config.output == TS_CONSOLE_OUTPUT_UART) linenoiseSetReadFunction(console_read);
    linenoiseHistorySetMaxLen(s_console.config.max_history);
    
    /* Load history if available */
#ifdef CONFIG_TS_CONSOLE_HISTORY_FILE
    linenoiseHistoryLoad(CONFIG_TS_CONSOLE_HISTORY_FILE);
#endif
    
    while (s_console.running) {
        /* Get a line of input - linenoise blocks waiting for input in dumb mode */
        char *line = linenoise(s_console.prompt);
        
        if (line == NULL) {
            if (!s_console.running) break;
            /* EOF, error, or Ctrl+C - add small delay before retry */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        xSemaphoreTake(s_console.mutex, portMAX_DELAY);
        if (!s_console.running || (s_console.control_owner &&
            s_console.control_owner != xTaskGetCurrentTaskHandle())) {
            xSemaphoreGive(s_console.mutex);
            linenoiseFree(line);
            break;
        }
        s_console.command_active = true;
        xSemaphoreGive(s_console.mutex);

        /* Only process non-empty lines */
        if (strlen(line) > 0) {
            /* Add to history */
            linenoiseHistoryAdd(line);
            
            ts_cmd_result_t result;
            esp_err_t err = ts_console_exec(line, &result);
            int ret = result.code;

            if (err == ESP_ERR_NOT_FOUND) {
                ts_console_error("Unknown command: %s\n", line);
            } else if (err == ESP_ERR_INVALID_ARG) {
                /* Command was empty or whitespace only */
            } else if (err != ESP_OK) {
                ts_console_error("Error: %s\n", esp_err_to_name(err));
            } else if (ret != 0) {
                ts_console_error("Command returned error: %d\n", ret);
            }
        }
        /* For empty lines, just free and loop - linenoise handles prompt */
        
        linenoiseFree(line);
        xSemaphoreTake(s_console.mutex, portMAX_DELAY);
        s_console.command_active = false;
        xSemaphoreGive(s_console.mutex);
    }
    
    /* Save history before exit */
#ifdef CONFIG_TS_CONSOLE_HISTORY_FILE
    linenoiseHistorySave(CONFIG_TS_CONSOLE_HISTORY_FILE);
#endif
    
    TS_LOGI(TAG, "Console task stopped");
    xSemaphoreTake(s_console.mutex, portMAX_DELAY);
    s_console.task_handle = NULL;
    xSemaphoreGive(s_console.exit_done);
    if (s_console.control_owner == xTaskGetCurrentTaskHandle()) s_console.control_owner = NULL;
    xSemaphoreGive(s_console.mutex);
    vTaskDelete(NULL);
}

static void add_cmd_to_registry(const char *name, ts_cmd_category_t category)
{
    /* PSRAM first for reduced DRAM fragmentation */
    cmd_entry_t *entry = heap_caps_malloc(sizeof(cmd_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entry) entry = malloc(sizeof(cmd_entry_t));
    if (entry == NULL) {
        return;
    }
    
    strncpy(entry->name, name, TS_CONSOLE_MAX_CMD_NAME - 1);
    entry->name[TS_CONSOLE_MAX_CMD_NAME - 1] = '\0';
    entry->category = category;
    entry->next = s_console.cmd_list;
    s_console.cmd_list = entry;
    s_console.cmd_count++;
}

static void remove_cmd_from_registry(const char *name)
{
    cmd_entry_t **pp = &s_console.cmd_list;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            cmd_entry_t *entry = *pp;
            *pp = entry->next;
            free(entry);
            s_console.cmd_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void free_cmd_registry(void)
{
    cmd_entry_t *entry = s_console.cmd_list;
    while (entry) {
        cmd_entry_t *next = entry->next;
        free(entry);
        entry = next;
    }
    s_console.cmd_list = NULL;
    s_console.cmd_count = 0;
}

/*===========================================================================*/
/*                          Core API Implementation                           */
/*===========================================================================*/

esp_err_t ts_console_init(const ts_console_config_t *config)
{
    if (s_console.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Initialize i18n (loads system.language from config for help/ssh --help etc.) */
    esp_err_t i18n_ret = ts_i18n_init();
    if (i18n_ret != ESP_OK) {
        return i18n_ret;
    }
    
    /* Use defaults if no config provided */
    if (config) {
        s_console.config = *config;
    } else {
        ts_console_config_t default_config = TS_CONSOLE_DEFAULT_CONFIG();
        s_console.config = default_config;
    }
    
    /* Create mutex */
    s_console.mutex = xSemaphoreCreateMutex();
    if (s_console.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    s_console.exit_done = xSemaphoreCreateBinary();
    if (!s_console.exit_done) {
        vSemaphoreDelete(s_console.mutex);
        return ESP_ERR_NO_MEM;
    }
    s_exec_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_exec_mutex) { vSemaphoreDelete(s_console.exit_done); vSemaphoreDelete(s_console.mutex); s_console.mutex = NULL; return ESP_ERR_NO_MEM; }

    /* Set prompt */
    strncpy(s_console.prompt, s_console.config.prompt, TS_CONSOLE_MAX_PROMPT_LENGTH - 1);
    s_console.prompt[TS_CONSOLE_MAX_PROMPT_LENGTH - 1] = '\0';
    
    /* Initialize esp_console */
    esp_console_config_t console_config = {
        .max_cmdline_args = TS_CONSOLE_MAX_ARGS,
        .max_cmdline_length = TS_CONSOLE_MAX_LINE_LENGTH,
        .hint_color = atoi("36"),  /* cyan */
        .hint_bold = 0
    };
    
    esp_err_t ret = esp_console_init(&console_config);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_console.exit_done);
        vSemaphoreDelete(s_console.mutex);
        vSemaphoreDelete(s_exec_mutex); s_exec_mutex = NULL;
        return ret;
    }
    
    /* Configure UART for console */
    if (s_console.config.output == TS_CONSOLE_OUTPUT_UART) {
        /* Drain stdout before reconfiguring */
        fflush(stdout);
        fsync(fileno(stdout));
        
        /* Install UART driver for interrupt-driven reads and writes */
        const int uart_num = CONFIG_ESP_CONSOLE_UART_NUM;
        const uart_config_t uart_config = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .source_clk = UART_SCLK_DEFAULT,
        };
        /* Install UART driver with RX buffer only */
        ESP_ERROR_CHECK(uart_driver_install(uart_num, 256, 0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
        
        /* Tell VFS to use UART driver - this makes stdin blocking */
        uart_vfs_dev_use_driver(uart_num);
        
        /* Configure line endings */
        uart_vfs_dev_port_set_rx_line_endings(uart_num, ESP_LINE_ENDINGS_CR);
        uart_vfs_dev_port_set_tx_line_endings(uart_num, ESP_LINE_ENDINGS_CRLF);
        
        /* Disable buffering on stdin */
        setvbuf(stdin, NULL, _IONBF, 0);
    }
    
    s_console.initialized = true;
    TS_LOGI(TAG, "Console initialized");
    
    return ESP_OK;
}

/* Claim this input lifecycle without holding the mutex while waiting for its reader. */
static esp_err_t stop_console(bool keep_control)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    xSemaphoreTake(s_console.mutex, portMAX_DELAY);
    if ((s_console.control_owner && s_console.control_owner != caller) ||
        (s_console.command_active && s_console.task_handle != caller) ||
        (keep_control && s_console.task_handle == caller)) {
        xSemaphoreGive(s_console.mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    s_console.control_owner = caller;
    s_console.running = false;
    TaskHandle_t reader = s_console.task_handle;
    xSemaphoreGive(s_console.mutex);

    if (reader && reader != caller) xSemaphoreTake(s_console.exit_done, portMAX_DELAY);
    if (reader != caller) {
        xSemaphoreTake(s_console.mutex, portMAX_DELAY);
        if (!keep_control) s_console.control_owner = NULL;
        xSemaphoreGive(s_console.mutex);
    }
    return ESP_OK;
}

esp_err_t ts_console_deinit(void)
{
    if (!s_console.initialized) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&s_exec_owner)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = stop_console(true);
    if (ret != ESP_OK) return ret;
    free_cmd_registry();
    esp_console_deinit();
    if (s_console.config.output == TS_CONSOLE_OUTPUT_UART) {
        linenoiseSetReadFunction(read);
        uart_vfs_dev_use_nonblocking(CONFIG_ESP_CONSOLE_UART_NUM);
        uart_driver_delete(CONFIG_ESP_CONSOLE_UART_NUM);
    }
    vSemaphoreDelete(s_console.exit_done);
    s_console.exit_done = NULL;
    vSemaphoreDelete(s_console.mutex);
    s_console.mutex = NULL;
    if (s_exec_mutex) { vSemaphoreDelete(s_exec_mutex); s_exec_mutex = NULL; }
    s_console.initialized = false;
    s_console.control_owner = NULL;
    return ESP_OK;
}

esp_err_t ts_console_start(void)
{
    if (!s_console.initialized) return ESP_ERR_INVALID_STATE;
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    xSemaphoreTake(s_console.mutex, portMAX_DELAY);
    if (s_console.control_owner) {
        if (s_console.control_owner == caller && s_console.task_handle == caller) {
            s_console.running = true;
            s_console.control_owner = NULL;
            ts_console_clear_interrupt();
            xSemaphoreGive(s_console.mutex);
            return ESP_OK;
        }
        xSemaphoreGive(s_console.mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    if (s_console.running) {
        xSemaphoreGive(s_console.mutex);
        return ESP_OK;
    }
    if (s_console.task_handle) {
        xSemaphoreGive(s_console.mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    xSemaphoreTake(s_console.exit_done, 0);
    s_console.running = true;
    BaseType_t ret = xTaskCreate(console_task, "ts_console", s_console.config.task_stack_size,
                                NULL, s_console.config.task_priority, &s_console.task_handle);
    if (ret != pdPASS) {
        s_console.running = false;
        s_console.task_handle = NULL;
    }
    xSemaphoreGive(s_console.mutex);
    return ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ts_console_stop(void)
{
    if (!s_console.initialized) return ESP_OK;
    return stop_console(false);
}

bool ts_console_is_initialized(void)
{
    return s_console.initialized;
}

bool ts_console_is_running(void)
{
    return s_console.running;
}

/*===========================================================================*/
/*                      Command Registration                                  */
/*===========================================================================*/

esp_err_t ts_console_register_cmd(const ts_console_cmd_t *cmd)
{
    if (cmd == NULL || cmd->command == NULL || cmd->func == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Register with esp_console */
    esp_console_cmd_t esp_cmd = {
        .command = cmd->command,
        .help = cmd->help,
        .hint = cmd->hint,
        .func = cmd->func,
        .argtable = cmd->argtable
    };
    
    esp_err_t ret = esp_console_cmd_register(&esp_cmd);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* Add to our registry for category tracking */
    xSemaphoreTake(s_console.mutex, portMAX_DELAY);
    add_cmd_to_registry(cmd->command, cmd->category);
    xSemaphoreGive(s_console.mutex);
    
    TS_LOGD(TAG, "Registered command: %s (category: %s)", 
            cmd->command, s_category_names[cmd->category]);
    
    return ESP_OK;
}

esp_err_t ts_console_register_cmds(const ts_console_cmd_t *cmds, size_t count)
{
    if (cmds == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (size_t i = 0; i < count; i++) {
        esp_err_t ret = ts_console_register_cmd(&cmds[i]);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to register command: %s", cmds[i].command);
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_console_unregister_cmd(const char *cmd_name)
{
    if (cmd_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_console.mutex, portMAX_DELAY);
    remove_cmd_from_registry(cmd_name);
    xSemaphoreGive(s_console.mutex);
    
    /* Note: esp_console doesn't provide unregister API */
    return ESP_OK;
}

size_t ts_console_get_cmd_count(void)
{
    return s_console.cmd_count;
}

size_t ts_console_get_cmds_by_category(ts_cmd_category_t category,
                                        const char **cmds, size_t max_cmds)
{
    size_t count = 0;
    
    xSemaphoreTake(s_console.mutex, portMAX_DELAY);
    
    for (cmd_entry_t *entry = s_console.cmd_list; entry; entry = entry->next) {
        if (entry->category == category) {
            if (cmds && count < max_cmds) {
                cmds[count] = entry->name;
            }
            count++;
        }
    }
    
    xSemaphoreGive(s_console.mutex);
    
    return count;
}

/*===========================================================================*/
/*                      Command Execution                                     */
/*===========================================================================*/

esp_err_t ts_console_exec_with_context(const char *cmdline, ts_cmd_result_t *result,
                                     const ts_console_exec_context_t *context)
{
    if (result) *result = (ts_cmd_result_t){.code = -1};
    if (!cmdline) return ESP_ERR_INVALID_ARG;
    if (!s_exec_mutex || xSemaphoreTakeRecursive(s_exec_mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    bool outer = atomic_load(&s_exec_owner) == NULL;
    char previous[sizeof(s_raw_cmdline)];
    memcpy(previous, s_raw_cmdline, sizeof(previous));
    if (outer) {
        s_exec_context = context;
        s_console.interrupt_requested = false;
        atomic_store(&s_exec_owner, xTaskGetCurrentTaskHandle());
    }
    snprintf(s_raw_cmdline, sizeof(s_raw_cmdline), "%s", cmdline);
    int code = -1;
    esp_err_t err = ts_console_interrupted() ? ESP_ERR_TIMEOUT : esp_console_run(cmdline, &code);
    if (result) result->code = err == ESP_OK ? code : -1;
    memcpy(s_raw_cmdline, previous, sizeof(previous));
    if (outer) { atomic_store(&s_exec_owner, NULL); s_exec_context = NULL; }
    xSemaphoreGiveRecursive(s_exec_mutex);
    return err;
}
esp_err_t ts_console_exec(const char *cmdline, ts_cmd_result_t *result)
{
    return ts_console_exec_with_context(cmdline, result, NULL);
}

esp_err_t ts_console_exec_cmd(const char *cmd_name, int argc, char **argv,
                               ts_cmd_result_t *result)
{
    if (cmd_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Build command line */
    char cmdline[TS_CONSOLE_MAX_LINE_LENGTH];
    int offset = snprintf(cmdline, sizeof(cmdline), "%s", cmd_name);
    
    for (int i = 0; i < argc && offset < sizeof(cmdline) - 1; i++) {
        if (argv[i]) {
            offset += snprintf(cmdline + offset, sizeof(cmdline) - offset, " %s", argv[i]);
        }
    }
    
    return ts_console_exec(cmdline, result);
}

/*===========================================================================*/
/*                              Output                                        */
/*===========================================================================*/

/* One formatted message is kept complete below this bound; longer output is
 * cut on a UTF-8 boundary and followed by an explicit truncation notice. */
#define TS_CONSOLE_MAX_OUTPUT 16384

static size_t utf8_complete_prefix(const char *s, size_t n)
{
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return 0;
    unsigned char c = (unsigned char)s[i - 1];
    size_t w = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
             : (c & 0xF8) == 0xF0 ? 4 : 1;
    return n - (i - 1) < w ? i - 1 : n;
}

static void console_emit(const char *data, size_t len)
{
    const ts_console_exec_context_t *ctx = ts_console_current_context();
    if (ctx && ctx->output) ctx->output(data, len, ctx->user_data);
    else if (s_console.output_cb) s_console.output_cb(data, len, s_console.output_user_data);
    fwrite(data, 1, len, stdout);
}

static int do_printf(const char *prefix, const char *fmt, va_list args)
{
    char stack[512];
    const size_t plen = prefix ? strlen(prefix) : 0;

    va_list probe;
    va_copy(probe, args);
    int body = vsnprintf(stack, sizeof(stack), fmt, probe);
    va_end(probe);
    if (body < 0) return -1;

    const size_t need = plen + (size_t)body;
    size_t limit = need < TS_CONSOLE_MAX_OUTPUT ? need : TS_CONSOLE_MAX_OUTPUT;
    char *buf = stack;
    if (limit + 1 > sizeof(stack)) {
        buf = heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = malloc(limit + 1);
        if (!buf) { buf = stack; limit = sizeof(stack) - 1; }
    }

    size_t off = plen < limit ? plen : limit;
    if (off) memcpy(buf, prefix, off);
    bool failed = false;
    if (off < limit) {
        va_list build;
        va_copy(build, args);
        int w = vsnprintf(buf + off, limit - off + 1, fmt, build);
        va_end(build);
        if (w < 0) failed = true;
        else if (w > 0) off += (size_t)w < limit - off ? (size_t)w : limit - off;
    }

    const bool cut = need > limit || failed;
    size_t shown = strnlen(buf, off);  /* keep the old text/NUL contract */
    if (cut) shown = utf8_complete_prefix(buf, shown);
    buf[shown] = '\0';
    console_emit(buf, shown);

    int printed = (int)shown;
    if (cut) {
        char notice[96];
        int n = snprintf(notice, sizeof(notice),
                         "%s[console output truncated: %u of %u bytes shown]\n",
                         shown && buf[shown - 1] != '\n' ? "\n" : "",
                         (unsigned)shown, (unsigned)need);
        if (n > 0) {
            console_emit(notice, (size_t)n);
            printed += n;
        }
    }
    if (buf != stack) free(buf);
    return printed;
}

int ts_console_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = do_printf(NULL, fmt, args);
    va_end(args);
    return ret;
}

int ts_console_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
#ifdef CONFIG_TS_CONSOLE_ENABLE_COLORS
    int ret = do_printf("\033[31mError: \033[0m", fmt, args);
#else
    int ret = do_printf("Error: ", fmt, args);
#endif
    va_end(args);
    return ret;
}

int ts_console_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
#ifdef CONFIG_TS_CONSOLE_ENABLE_COLORS
    int ret = do_printf("\033[33mWarning: \033[0m", fmt, args);
#else
    int ret = do_printf("Warning: ", fmt, args);
#endif
    va_end(args);
    return ret;
}

int ts_console_success(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
#ifdef CONFIG_TS_CONSOLE_ENABLE_COLORS
    int ret = do_printf("\033[32m", fmt, args);
    printf("\033[0m");
#else
    int ret = do_printf(NULL, fmt, args);
#endif
    va_end(args);
    return ret;
}

esp_err_t ts_console_set_output_cb(ts_console_output_cb_t cb, void *user_data)
{
    s_console.output_cb = cb;
    s_console.output_user_data = user_data;
    return ESP_OK;
}

esp_err_t ts_console_clear_output_cb(void)
{
    s_console.output_cb = NULL;
    s_console.output_user_data = NULL;
    return ESP_OK;
}

/*===========================================================================*/
/*                      Prompt Management                                     */
/*===========================================================================*/

esp_err_t ts_console_set_prompt(const char *prompt)
{
    if (prompt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_console.prompt, prompt, TS_CONSOLE_MAX_PROMPT_LENGTH - 1);
    s_console.prompt[TS_CONSOLE_MAX_PROMPT_LENGTH - 1] = '\0';
    
    return ESP_OK;
}

const char *ts_console_get_prompt(void)
{
    return s_console.prompt;
}

/*===========================================================================*/
/*                      History Management                                    */
/*===========================================================================*/

esp_err_t ts_console_history_add(const char *cmdline)
{
    if (cmdline == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    linenoiseHistoryAdd(cmdline);
    return ESP_OK;
}

esp_err_t ts_console_history_clear(void)
{
    /* Linenoise doesn't have a clear function, so we set max to 0 and back */
    int max = s_console.config.max_history;
    linenoiseHistorySetMaxLen(0);
    linenoiseHistorySetMaxLen(max);
    return ESP_OK;
}

esp_err_t ts_console_history_save(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (linenoiseHistorySave(path) != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_console_history_load(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (linenoiseHistoryLoad(path) != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Utility                                           */
/*===========================================================================*/

const char *ts_console_category_name(ts_cmd_category_t category)
{
    if (category >= TS_CMD_CAT_MAX) {
        return "Unknown";
    }
    return s_category_names[category];
}

const char *ts_console_get_raw_cmdline(void)
{
    return s_raw_cmdline;
}
