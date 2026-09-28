/**
 * @file ts_automation_types.h
 * @brief TianShanOS Automation Engine - Type Definitions
 *
 * Defines all internal types for sources, rules, actions, and variables.
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_AUTOMATION_TYPES_H
#define TS_AUTOMATION_TYPES_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                           Constants                                        */
/*===========================================================================*/

#define TS_AUTO_NAME_MAX_LEN        64   /**< Max length for names/IDs (increased for compound names like "source.suffix") */
#define TS_AUTO_LABEL_MAX_LEN       48   /**< Max length for display labels */
#define TS_AUTO_PATH_MAX_LEN        96   /**< Max length for paths */
#define TS_AUTO_EXPR_MAX_LEN        64   /**< Max length for expressions */
#define TS_AUTO_VALUE_STR_MAX_LEN   64   /**< Max length for string values */
#define TS_AUTO_MAX_MAPPINGS        4    /**< Max mappings per source */

/*===========================================================================*/
/*                           Value Type                                       */
/*===========================================================================*/

/**
 * @brief Variable value types
 */
typedef enum {
    TS_AUTO_VAL_NULL = 0,               /**< Null/undefined */
    TS_AUTO_VAL_BOOL,                    /**< Boolean */
    TS_AUTO_VAL_INT,                     /**< 32-bit signed integer */
    TS_AUTO_VAL_FLOAT,                   /**< Float (double precision) */
    TS_AUTO_VAL_STRING,                  /**< String */
} ts_auto_value_type_t;

/**
 * @brief Variable value union
 */
typedef struct {
    ts_auto_value_type_t type;
    union {
        bool bool_val;
        int32_t int_val;
        double float_val;
        char str_val[TS_AUTO_VALUE_STR_MAX_LEN];
    };
} ts_auto_value_t;

/*===========================================================================*/
/*                           Data Source                                      */
/*===========================================================================*/

/**
 * @brief Data source types
 */
typedef enum {
    TS_AUTO_SRC_BUILTIN = 0,            /**< ESP32 builtin (HAL/drivers) */
    TS_AUTO_SRC_WEBSOCKET,               /**< WebSocket endpoint */
    TS_AUTO_SRC_SOCKETIO,                /**< Socket.IO v4 endpoint */
    TS_AUTO_SRC_REST,                    /**< REST API endpoint */
    TS_AUTO_SRC_VARIABLE,                /**< ts_var variable system */
} ts_auto_source_type_t;

/**
 * @brief Builtin source subtypes (ESP32 HAL/driver mappings)
 */
typedef enum {
    TS_AUTO_BUILTIN_GPIO = 0,           /**< GPIO input - ts_hal_gpio */
    TS_AUTO_BUILTIN_ADC,                 /**< ADC channel - ts_hal_adc */
    TS_AUTO_BUILTIN_POWER,               /**< Power monitor - ts_power_monitor */
    TS_AUTO_BUILTIN_AGX_STATUS,          /**< AGX status - ts_device_ctrl */
    TS_AUTO_BUILTIN_LPMU_STATUS,         /**< LPMU status - ts_device_ctrl */
    TS_AUTO_BUILTIN_CHIP_TEMP,           /**< ESP32 chip temperature */
    TS_AUTO_BUILTIN_HEAP_FREE,           /**< Free heap memory */
    TS_AUTO_BUILTIN_UPTIME,              /**< System uptime */
} ts_auto_builtin_type_t;

/**
 * @brief Builtin source configuration
 */
typedef struct {
    ts_auto_builtin_type_t type;         /**< Builtin source type */
    union {
        struct {
            uint8_t pin;                 /**< GPIO pin number */
        } gpio;
        struct {
            uint8_t channel;             /**< ADC channel */
            uint8_t atten;               /**< Attenuation */
        } adc;
        struct {
            uint8_t device_index;        /**< AGX/LPMU device index */
        } device;
    };
} ts_auto_builtin_config_t;

/**
 * @brief Data mapping: JSONPath -> Variable
 *
 * Maps a JSON path from source data to a variable name.
 * Used by WebSocket, Socket.IO, and REST sources.
 */
typedef struct {
    char json_path[TS_AUTO_PATH_MAX_LEN];  /**< JSONPath expression (e.g., "cpu.cores[0].freq") */
    char var_name[TS_AUTO_NAME_MAX_LEN];   /**< Target variable name (e.g., "lpmu.cpu_freq") */
    char transform[TS_AUTO_EXPR_MAX_LEN];  /**< Optional transform expression */
} ts_auto_mapping_t;

/**
 * @brief WebSocket source configuration
 */
