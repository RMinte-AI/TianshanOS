#include "ts_rule_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Link only the production codec/filter/cJSON: no live template manager exists. */
static esp_err_t decode(const char *raw, ts_auto_rule_t *rule)
{
    cJSON *json = cJSON_Parse(raw);
    assert(json);
    esp_err_t ret = ts_rule_decode(json, rule);
    cJSON_Delete(json);
    return ret;
}

int main(void)
{
    ts_auto_rule_t rule;
    assert(decode("{\"id\":\"draft\",\"name\":\"Needs action\",\"enabled\":false,\"actions\":[]}", &rule) == ESP_OK);
    assert(!rule.enabled && !rule.action_count && !rule.actions);
    cJSON *saved = ts_rule_encode(&rule);
    ts_auto_rule_t reread;
    assert(saved && ts_rule_decode(saved, &reread) == ESP_OK);
    assert(!reread.enabled && !reread.action_count);
    ts_rule_dispose(&reread);
    ts_rule_dispose(&rule);
    cJSON_Delete(saved);
    assert(decode("{\"id\":\"draft\",\"name\":\"Invalid\",\"enabled\":true,\"actions\":[]}", &rule) == ESP_ERR_INVALID_ARG);
    assert(decode("{\"id\":\"draft\",\"name\":\"Invalid\",\"enabled\":false}", &rule) == ESP_ERR_INVALID_ARG);

    assert(decode("{\"id\":\"ref\",\"name\":\"Reference\",\"actions\":[{\"template_id\":\"same-id\",\"type\":\"ssh\",\"command\":\"cached old command\",\"delay_ms\":123,\"repeat_mode\":\"count\",\"repeat_count\":3,\"repeat_interval_ms\":250,\"condition\":{\"variable\":\"ready\",\"op\":\"eq\",\"value\":true}}]}", &rule) == ESP_OK);
    const ts_auto_action_t *action = &rule.actions[0];
    assert(action->type == TS_AUTO_ACT_TEMPLATE_REF && !strcmp(action->template_id, "same-id"));
    assert(action->delay_ms == 123 && action->repeat_count == 3 && action->repeat_interval_ms == 250);
    assert(action->condition.has_condition && !strcmp(action->condition.variable, "ready"));
    saved = ts_rule_encode(&rule);
    assert(saved && ts_rule_decode(saved, &reread) == ESP_OK);
    assert(reread.actions[0].type == TS_AUTO_ACT_TEMPLATE_REF && reread.actions[0].delay_ms == 123);
    ts_rule_dispose(&reread);
    ts_rule_dispose(&rule);
    cJSON_Delete(saved);
    puts("PASS rule codec: disabled drafts, pure references and per-step fields");
    return 0;
}
