/* Real registered object adapters and the shared API implementation, connected
 * to the production owner and real temporary configuration files. */
static void free_api(ts_api_result_t *result)
{ free(result->message);cJSON_Delete(result->data);*result=(ts_api_result_t){0}; }
static cJSON *api_params(const char *json)
{
    cJSON *params=cJSON_Parse(json);assert(params);
    assert(cJSON_AddStringToObject(params,"active_version",status().active_version));return params;
}
static void import_preview_params(cJSON *params,const ts_api_result_t *reply)
{
    const cJSON *digest=cJSON_GetObjectItemCaseSensitive(reply->data,"package_digest"),*credential=cJSON_GetObjectItemCaseSensitive(reply->data,"credential_generation");
    assert(cJSON_IsString(digest)&&cJSON_IsNumber(credential));
    cJSON_DeleteItemFromObjectCaseSensitive(params,"expected_digest");cJSON_DeleteItemFromObjectCaseSensitive(params,"expected_credential");
    assert(cJSON_AddStringToObject(params,"expected_digest",digest->valuestring)&&cJSON_AddNumberToObject(params,"expected_credential",credential->valuedouble));
    cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(params,"preview"),false);
}
static cJSON *import_params(const char *content)
{
    cJSON *params=api_params("{\"preview\":true}");assert(cJSON_AddStringToObject(params,"tscfg",content));return params;
}
static void config_pack_api_cases(void)
{
    lifecycle_seed();ts_api_result_t reply={0};char before[33];strcpy(before,status().active_version);
    cJSON *params=import_params("{\"type\":\"ssh_command\",\"command\":{\"id\":\"new_cmd\",\"name\":\"New command\",\"host_id\":\"pkg_host\",\"command\":\"true\",\"nohup\":true,\"serviceMode\":true,\"readyInterval\":6500},\"host\":{\"id\":\"pkg_host\",\"host\":\"192.0.2.88\",\"username\":\"operator\",\"keyid\":\"reference\"}}");
    assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code==TS_API_OK);
    assert(!strcmp(before,status().active_version)&&cJSON_GetArraySize(cJSON_GetObjectItem(reply.data,"items"))==2);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply.data,"id")),"new_cmd"));
    import_preview_params(params,&reply);free_api(&reply);
    ts_ssh_host_config_t host;ts_ssh_command_config_t command;
    assert(ts_ssh_hosts_config_get("pkg_host",&host)==ESP_ERR_NOT_FOUND&&ts_ssh_commands_config_get("new_cmd",&command)==ESP_ERR_NOT_FOUND);
    cJSON *digest=cJSON_GetObjectItemCaseSensitive(params,"expected_digest");char original=digest->valuestring[0];digest->valuestring[0]=original=='0'?'1':'0';
    assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code!=TS_API_OK&&!strcmp(reply.message,"credential_changed"));
    assert(!strcmp(before,status().active_version));free_api(&reply);digest->valuestring[0]=original;
    fail_install=true;assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code!=TS_API_OK);fail_install=false;
    assert(cJSON_IsFalse(cJSON_GetObjectItem(reply.data,"saved"))&&!strcmp(before,status().active_version));free_api(&reply);
    assert(ts_ssh_hosts_config_get("pkg_host",&host)==ESP_ERR_NOT_FOUND&&ts_ssh_commands_config_get("new_cmd",&command)==ESP_ERR_NOT_FOUND);
    assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(reply.data,"imported")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(reply.data,"saved"))&&cJSON_IsFalse(cJSON_GetObjectItem(reply.data,"restart_required")));
    assert(ts_ssh_hosts_config_get("pkg_host",&host)==ESP_OK&&ts_ssh_commands_config_get("new_cmd",&command)==ESP_OK&&command.ready_check_interval_ms==6500);
    free_api(&reply);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(params,"active_version",cJSON_CreateString(status().active_version)));
    cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(params,"preview"),true);
    assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(reply.data,"unchanged")));
    import_preview_params(params,&reply);free_api(&reply);strcpy(before,status().active_version);
    assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code==TS_API_OK&&cJSON_IsFalse(cJSON_GetObjectItem(reply.data,"saved"))&&!strcmp(before,status().active_version));
    free_api(&reply);
    reject_package=true;assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code!=TS_API_OK&&!strcmp(before,status().active_version));reject_package=false;
    free_api(&reply);cJSON_Delete(params);
    params=import_params("{\"type\":\"ssh_command\",\"command\":{\"id\":\"missing_host\",\"host_id\":\"not_present\",\"name\":\"Missing host\",\"command\":\"true\"}}");
    assert(api_ssh_commands_import(params,&reply)==ESP_OK&&reply.code!=TS_API_OK&&!strcmp(reply.message,"dependency_missing"));
    assert(!strcmp(before,status().active_version));free_api(&reply);cJSON_Delete(params);
    const char *contents[]={
        "{\"type\":\"action_template\",\"template\":{\"id\":\"imported_action\",\"name\":\"Imported\",\"type\":\"log\",\"log\":{\"message\":\"Imported action\"}}}",
        "{\"type\":\"automation_source\",\"source\":{\"id\":\"imported_source\",\"label\":\"Imported source\",\"type\":\"rest\",\"url\":\"http://example.invalid/values\"}}",
        "{\"id\":\"imported_host\",\"host\":\"192.0.2.99\",\"username\":\"operator\"}"};
    esp_err_t (*handlers[])(const cJSON*,ts_api_result_t*)={api_automation_actions_import,api_automation_sources_import,api_ssh_hosts_import};
    for(unsigned i=0;i<3;++i){
        params=import_params(contents[i]);assert(handlers[i](params,&reply)==ESP_OK&&reply.code==TS_API_OK);
        import_preview_params(params,&reply);free_api(&reply);
        assert(handlers[i](params,&reply)==ESP_OK&&reply.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(reply.data,"saved")));
        free_api(&reply);cJSON_Delete(params);
    }
    ts_action_template_t action;ts_auto_source_t source;
    assert(ts_action_template_get("imported_action",&action)==ESP_OK&&ts_source_get_copy("imported_source",&source)==ESP_OK&&ts_ssh_hosts_config_get("imported_host",&host)==ESP_OK);
    lifecycle_seed();puts("PASS config pack APIs: verified content IDs, whole-candidate preview, atomic host/command commit, failed save, unchanged import, signature rejection, dependency errors and immediate four-kind application");
}
static void *versioned_trigger_thread(void *params)
{
    ts_api_result_t result={0};assert(api_automation_rules_trigger(params,&result)==ESP_OK&&result.code==TS_API_OK);
    free_api(&result);return NULL;
}
static void filter_api_roundtrip(void)
{
    cJSON *params=api_params("{\"id\":\"filter_precision\",\"name\":\"Filter\",\"type\":\"led\",\"async\":true,\"enabled\":false,\"led\":{\"device\":\"led_matrix\",\"ctrl_type\":\"filter\",\"filter\":\"wave\",\"filter_params\":{\"speed\":40,\"angle\":0,\"amplitude\":0,\"wavelength\":8}}}");
    ts_api_result_t result={0};
    assert(api_automation_actions_add(params,&result)==ESP_OK&&result.code==TS_API_OK);
    free_api(&result);cJSON_Delete(params);
    params=api_params("{\"id\":\"filter_precision\"}");
    assert(api_automation_actions_get(params,&result)==ESP_OK&&result.code==TS_API_OK);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result.data,"async"))&&cJSON_IsFalse(cJSON_GetObjectItem(result.data,"enabled")));
    const cJSON *filter=cJSON_GetObjectItem(cJSON_GetObjectItem(result.data,"led"),"filter_params");
    assert(cJSON_IsNumber(cJSON_GetObjectItem(filter,"angle"))&&cJSON_GetObjectItem(filter,"angle")->valuedouble==0);
    assert(cJSON_IsNumber(cJSON_GetObjectItem(filter,"amplitude"))&&cJSON_GetObjectItem(filter,"amplitude")->valuedouble==0);
    cJSON *roundtrip=cJSON_Duplicate(result.data,true),*bad=cJSON_Duplicate(result.data,true);assert(roundtrip&&bad);
    free_api(&result);cJSON_Delete(params);
    assert(cJSON_ReplaceItemInObject(cJSON_GetObjectItem(cJSON_GetObjectItem(bad,"led"),"filter_params"),"speed",cJSON_CreateNumber(0)));
    char before[33];strcpy(before,status().active_version);
    assert(api_automation_actions_update(bad,&result)==ESP_OK&&result.code==TS_API_ERR_INVALID_ARG&&!strcmp(before,status().active_version));
    free_api(&result);cJSON_Delete(bad);
    assert(api_automation_actions_update(roundtrip,&result)==ESP_OK&&result.code==TS_API_OK);
    free_api(&result);cJSON_Delete(roundtrip);
}
static void config_api_cases(void)
{
    lifecycle_seed();ts_api_result_t result={0};
    /* A long admitted action does not retain the configuration writer. */
    cJSON *trigger=api_params("{\"id\":\"manual\"}");pthread_t running;
    block_action=true;action_reached=action_released=false;
    assert(!pthread_create(&running,NULL,versioned_trigger_thread,trigger));
    pthread_mutex_lock(&execution_gate);while(!action_reached)pthread_cond_wait(&execution_changed,&execution_gate);pthread_mutex_unlock(&execution_gate);
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"edit_during_manual\",\"value_type\":2,\"value\":1}",NULL,NULL)==ESP_OK);
    pthread_mutex_lock(&execution_gate);action_released=true;pthread_cond_broadcast(&execution_changed);pthread_mutex_unlock(&execution_gate);
    assert(!pthread_join(running,NULL));block_action=false;cJSON_Delete(trigger);
    assert(api_automation_actions_list(NULL,&result)==ESP_OK&&result.code==TS_API_OK);
    assert(!strcmp(cJSON_GetObjectItem(result.data,"active_version")->valuestring,status().active_version));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(result.data,"templates"))==1);free_api(&result);
    /* Template tests have the same displayed-version boundary as manual rules. */
    unsigned before_templates=template_executions;
    cJSON *template_params=cJSON_Parse("{\"id\":\"action\"}");
    assert(api_automation_actions_execute(template_params,&result)==ESP_OK&&!strcmp(result.message,"configuration_version_required"));
    free_api(&result);
    assert(cJSON_AddStringToObject(template_params,"active_version","old-version"));
    assert(api_automation_actions_execute(template_params,&result)==ESP_OK&&!strcmp(result.message,"configuration_changed"));
    free_api(&result);
    assert(template_executions==before_templates);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(template_params,"active_version",cJSON_CreateString(status().active_version)));
    assert(api_automation_actions_execute(template_params,&result)==ESP_OK&&result.code==TS_API_OK);
    assert(template_executions==before_templates+1);
    free_api(&result);
    template_execution_ret=ESP_ERR_TIMEOUT;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_TIMEOUT};
    strcpy(template_outcome.output,"executor deadline");
    assert(api_automation_actions_execute(template_params,&result)==ESP_OK&&!strcmp(result.message,"action_timeout"));
    assert(!strcmp(cJSON_GetObjectItem(result.data,"detail")->valuestring,"executor deadline"));free_api(&result);
    template_execution_ret=ESP_ERR_NOT_FOUND;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_FAILED,.reason="action_image_unavailable"};
    strcpy(template_outcome.output,"/sdcard/images/missing.png");
    assert(api_automation_actions_execute(template_params,&result)==ESP_OK&&!strcmp(result.message,"action_image_unavailable"));
    assert(!strcmp(cJSON_GetObjectItem(result.data,"detail")->valuestring,"/sdcard/images/missing.png"));free_api(&result);
    template_execution_ret=ESP_OK;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_SUCCESS};
    cJSON_Delete(template_params);
    filter_api_roundtrip();
    cJSON *params=api_params("{\"id\":\"cmd\",\"host_id\":\"host\",\"name\":\"Edited\",\"command\":\"run\",\"serviceMode\":true,\"nohup\":true,\"readyPattern\":\"ready\",\"readyInterval\":7000,\"varName\":\"reply\"}");
    char stale[33];strcpy(stale,status().active_version);
    assert(api_ssh_commands_add(params,&result)==ESP_OK&&result.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(result.data,"saved")));
    free_api(&result);
    assert(api_ssh_commands_add(params,&result)==ESP_OK&&result.code!=TS_API_OK&&!strcmp(result.message,"configuration_changed"));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result.data,"saved")));free_api(&result);cJSON_Delete(params);
    params=api_params("{\"id\":\"cmd\"}");
    assert(api_ssh_commands_get(params,&result)==ESP_OK&&result.code==TS_API_OK);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result.data,"serviceMode"))&&cJSON_GetObjectItem(result.data,"readyInterval")->valueint==7000);
    assert(!strcmp(cJSON_GetObjectItem(result.data,"varName")->valuestring,"reply"));free_api(&result);cJSON_Delete(params);

    /* Manual starts stay bound to the version displayed by the client. */
    params=api_params("{\"id\":\"missing_rule_for_version_check\"}");
    assert(api_automation_rules_trigger(params,&result)==ESP_OK&&result.code==TS_API_ERR_NOT_FOUND);free_api(&result);
    cJSON_DeleteItemFromObjectCaseSensitive(params,"active_version");
    assert(api_automation_rules_trigger(params,&result)==ESP_OK&&!strcmp(result.message,"configuration_version_required"));free_api(&result);
    cJSON_AddStringToObject(params,"active_version",stale);
    cJSON_ReplaceItemInObjectCaseSensitive(params,"id",cJSON_CreateString("manual"));
    assert(api_automation_rules_trigger(params,&result)==ESP_OK&&!strcmp(result.message,"configuration_changed"));free_api(&result);cJSON_Delete(params);

    /* Editing does not pre-register unrelated variables or save a legacy file. */
    params=api_params("{\"id\":\"new_source\",\"label\":\"Temperature\",\"type\":\"rest\",\"url\":\"http://example.invalid/api\",\"json_path\":\"temp\"}");
    assert(api_automation_sources_add(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    ts_auto_source_t source;assert(ts_source_get_copy("new_source",&source)==ESP_OK&&source.enabled&&source.auto_discover);
    assert(source.poll_interval_ms==1000&&!strcmp(source.rest.url,"http://example.invalid/api")&&!strcmp(source.rest.method,"GET"));
    assert(source.mapping_count==1&&!strcmp(source.mappings[0].var_name,"new_source.temp"));
    params=api_params("{\"id\":\"new_source\",\"json_path\":\"voltage\",\"var_name\":\"new_source.voltage\"}");
    assert(api_automation_sources_add_mapping(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    assert(ts_source_get_copy("new_source",&source)==ESP_OK&&source.mapping_count==2);
    params=api_params("{\"id\":\"new_source\",\"index\":1}");
    assert(api_automation_sources_remove_mapping(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    assert(ts_source_get_copy("new_source",&source)==ESP_OK&&source.mapping_count==1);
    /* A real transport update must be visible through the saved-view API. */
    xSemaphoreTake(ts_source_runtime.mutex,portMAX_DELAY);
    for(int i=0;i<ts_source_runtime.count;++i)if(!strcmp(ts_source_runtime.sources[i].id,"new_source")){
        ts_source_runtime.sources[i].connected=true;ts_source_runtime.sources[i].last_update_ms=321;
    }
    xSemaphoreGive(ts_source_runtime.mutex);
    assert(api_automation_sources_list(NULL,&result)==ESP_OK&&result.code==TS_API_OK);
    const cJSON *shown=NULL,*entry;
    cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(result.data,"sources"))
        if(!strcmp(cJSON_GetObjectItemCaseSensitive(entry,"id")->valuestring,"new_source")){shown=entry;break;}
    assert(shown&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(shown,"connected")));
    assert(cJSON_GetObjectItemCaseSensitive(shown,"last_update_ms")->valueint==321);free_api(&result);

    params=api_params("{\"name\":\"user_limit\",\"value\":7}");
    assert(api_automation_variables_set(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    const ts_auto_config_view_t *view=ts_auto_config_acquire();
    const cJSON *variables=cJSON_GetObjectItemCaseSensitive(ts_auto_config_json(view),"variables"),*row;bool found=false;
    cJSON_ArrayForEach(row,variables){
        if(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,"name")),"user_limit"))continue;
        found=true;assert(cJSON_GetObjectItemCaseSensitive(row,"value")->valueint==7);
    }
    assert(found);ts_auto_config_release(view);
    params=api_params("{\"name\":\"user_limit\",\"value\":99,\"create_only\":true}");
    strcpy(stale,status().active_version);assert(api_automation_variables_set(params,&result)==ESP_OK&&result.code==TS_API_OK);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result.data,"created"))&&cJSON_IsFalse(cJSON_GetObjectItem(result.data,"saved"))&&!strcmp(stale,status().active_version));
    free_api(&result);cJSON_Delete(params);

    params=api_params("{\"id\":\"action\",\"preview\":true}");strcpy(stale,status().active_version);
    assert(api_automation_actions_delete(params,&result)==ESP_OK&&result.code==TS_API_OK);
    const cJSON *impact=cJSON_GetObjectItem(result.data,"impact");assert(cJSON_GetArraySize(cJSON_GetObjectItem(impact,"rules"))==1);
    assert(!strcmp(stale,status().active_version)&&cJSON_IsFalse(cJSON_GetObjectItem(result.data,"saved")));
    free_api(&result);cJSON_Delete(params);
    params=api_params("{\"id\":\"action\"}");
    assert(api_automation_actions_delete(params,&result)==ESP_OK&&result.code!=TS_API_OK&&!strcmp(result.message,"references_exist"));free_api(&result);cJSON_Delete(params);
    /* Replace referenced actions in one commit, then hard-delete a rule. */
    params=api_params("{\"id\":\"replacement\",\"name\":\"Replacement\",\"type\":\"log\",\"log\":{\"message\":\"replacement\"}}");
    assert(api_automation_actions_add(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    params=api_params("{\"id\":\"action\",\"replacement_id\":\"replacement\"}");
    assert(api_automation_actions_delete(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    ts_auto_rule_t rule;assert(ts_rule_acquire("auto",&rule)==ESP_OK&&!strcmp(rule.actions[0].template_id,"replacement"));ts_rule_release(&rule);
    params=api_params("{\"id\":\"manual\"}");assert(api_automation_rules_delete(params,&result)==ESP_OK&&result.code==TS_API_OK);free_api(&result);cJSON_Delete(params);
    assert(ts_rule_acquire("manual",&rule)==ESP_ERR_NOT_FOUND);

    /* Forced deletion bypasses remote service admission and keeps disabled drafts. */
    lifecycle_seed();refuse_service=true;params=api_params("{\"id\":\"cmd\",\"force\":true}");
    assert(api_ssh_commands_remove(params,&result)==ESP_OK&&result.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(result.data,"saved")));
    free_api(&result);cJSON_Delete(params);refuse_service=false;
    assert(ts_rule_acquire("auto",&rule)==ESP_OK&&!rule.enabled&&!rule.action_count);ts_rule_release(&rule);
    /* A malformed runtime collection remains visible for explicit force repair. */
    lifecycle_seed();view=ts_auto_config_acquire();cJSON *actions=cJSON_GetObjectItemCaseSensitive((cJSON*)ts_auto_config_json(view),"actions");
    cJSON_AddItemToArray(actions,cJSON_CreateString("unreadable reference"));ts_auto_config_release(view);
    params=api_params("{\"id\":\"cmd\",\"force\":true}");
    assert(api_ssh_commands_remove(params,&result)==ESP_OK&&result.code!=TS_API_OK&&!strcmp(result.message,"pause_required"));
    free_api(&result);cJSON_Delete(params);
    params=api_params("{\"id\":\"cmd\",\"force\":true,\"pause_automation\":true}");fail_prepare=true;
    assert(api_ssh_commands_remove(params,&result)==ESP_OK&&result.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(result.data,"paused")));
    fail_prepare=false;free_api(&result);cJSON_Delete(params);
    assert(api_automation_actions_list(NULL,&result)==ESP_OK&&result.code==TS_API_OK&&cJSON_GetArraySize(cJSON_GetObjectItem(result.data,"templates"))>=1);
    free_api(&result);
    lifecycle_seed();
    puts("PASS configuration APIs: saved-view lists, aliases/defaults, versioned writes, persisted initial values, source mappings, preview/replace/hard delete, independent force and paused malformed collection");
}