typedef struct {
    char uri[TS_AUTO_PATH_MAX_LEN];     /**< WebSocket URI */
    char path[TS_AUTO_NAME_MAX_LEN];    /**< JSON path to extract (deprecated, use mappings) */
    uint16_t reconnect_ms;               /**< Reconnect interval */
} ts_auto_ws_config_t;

/**
 * @brief Socket.IO source configuration
 */
typedef struct {
    char url[TS_AUTO_PATH_MAX_LEN];      /**< Server URL (http://host:port) */
    char event[TS_AUTO_NAME_MAX_LEN];    /**< Event name to subscribe */
    uint16_t reconnect_ms;               /**< Reconnect interval */
} ts_auto_sio_config_t;

/**
 * @brief REST source configuration
 */
typedef struct {
    char url[TS_AUTO_PATH_MAX_LEN];     /**< REST endpoint URL */
    char path[TS_AUTO_NAME_MAX_LEN];    /**< JSON path to extract (deprecated, use mappings) */
    char method[8];                      /**< HTTP method (GET/POST) */
    char auth_header[TS_AUTO_LABEL_MAX_LEN]; /**< Authorization header */
} ts_auto_rest_config_t;

/**
 * @brief Variable source configuration (ts_var integration)
 *
 * Two modes:
 * 1. SSH Command Mode: Execute SSH command periodically, store results in variables
 * 2. Watch Mode: Monitor existing variables for changes (deprecated)
 */
typedef struct {
    /* SSH Command Mode (primary) */
    char ssh_host_id[TS_AUTO_NAME_MAX_LEN];  /**< SSH host ID (from hosts config) */
    char ssh_command[TS_AUTO_PATH_MAX_LEN];  /**< Command to execute */
    char var_prefix[TS_AUTO_NAME_MAX_LEN];   /**< Variable prefix (command alias + ".") */
    char expect_pattern[64];                  /**< Success match pattern */
    char fail_pattern[64];                    /**< Failure match pattern */
    char extract_pattern[64];                 /**< Value extraction regex */
    uint16_t timeout_sec;                     /**< Command timeout in seconds */
    
    /* Watch Mode (legacy) */
    char var_name[TS_AUTO_NAME_MAX_LEN];     /**< Single variable to watch */
    bool watch_all;                           /**< Watch all variables with prefix */
} ts_auto_var_config_t;

/**
 * @brief Data source definition
 */
typedef struct {
    char id[TS_AUTO_NAME_MAX_LEN];      /**< Unique source ID */
    char label[TS_AUTO_LABEL_MAX_LEN];  /**< Display label */
    ts_auto_source_type_t type;          /**< Source type */
    uint32_t poll_interval_ms;           /**< Polling interval (0 = event-driven) */
    bool enabled;                        /**< Is source enabled */
    bool connected;                      /**< Is source connected (runtime) */
    bool auto_discover;                  /**< Auto-discover JSON fields as variables */
    bool auto_discovered;                /**< Auto-discovery completed (runtime, prevents duplicates) */

    union {
        ts_auto_builtin_config_t builtin;
        ts_auto_ws_config_t websocket;
        ts_auto_sio_config_t socketio;   /**< Socket.IO config */
        ts_auto_rest_config_t rest;
        ts_auto_var_config_t variable;   /**< Variable system config */
    };

    /* Data mappings: JSONPath -> Variable */
    ts_auto_mapping_t mappings[TS_AUTO_MAX_MAPPINGS]; /**< Path-to-variable mappings */
    uint8_t mapping_count;               /**< Number of active mappings */

    ts_auto_value_t last_value;          /**< Last received value (deprecated) */
    int64_t last_update_ms;              /**< Last update timestamp */
} ts_auto_source_t;

/*===========================================================================*/
/*                           Variables                                        */
/*===========================================================================*/

/**
 * @brief Variable flags
 */
typedef enum {
    TS_AUTO_VAR_PERSISTENT  = (1 << 0), /**< Persist to NVS */
    TS_AUTO_VAR_READONLY    = (1 << 1), /**< Read-only (from source) */
    TS_AUTO_VAR_COMPUTED    = (1 << 2), /**< Computed from expression */
} ts_auto_var_flags_t;

/**
 * @brief Variable definition
 */
