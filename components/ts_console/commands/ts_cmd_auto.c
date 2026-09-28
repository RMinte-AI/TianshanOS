/**
 * @file ts_cmd_auto.c
 * @brief Automation Rule Console Commands
 * 
 * 实现 auto 命令族：
 * - auto --history [-n count]     显示执行历史
 * - auto --stats                  显示统计信息
 * - auto --list                   列出规则
 * - auto --trigger -r <rule_id>   手动触发规则
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-31
 */

#include "ts_console.h"
#include "ts_rule_engine.h"
#include "argtable3/argtable3.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#define TAG "cmd_auto"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *history;     // --history
    struct arg_lit *stats;       // --stats
    struct arg_lit *list;        // --list
    struct arg_lit *trigger;     // --trigger
    struct arg_lit *clear;       // --clear (clear history)
    struct arg_str *rule_id;     // -r <rule_id>
    struct arg_int *count;       // -n <count>
    struct arg_lit *json;        // --json
    struct arg_lit *help;
    struct arg_end *end;
} s_auto_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 获取状态颜色代码
 */
static const char *status_color(ts_rule_exec_status_t status)
{
    switch (status) {
        case TS_RULE_EXEC_SUCCESS: return "\033[32m";  // 绿色
        case TS_RULE_EXEC_PARTIAL: return "\033[33m";  // 黄色
        case TS_RULE_EXEC_FAILED:  return "\033[31m";  // 红色
        case TS_RULE_EXEC_SKIPPED: return "\033[36m";  // 青色
        default:                   return "\033[0m";
    }
}

/**
 * @brief 触发源转字符串
 */
static const char *trigger_source_str(ts_rule_trigger_source_t src)
{
    switch (src) {
        case TS_RULE_TRIGGER_CONDITION: return "COND";
        case TS_RULE_TRIGGER_MANUAL:    return "MANUAL";
        case TS_RULE_TRIGGER_TIMER:     return "TIMER";
        case TS_RULE_TRIGGER_STARTUP:   return "STARTUP";
        default:                        return "?";
    }
}

/**
 * @brief 格式化时间戳为相对时间
 */
static void format_relative_time(int64_t timestamp_ms, char *buf, size_t len)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t diff_ms = now_ms - timestamp_ms;
    
    if (diff_ms < 1000) {
        snprintf(buf, len, "just now");
    } else if (diff_ms < 60000) {
        snprintf(buf, len, "%" PRId64 "s ago", diff_ms / 1000);
    } else if (diff_ms < 3600000) {
        snprintf(buf, len, "%" PRId64 "m ago", diff_ms / 60000);
    } else {
        snprintf(buf, len, "%" PRId64 "h ago", diff_ms / 3600000);
    }
}

/*===========================================================================*/
/*                          Command: auto --history                           */
/*===========================================================================*/

static int cmd_auto_history(int count, bool json_output)
{
    if (count <= 0) count = 10;  // 默认显示 10 条
    if (count > TS_RULE_EXEC_HISTORY_SIZE) count = TS_RULE_EXEC_HISTORY_SIZE;
    
    // 在栈上分配少量记录，大量使用 PSRAM
    ts_rule_exec_record_t *records;
    if (count <= 4) {
        // 小量使用栈
        records = alloca(count * sizeof(ts_rule_exec_record_t));
    } else {
        records = heap_caps_malloc(count * sizeof(ts_rule_exec_record_t), MALLOC_CAP_SPIRAM);
        if (!records) {
            records = malloc(count * sizeof(ts_rule_exec_record_t));
        }
        if (!records) {
            printf("Error: memory allocation failed\n");
            return 1;
        }
    }
    
    int actual = 0;
    esp_err_t ret = ts_rule_get_exec_history(records, count, &actual);
    
    if (ret != ESP_OK) {
        printf("Error: failed to get history (%s)\n", esp_err_to_name(ret));
        if (count > 4) free(records);
        return 1;
    }
    
    if (json_output) {
        printf("{\"history\":[\n");
        for (int i = 0; i < actual; i++) {
            printf("  {\"rule\":\"%s\",\"status\":\"%s\",\"source\":\"%s\","
                   "\"actions\":%d,\"failed\":%d,\"msg\":\"%s\",\"ts\":%" PRId64 "}%s\n",
                   records[i].rule_id,
                   ts_rule_exec_status_str(records[i].status),
                   trigger_source_str(records[i].source),
                   records[i].action_count,
                   records[i].failed_count,
                   records[i].message,
                   records[i].timestamp_ms,
                   (i < actual - 1) ? "," : "");
        }
        printf("],\"count\":%d}\n", actual);
    } else {
        printf("\n\033[1m%-20s %-8s %-6s %-10s %s\033[0m\n",
               "RULE", "STATUS", "SRC", "WHEN", "MESSAGE");
        printf("─────────────────────────────────────────────────────────────\n");
        
        for (int i = 0; i < actual; i++) {
            char when[16];
            format_relative_time(records[i].timestamp_ms, when, sizeof(when));
            
            printf("%-20s %s%-8s\033[0m %-6s %-10s %s\n",
                   records[i].rule_id,
                   status_color(records[i].status),
                   ts_rule_exec_status_str(records[i].status),
                   trigger_source_str(records[i].source),
                   when,
                   records[i].message);
        }
        
        if (actual == 0) {
            printf("  (no execution history)\n");
        }
        printf("\n");
    }
    
    if (count > 4) free(records);
    return 0;
}

