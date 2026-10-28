#include "ts_automation_document.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add(cJSON *document, const char *collection, const char *raw)
{
    cJSON *item = cJSON_Parse(raw);
    assert(item && cJSON_AddItemToArray(cJSON_GetObjectItem(document, collection), item));
}

static cJSON *configuration(int rule_count)
{
    ts_auto_document_t empty = {.enabled=true, .revision=1};
    cJSON *json = ts_auto_document_encode(&empty);
    assert(json);
    add(json,"hosts","{\"id\":\"host\",\"host\":\"192.0.2.9\",\"port\":22,\"username\":\"operator\",\"auth_type\":\"key\",\"keyid\":\"key-ref\",\"enabled\":true}");
    add(json,"commands","{\"id\":\"command\",\"host_id\":\"host\",\"name\":\"Model\",\"command\":\"run-model\",\"serviceMode\":true,\"nohup\":true,\"enabled\":true}");
    add(json,"actions","{\"id\":\"template\",\"name\":\"Start\",\"enabled\":true,\"type\":\"ssh_cmd_ref\",\"ssh_ref\":{\"cmd_id\":\"command\"}}");
    add(json,"variables","{\"name\":\"user.threshold\",\"flags\":1,\"value_type\":3,\"value\":1.0}");
    for(int i=0;i<rule_count;++i) {
        char raw[512];
        snprintf(raw,sizeof(raw),"{\"id\":\"rule-%d\",\"name\":\"Rule %d\",\"enabled\":true,\"conditions\":[{\"variable\":\"not-yet-reported.value\",\"operator\":\"gt\",\"value\":2}],\"actions\":[{\"template_id\":\"template\",\"delay_ms\":25,\"repeat_mode\":\"count\",\"repeat_count\":3}]}",i,i);
        add(json,"rules",raw);
    }
    return json;
}