typedef struct {
    char name[TS_AUTO_NAME_MAX_LEN];    /**< Variable name (e.g., "agx.power") */
    char source_id[TS_AUTO_NAME_MAX_LEN]; /**< Source ID or empty for computed */
    char expression[TS_AUTO_EXPR_MAX_LEN]; /**< Transform expression */
    ts_auto_value_t value;               /**< Current value */
    ts_auto_value_t default_value;       /**< Default value */
    uint32_t flags;                      /**< Variable flags */
    int64_t last_change_ms;              /**< Last change timestamp */
    int64_t last_update_ms;              /**< Last update timestamp */
} ts_auto_variable_t;

/*===========================================================================*/
/*                           Rules                                            */
/*===========================================================================*/

/**
 * @brief Comparison operators
 */
typedef enum {
    TS_AUTO_OP_EQ = 0,                  /**< Equal (==) */
    TS_AUTO_OP_NE,                       /**< Not equal (!=) */
    TS_AUTO_OP_LT,                       /**< Less than (<) */
    TS_AUTO_OP_LE,                       /**< Less than or equal (<=) */
    TS_AUTO_OP_GT,                       /**< Greater than (>) */
    TS_AUTO_OP_GE,                       /**< Greater than or equal (>=) */
    TS_AUTO_OP_CONTAINS,                 /**< String contains */
    TS_AUTO_OP_MATCHES,                  /**< Regex matches */
    TS_AUTO_OP_CHANGED,                  /**< Value changed */
    TS_AUTO_OP_CHANGED_TO,               /**< Changed to specific value */
} ts_auto_operator_t;

/**
 * @brief Logical operators for combining conditions
 */
typedef enum {
    TS_AUTO_LOGIC_AND = 0,              /**< All conditions must be true */
    TS_AUTO_LOGIC_OR,                    /**< Any condition must be true */
} ts_auto_logic_t;

/**
 * @brief Rule condition
 */
typedef struct {
    char variable[TS_AUTO_NAME_MAX_LEN]; /**< Variable name to check */
    ts_auto_operator_t op;               /**< Comparison operator */
    ts_auto_value_t value;               /**< Comparison value */
} ts_auto_condition_t;

/**
 * @brief Rule condition group
 */
typedef struct {
    ts_auto_condition_t *conditions;     /**< Array of conditions */
    uint8_t count;                       /**< Number of conditions */
    ts_auto_logic_t logic;               /**< AND or OR */
} ts_auto_condition_group_t;

/*===========================================================================*/
/*                           Actions                                          */
/*===========================================================================*/

/**
 * @brief Action types
 */
typedef enum {
    TS_AUTO_ACT_LED = 0,                /**< LED control */
    TS_AUTO_ACT_SSH_CMD,                 /**< SSH command execution (direct) - deprecated */
    TS_AUTO_ACT_GPIO,                    /**< GPIO output control - deprecated, use CLI */
    TS_AUTO_ACT_WEBHOOK,                 /**< HTTP webhook call */
    TS_AUTO_ACT_LOG,                     /**< Log message */
    TS_AUTO_ACT_SET_VAR,                 /**< Set variable value */
    TS_AUTO_ACT_DEVICE_CTRL,             /**< Device control - deprecated, use CLI */
    TS_AUTO_ACT_SSH_CMD_REF,             /**< SSH command reference (from registered commands) */
    TS_AUTO_ACT_CLI,                     /**< TianShanOS CLI command execution */
    TS_AUTO_ACT_TEMPLATE_REF, /**< Unresolved template-only reference; never execute directly. */
} ts_auto_action_type_t;

/**
 * @brief LED control type
 */
typedef enum {
    TS_LED_CTRL_FILL = 0,       /**< Fill with solid color */
    TS_LED_CTRL_EFFECT,         /**< Run effect animation */
    TS_LED_CTRL_BRIGHTNESS,     /**< Adjust brightness only */
    TS_LED_CTRL_OFF,            /**< Turn off */
    TS_LED_CTRL_TEXT,           /**< Display text (matrix only) */
    TS_LED_CTRL_IMAGE,          /**< Display image (matrix only) */
    TS_LED_CTRL_QRCODE,         /**< Display QR code (matrix only) */
    TS_LED_CTRL_FILTER,         /**< Apply post-processing filter (matrix only) */
    TS_LED_CTRL_FILTER_STOP,    /**< Stop filter (matrix only) */
    TS_LED_CTRL_TEXT_STOP,      /**< Stop text overlay (matrix only) */
} ts_led_ctrl_type_t;

/**
 * @brief LED action parameters
 */