/*===========================================================================*/
/*                          Command: auto --stats                             */
/*===========================================================================*/

static int cmd_auto_stats(bool json_output)
{
    ts_rule_engine_stats_t stats;
    esp_err_t ret = ts_rule_engine_get_stats(&stats);
    
    if (ret != ESP_OK) {
        printf("Error: failed to get stats (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    
    int rule_count = ts_rule_count();
    
    if (json_output) {
        printf("{\"rules\":%d,\"evaluations\":%" PRIu32 ",\"triggers\":%" PRIu32 ","
               "\"actions\":%" PRIu32 ",\"failed\":%" PRIu32 ",\"last_eval_ms\":%" PRId64 "}\n",
               rule_count, stats.total_evaluations, stats.total_triggers,
               stats.total_actions, stats.failed_actions, stats.last_evaluation_ms);
    } else {
        printf("\n\033[1mAutomation Engine Statistics\033[0m\n");
        printf("─────────────────────────────────\n");
        printf("  Active Rules:     %d\n", rule_count);
        printf("  Total Evals:      %" PRIu32 "\n", stats.total_evaluations);
        printf("  Total Triggers:   %" PRIu32 "\n", stats.total_triggers);
        printf("  Actions Exec:     %" PRIu32 "\n", stats.total_actions);
        printf("  Actions Failed:   %" PRIu32 "\n", stats.failed_actions);
        
        if (stats.total_actions > 0) {
            float success_rate = 100.0f * (float)(stats.total_actions - stats.failed_actions) 
                                 / (float)stats.total_actions;
            printf("  Success Rate:     %.1f%%\n", success_rate);
        }
        printf("\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: auto --list                              */
/*===========================================================================*/

static int cmd_auto_list(bool json_output)
{
    int count = ts_rule_count();
    
    if (json_output) {
        printf("{\"rules\":[\n");
    } else {
        printf("\n\033[1m%-20s %-8s %-10s %-10s %s\033[0m\n",
               "ID", "ENABLED", "COOLDOWN", "TRIGGERS", "CONDITIONS");
        printf("─────────────────────────────────────────────────────────────\n");
    }
    
    for (int i = 0; i < count; i++) {
        ts_auto_rule_t rule;
        if (ts_rule_get_by_index(i, &rule) == ESP_OK) {
            if (json_output) {
                printf("  {\"id\":\"%s\",\"enabled\":%s,\"cooldown_ms\":%" PRIu32 ","
                       "\"triggers\":%" PRIu32 ",\"actions\":%d}%s\n",
                       rule.id,
                       rule.enabled ? "true" : "false",
                       rule.cooldown_ms,
                       rule.trigger_count,
                       rule.action_count,
                       (i < count - 1) ? "," : "");
            } else {
                printf("%-20s %-8s %-10" PRIu32 " %-10" PRIu32 " %d cond, %d act\n",
                       rule.id,
                       rule.enabled ? "\033[32mYES\033[0m" : "\033[31mNO\033[0m",
                       rule.cooldown_ms,
                       rule.trigger_count,
                       rule.conditions.count,
                       rule.action_count);
            }
            ts_rule_release(&rule);
        }
    }
    
    if (json_output) {
        printf("],\"count\":%d}\n", count);
    } else {
        if (count == 0) {
            printf("  (no rules registered)\n");
        }
        printf("\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: auto --trigger                           */
/*===========================================================================*/

static int cmd_auto_trigger(const char *rule_id)
{
    if (!rule_id || !rule_id[0]) {
        printf("Error: rule ID required (-r <rule_id>)\n");
        return 1;
    }
    
    esp_err_t ret = ts_rule_trigger(rule_id);
    if (ret == ESP_OK) {
        printf("Rule '%s' triggered successfully\n", rule_id);
        return 0;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        printf("Error: rule '%s' not found\n", rule_id);
        return 1;
    } else {
        printf("Error: failed to trigger (%s)\n", esp_err_to_name(ret));
        return 1;
    }
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_auto_handler(int argc, char **argv)
{
    // 解析参数
    int nerrors = arg_parse(argc, argv, (void **)&s_auto_args);
    
    if (s_auto_args.help->count > 0) {
        printf("Usage: auto [options]\n");
        printf("Automation rule management and monitoring\n\n");
        printf("Options:\n");
        printf("  --history     Show recent execution history\n");
        printf("  --stats       Show engine statistics\n");
        printf("  --list        List all registered rules\n");
        printf("  --trigger     Trigger a rule manually\n");
        printf("  --clear       Clear execution history\n");
        printf("  -r <id>       Rule ID (for trigger)\n");
        printf("  -n <count>    Number of records (for history, default 10)\n");
        printf("  --json        JSON output format\n");
        printf("\nExamples:\n");
        printf("  auto --history             # Show last 10 executions\n");
        printf("  auto --history -n 5        # Show last 5 executions\n");
        printf("  auto --stats               # Show statistics\n");
        printf("  auto --list                # List all rules\n");
        printf("  auto --trigger -r my_rule  # Trigger rule 'my_rule'\n");
        return 0;
    }
    
    if (nerrors > 0) {
        arg_print_errors(stderr, s_auto_args.end, "auto");
        return 1;
    }
    
    bool json = s_auto_args.json->count > 0;
    
    // 处理各种操作
    if (s_auto_args.history->count > 0) {
        int count = (s_auto_args.count->count > 0) ? s_auto_args.count->ival[0] : 10;
        return cmd_auto_history(count, json);
    }
    
    if (s_auto_args.stats->count > 0) {
        return cmd_auto_stats(json);
    }
    
    if (s_auto_args.list->count > 0) {
        return cmd_auto_list(json);
    }
    
    if (s_auto_args.trigger->count > 0) {
        const char *rule_id = (s_auto_args.rule_id->count > 0) 
                              ? s_auto_args.rule_id->sval[0] : NULL;
        return cmd_auto_trigger(rule_id);
    }
    
    if (s_auto_args.clear->count > 0) {
        esp_err_t ret = ts_rule_clear_exec_history();
        if (ret == ESP_OK) {
            printf("Execution history cleared\n");
            return 0;
        } else {
            printf("Error: %s\n", esp_err_to_name(ret));
            return 1;
        }
    }
    
    // 默认显示统计 + 最近 5 条历史
    printf("\n");
    cmd_auto_stats(false);
    printf("\033[1mRecent Executions\033[0m\n");
    printf("─────────────────────────────────\n");
    cmd_auto_history(5, false);
    
    return 0;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

void ts_cmd_auto_register(void)
{
    // 初始化参数表
    s_auto_args.history  = arg_lit0(NULL, "history",  "Show execution history");
    s_auto_args.stats    = arg_lit0(NULL, "stats",    "Show statistics");
    s_auto_args.list     = arg_lit0(NULL, "list",     "List all rules");
    s_auto_args.trigger  = arg_lit0(NULL, "trigger",  "Trigger a rule manually");
    s_auto_args.clear    = arg_lit0(NULL, "clear",    "Clear execution history");
    s_auto_args.rule_id  = arg_str0("r",  "rule",     "<id>", "Rule ID");
    s_auto_args.count    = arg_int0("n",  NULL,       "<num>", "Count (default 10)");
    s_auto_args.json     = arg_lit0(NULL, "json",     "JSON output");
    s_auto_args.help     = arg_lit0("h",  "help",     "Show help");
    s_auto_args.end      = arg_end(2);
    
    // 注册命令
    const esp_console_cmd_t cmd = {
        .command = "auto",
        .help = "Automation rule management (history/stats/list/trigger)",
        .hint = NULL,
        .func = cmd_auto_handler,
        .argtable = &s_auto_args,
    };
    
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    ESP_LOGI(TAG, "Command 'auto' registered");
}