int main(void)
{
    cJSON *json=configuration(64);
    ts_auto_document_t *document=NULL,*reloaded=NULL;
    ts_auto_config_error_t error;
    assert(ts_auto_document_decode(json,&document,&error)==ESP_OK);
    assert(document->rule_count==64 && ts_auto_document_validate(document,&error)==ESP_OK);
    assert(document->variables[0].default_value.type==TS_AUTO_VAL_FLOAT);
    document->variables[0].value.float_val=999;
    cJSON *saved=ts_auto_document_encode(document);
    assert(saved && ts_auto_document_decode(saved,&reloaded,&error)==ESP_OK);
    char *measured=cJSON_PrintUnformatted(saved);
    assert(measured);printf("64-rule shared-dependency configuration: %zu JSON bytes\n",strlen(measured));free(measured);
    assert(ts_auto_document_validate(reloaded,&error)==ESP_OK);
    assert(reloaded->rules[63].actions[0].delay_ms==25 &&
           !strcmp(reloaded->rules[63].actions[0].template_id,"template"));
    assert(reloaded->variables[0].default_value.type==TS_AUTO_VAL_FLOAT &&
           reloaded->variables[0].default_value.float_val==1.0);
    reloaded->hosts[0].enabled=false;
    assert(ts_auto_document_validate(reloaded,&error)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(error.reason,"dependency_disabled") &&
           !strcmp(error.collection,"commands") && !strcmp(error.dependency,"host"));
    reloaded->hosts[0].enabled=true;
    strcpy(reloaded->actions[0].action.ssh_ref.cmd_id,"missing");
    assert(ts_auto_document_validate(reloaded,&error)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(error.reason,"dependency_missing") && !strcmp(error.id,"template"));
    ts_auto_document_free(reloaded);ts_auto_document_free(document);cJSON_Delete(saved);
    cJSON_ReplaceItemInObject(json,"sources",cJSON_CreateObject());
    assert(ts_auto_document_decode(json,&document,&error)==ESP_ERR_INVALID_ARG && !document);
    assert(!strcmp(error.reason,"configuration_invalid"));
    cJSON_Delete(json);

    json=configuration(65);
    assert(ts_auto_document_decode(json,&document,&error)==ESP_ERR_INVALID_SIZE && !document);
    assert(!strcmp(error.reason,"too_many_rules"));cJSON_Delete(json);

    ts_auto_document_t empty={.enabled=true,.revision=1};
    json=ts_auto_document_encode(&empty);
    add(json,"rules","{\"id\":\"draft\",\"name\":\"Needs action\",\"enabled\":false,\"actions\":[]}");
    assert(ts_auto_document_decode(json,&document,&error)==ESP_OK);
    assert(ts_auto_document_validate(document,&error)==ESP_OK);
    ts_auto_document_free(document);cJSON_Delete(json);

    json=configuration(2);
    cJSON *impact=NULL;
    assert(ts_auto_document_remove(json,TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_NORMAL,
                                   NULL,false,&impact,&error)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(error.reason,"references_exist") &&
           cJSON_GetArraySize(cJSON_GetObjectItem(impact,"rules"))==2 &&
           cJSON_GetArraySize(cJSON_GetObjectItem(impact,"actions"))==1);
    cJSON_Delete(impact);
    add(json,"commands","{\"id\":\"replacement\",\"host_id\":\"host\",\"name\":\"New model\",\"command\":\"new-model\",\"enabled\":true}");
    assert(ts_auto_document_remove(json,TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_REPLACE,
                                   "replacement",false,&impact,&error)==ESP_OK);
    cJSON_Delete(impact);
    assert(ts_auto_document_decode(json,&document,&error)==ESP_OK &&
           ts_auto_document_validate(document,&error)==ESP_OK);
    assert(document->command_count==1 && !strcmp(document->actions[0].action.ssh_ref.cmd_id,"replacement"));
    assert(document->rules[0].actions[0].delay_ms==25 &&
           document->rules[0].actions[0].repeat_count==3);
    ts_auto_document_free(document);cJSON_Delete(json);

    json=configuration(2);
    cJSON *first=cJSON_GetArrayItem(cJSON_GetObjectItem(json,"rules"),0);
    cJSON *direct=cJSON_Parse("{\"type\":\"ssh_cmd_ref\",\"cmd_id\":\"command\",\"delay_ms\":5}");
    assert(direct && cJSON_AddItemToArray(cJSON_GetObjectItem(first,"actions"),direct));
    assert(ts_auto_document_remove(json,TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_FORCE,
                                   NULL,false,&impact,&error)==ESP_OK);
    cJSON_Delete(impact);
    assert(ts_auto_document_decode(json,&document,&error)==ESP_OK &&
           ts_auto_document_validate(document,&error)==ESP_OK);
    assert(!document->command_count && !document->actions[0].enabled);
    assert(!document->rules[0].enabled && !document->rules[0].action_count &&
           !document->rules[1].enabled && !document->rules[1].action_count);
    ts_auto_document_free(document);cJSON_Delete(json);

    json=configuration(1);
    add(json,"rules","{\"id\":\"broken\",\"name\":\"Needs correction\",\"actions\":{\"original-data\":\"keep\"}}");
    assert(ts_auto_document_remove(json,TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_FORCE,
                                   NULL,false,&impact,&error)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(error.reason,"pause_required"));
    cJSON_Delete(impact);
    assert(ts_auto_document_remove(json,TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_FORCE,
                                   NULL,true,&impact,&error)==ESP_OK && !impact);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(json,"enabled")) &&
           !strcmp(cJSON_GetObjectItem(json,"pause_reason")->valuestring,"references_unavailable"));
    assert(!cJSON_GetArraySize(cJSON_GetObjectItem(json,"commands")));
    cJSON *broken=cJSON_GetArrayItem(cJSON_GetObjectItem(json,"rules"),1);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(broken,"actions"),"original-data")->valuestring,"keep"));
    cJSON_Delete(json);
    puts("PASS complete configuration: 64/65 rules, candidate references, editable drafts and initial values");
    return 0;
}