typedef struct {
    char device[TS_AUTO_NAME_MAX_LEN];  /**< LED device (board/touch/matrix) */
    ts_led_ctrl_type_t ctrl_type;       /**< Control type */
    uint8_t index;                       /**< LED index or 0xFF for all */
    uint8_t r, g, b;                     /**< RGB color */
    uint8_t brightness;                  /**< Brightness (0-255) */
    char effect[TS_AUTO_NAME_MAX_LEN];  /**< Effect name (for EFFECT type) */
    uint16_t duration_ms;                /**< Duration, 0 = permanent */
    uint8_t speed;                       /**< Speed parameter (0-100) */
    
    /* Matrix advanced features */
    char text[128];                      /**< Text content (for TEXT type) */
    char font[32];                       /**< Font name (for TEXT type) */
    char image_path[128];                /**< Image path (for IMAGE type) */
    char qr_text[256];                   /**< QR code content (for QRCODE type) */
    char qr_ecc;                         /**< QR ECC level: L/M/Q/H */
    char filter[32];                     /**< Filter name (for FILTER type) */
    bool center;                         /**< Center image/text */
    bool loop;                           /**< Loop text scroll */
    char scroll[16];                     /**< Scroll direction: none/left/right/up/down */
    char align[16];                      /**< Text alignment: left/center/right */
    int16_t x, y;                        /**< Position offset */
} ts_auto_action_led_t;

/**
 * @brief SSH command action parameters
 */
typedef struct {
    char host_ref[TS_AUTO_NAME_MAX_LEN]; /**< Host reference from config */
    char command[TS_AUTO_PATH_MAX_LEN]; /**< Command to execute */
    bool async;                          /**< Execute asynchronously */
    uint32_t timeout_ms;                 /**< Timeout in ms */
} ts_auto_action_ssh_t;

/**
 * @brief GPIO action parameters
 */
typedef struct {
    uint8_t pin;                         /**< GPIO pin number */
    bool level;                          /**< Output level */
    uint32_t pulse_ms;                   /**< Pulse duration (0 = set level) */
} ts_auto_action_gpio_t;

/**
 * @brief Webhook action parameters
 */
typedef struct {
    char url[TS_AUTO_PATH_MAX_LEN];     /**< Webhook URL */
    char method[8];                      /**< HTTP method */
    char body_template[TS_AUTO_EXPR_MAX_LEN]; /**< JSON body template */
} ts_auto_action_webhook_t;

/**
 * @brief Log action parameters
 */
typedef struct {
    uint8_t level;                       /**< Log level (ESP_LOG_*) */
    char message[TS_AUTO_LABEL_MAX_LEN]; /**< Message template */
} ts_auto_action_log_t;

/**
 * @brief Set variable action parameters
 */
typedef struct {
    char variable[TS_AUTO_NAME_MAX_LEN]; /**< Target variable */
    ts_auto_value_t value;               /**< Value to set */
} ts_auto_action_set_var_t;

/**
 * @brief Device control action parameters
 */
typedef struct {
    char device[TS_AUTO_NAME_MAX_LEN];  /**< Device name (agx0, lpmu0, etc.) */
    char action[TS_AUTO_NAME_MAX_LEN];  /**< Action (power_on/off/reset/etc.) */
} ts_auto_action_device_t;

/**
 * @brief SSH command reference action parameters
 * 
 * References a pre-registered SSH command from ts_ssh_commands_config.
 * The command ID is used to look up the full command configuration at runtime.
 */
typedef struct {
    char cmd_id[TS_AUTO_NAME_MAX_LEN];  /**< SSH command ID (from ts_ssh_commands_config) */
} ts_auto_action_ssh_ref_t;

/**
 * @brief CLI command action parameters
 * 
 * Executes a TianShanOS CLI command locally.
 * Supports all registered console commands like gpio, device, fan, led, etc.
 */
typedef struct {
    char command[TS_AUTO_PATH_MAX_LEN]; /**< CLI command line (e.g., "gpio --set 48 1") */
    char var_name[TS_AUTO_NAME_MAX_LEN]; /**< Variable to store output (optional) */
    uint32_t timeout_ms;                 /**< Execution timeout in ms */
} ts_auto_action_cli_t;

/**
 * @brief Repeat execution mode for rule actions
 */
typedef enum {
    TS_AUTO_REPEAT_ONCE = 0,            /**< Execute once */
    TS_AUTO_REPEAT_WHILE_TRUE,           /**< Repeat while condition is true */
    TS_AUTO_REPEAT_COUNT,                /**< Repeat specified count times */
} ts_auto_repeat_mode_t;

/**
 * @brief Action-level condition (optional, for conditional execution within a rule)
 */
typedef struct {
    bool has_condition;                  /**< Whether this action has a condition */
    char variable[TS_AUTO_NAME_MAX_LEN]; /**< Variable name to check */
    ts_auto_operator_t op;               /**< Comparison operator */
    ts_auto_value_t value;               /**< Comparison value */
} ts_auto_action_condition_t;

/**
 * @brief Action definition
 */
typedef struct {
    ts_auto_action_type_t type;          /**< Action type */
    uint16_t delay_ms;                   /**< Delay before execution */
    bool async;                          /**< Execute asynchronously (non-blocking) */
    
    /* Repeat execution options (for rule actions) */
    ts_auto_repeat_mode_t repeat_mode;   /**< Repeat mode */
    uint8_t repeat_count;                /**< Repeat count (for REPEAT_COUNT mode) */
    uint16_t repeat_interval_ms;         /**< Interval between repeats */
    
    /* Action-level condition (optional) */
    ts_auto_action_condition_t condition; /**< Execute only if condition is met */
    
    /* Template reference (for rule actions) */
    bool runtime_snapshot; /**< Execution-only; never serialized. */
    void *runtime_binding; /**< Owned execution snapshot; never serialized. */
    char template_id[TS_AUTO_NAME_MAX_LEN]; /**< Source template ID (optional, for tracing) */

    union {
        ts_auto_action_led_t led;
        ts_auto_action_ssh_t ssh;
        ts_auto_action_gpio_t gpio;
        ts_auto_action_webhook_t webhook;
        ts_auto_action_log_t log;
        ts_auto_action_set_var_t set_var;
        ts_auto_action_device_t device;
        ts_auto_action_ssh_ref_t ssh_ref; /**< SSH command reference */
        ts_auto_action_cli_t cli;         /**< CLI command */
    };
} ts_auto_action_t;

/**
 * @brief Rule definition
 */
typedef struct {
    char id[TS_AUTO_NAME_MAX_LEN];      /**< Unique rule ID */
    char name[TS_AUTO_LABEL_MAX_LEN];   /**< Display name */
    char icon[64];                       /**< Display icon (emoji or /sdcard/images/xxx.png) */
    bool enabled;                        /**< Is rule enabled */
    bool manual_trigger;                 /**< Exclude from automatic evaluation */
    bool show_on_dashboard;
    bool allow_manual_trigger;
    uint8_t presentation_fields;          /**< bit 0: show present; bit 1: allow present */
    bool reference_unresolved;
    uint32_t revision;
    uint32_t instance;
    void *lease;                         /**< Internal read ownership, never persisted */
    uint32_t cooldown_ms;                /**< Min time between triggers */

    ts_auto_condition_group_t conditions; /**< Trigger conditions */

    ts_auto_action_t *actions;           /**< Array of actions */
    uint8_t action_count;                /**< Number of actions */

    // Runtime state
    int64_t last_trigger_ms;             /**< Last trigger timestamp */
    uint32_t trigger_count;              /**< Total trigger count */
} ts_auto_rule_t;

/*===========================================================================*/
/*                        Dashboard Configuration                             */
/*===========================================================================*/

/**
 * @brief Dashboard card types
 */
typedef enum {
    TS_AUTO_CARD_GAUGE = 0,             /**< Gauge display */
    TS_AUTO_CARD_CHART,                  /**< Time-series chart */
    TS_AUTO_CARD_LED_PREVIEW,            /**< LED matrix preview */
    TS_AUTO_CARD_STATUS,                 /**< Status indicator */
    TS_AUTO_CARD_BUTTON,                 /**< Action button */
    TS_AUTO_CARD_LOG,                    /**< Log viewer */
} ts_auto_card_type_t;

/**
 * @brief Dashboard card definition
 */
typedef struct {
    char id[TS_AUTO_NAME_MAX_LEN];      /**< Card ID */
    char title[TS_AUTO_LABEL_MAX_LEN];  /**< Card title */
    ts_auto_card_type_t type;            /**< Card type */
    char variable[TS_AUTO_NAME_MAX_LEN]; /**< Bound variable */
    uint8_t grid_x, grid_y;              /**< Grid position */
    uint8_t grid_w, grid_h;              /**< Grid size */
    char config_json[TS_AUTO_PATH_MAX_LEN]; /**< Type-specific JSON config */
} ts_auto_card_t;

/**
 * @brief Dashboard layout
 */
typedef struct {
    ts_auto_card_t *cards;               /**< Array of cards */
    uint8_t card_count;                  /**< Number of cards */
    uint8_t grid_cols;                   /**< Grid columns */
    uint8_t refresh_rate_ms;             /**< UI refresh rate */
} ts_auto_dashboard_t;

#ifdef __cplusplus
}
#endif

#endif /* TS_AUTOMATION_TYPES_H */
