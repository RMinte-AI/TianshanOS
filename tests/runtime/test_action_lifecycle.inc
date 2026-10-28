static void lifecycle_seed(void)
{
    cJSON *json=mode_seed();assert(ts_auto_config_install(TS_AUTO_CONFIG_SD,json,NULL)==ESP_OK);cJSON_Delete(json);
    assert(ts_auto_config_enable(true,NULL,NULL)==ESP_OK);
}
static ts_auto_action_t frozen_rule(void)
{
    ts_auto_action_t action={0};ts_auto_rule_t rule;
    ts_ssh_binding_lock();assert(ts_rule_acquire("auto",&rule)==ESP_OK);
    assert(ts_action_snapshot(&rule.actions[0],&action)==ESP_OK);
    ts_action_snapshot_owner(&action,rule.id,rule.instance);ts_rule_release(&rule);ts_ssh_binding_unlock();
    return action;
}
static void cancelled_entry(ts_auto_action_t *action)
{
    ts_action_queue_entry_t entry={.action=*action};ts_action_result_t result={0};unsigned before=execution_io;
    assert(execute_entry(&entry,&result)==ESP_ERR_INVALID_STATE&&result.status==TS_ACTION_STATUS_CANCELLED&&execution_io==before);
    ts_action_snapshot_release(action);
}
static void delete_waiting_rule(void){assert(ts_rule_unregister("auto")==ESP_OK);}
static void replace_reply(void)
{
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_VARIABLE,"reply.status",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"reply.status\",\"source_id\":\"user\",\"value_type\":4,\"value\":\"new\"}",NULL,NULL)==ESP_OK);
}
static void action_lifecycle_cases(void)
{
    lifecycle_seed();ts_auto_action_t action=frozen_rule();uint64_t epoch=action.runtime_epoch;
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"unrelated\",\"source_id\":\"user\",\"value_type\":2,\"value\":5}",NULL,NULL)==ESP_OK);
    assert(ts_action_snapshot_current(&action)&&epoch==ts_auto_config_execution_epoch());
    ts_action_queue_entry_t entry={.action=action};ts_action_result_t result={0};
    ts_action_snapshot_retain(&entry.action);ts_action_snapshot_release(&action); /* Queue owns a separate lease. */
    unsigned before=execution_io;assert(execute_entry(&entry,&result)==ESP_OK&&execution_io==before+1);ts_action_snapshot_release(&entry.action);
    action=frozen_rule();assert(ts_rule_unregister("auto")==ESP_OK);lifecycle_seed();cancelled_entry(&action);
    action=frozen_rule();ts_action_template_t template;assert(ts_action_template_get("action",&template)==ESP_OK);
    strcpy(template.name,"New template");assert(ts_action_template_update("action",&template)==ESP_OK);cancelled_entry(&action);
    lifecycle_seed();action=frozen_rule();ts_ssh_command_config_t command;assert(ts_ssh_commands_config_get("cmd",&command)==ESP_OK);
    strcpy(command.command,"changed");assert(ts_ssh_commands_config_add(&command,NULL,0)==ESP_OK);cancelled_entry(&action);
    lifecycle_seed();assert(ts_ssh_commands_config_get("cmd",&command)==ESP_OK);strcpy(command.var_name,"job");
    assert(ts_ssh_commands_config_add(&command,NULL,0)==ESP_OK&&ts_variable_set_string("job.status","ready")==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"other\",\"value_type\":2,\"value\":1}",NULL,NULL)==ESP_OK);
    ts_auto_value_t reply;assert(ts_variable_get("job.status",&reply)==ESP_OK&&!strcmp(reply.str_val,"ready"));
    action=frozen_rule();
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.77\",\"username\":\"operator\"}",NULL,NULL)==ESP_OK);
    assert(ts_variable_get("job.status",&reply)==ESP_OK&&reply.type==TS_AUTO_VAL_STRING&&!reply.str_val[0]);cancelled_entry(&action);
    /* Even an identical replacement host is a new object, not the queued host. */
    ts_auto_document_t empty={.enabled=true};cJSON *json=ts_auto_document_encode(&empty);assert(json);
    assert(cJSON_AddItemToArray(cJSON_GetObjectItem(json,"hosts"),cJSON_Parse("{\"id\":\"host\",\"host\":\"192.0.2.1\",\"username\":\"operator\"}")));
    assert(ts_auto_config_install(TS_AUTO_CONFIG_SD,json,NULL)==ESP_OK);cJSON_Delete(json);
    ts_auto_action_t direct={.type=TS_AUTO_ACT_SSH_CMD};strcpy(direct.ssh.host_ref,"host");strcpy(direct.ssh.command,"true");
    ts_ssh_binding_lock();assert(ts_action_snapshot(&direct,&action)==ESP_OK);ts_ssh_binding_unlock();epoch=action.runtime_epoch;
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_HOST,"host",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.1\",\"username\":\"operator\"}",NULL,NULL)==ESP_OK);
    assert(epoch==ts_auto_config_execution_epoch());cancelled_entry(&action);
    /* A deletion during delay or repeat interval stops the next launch. */
    lifecycle_seed();action=frozen_rule();action.delay_ms=200;delay_hook=delete_waiting_rule;cancelled_entry(&action);
    lifecycle_seed();action=frozen_rule();action.repeat_mode=TS_AUTO_REPEAT_COUNT;action.repeat_count=3;action.repeat_interval_ms=10;
    delay_hook=delete_waiting_rule;before=execution_io;
    assert(execute_action_with_repeat(&action,NULL,NULL)==ESP_ERR_INVALID_STATE&&execution_io==before+1);ts_action_snapshot_release(&action);
    /* A completed CLI operation does not write its reply into a recreated variable. */
    lifecycle_seed();
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"reply.exit_code\",\"source_id\":\"user\",\"value_type\":2,\"value\":9}",NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"reply.status\",\"source_id\":\"user\",\"value_type\":4,\"value\":\"before\"}",NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"reply.output\",\"source_id\":\"user\",\"value_type\":4,\"value\":\"before\"}",NULL,NULL)==ESP_OK);
    direct=(ts_auto_action_t){.type=TS_AUTO_ACT_CLI};strcpy(direct.cli.var_name,"reply");strcpy(direct.cli.command,"mock operation");
    assert(ts_action_snapshot(&direct,&action)==ESP_OK);epoch=action.runtime_epoch;
    entry=(ts_action_queue_entry_t){.action=action};execution_hook=replace_reply;
    assert(execute_entry(&entry,&result)==ESP_OK&&result.status==TS_ACTION_STATUS_SUCCESS);execution_hook=NULL;
    assert(epoch==ts_auto_config_execution_epoch());ts_auto_value_t value;
    assert(ts_variable_get("reply.status",&value)==ESP_OK&&!strcmp(value.str_val,"new"));
    assert(ts_variable_get("reply.exit_code",&value)==ESP_OK&&value.int_val==9);ts_action_snapshot_release(&action);
    /* Changing sources resets source-owned values/identities while SDK values remain. */
    lifecycle_seed();action=frozen_rule();ts_auto_source_t source;assert(ts_source_get_copy("source",&source)==ESP_OK);
    uint64_t sdk=ts_variable_instance("system.temperature");assert(ts_variable_set_int("mode_value",42)==ESP_OK);
    ts_auto_variable_t observed={0};strcpy(observed.name,"source.observed");strcpy(observed.source_id,"source");observed.value.type=TS_AUTO_VAL_INT;observed.value.int_val=7;
    assert(ts_variable_register(&observed)==ESP_OK);json=mode_seed();assert(ts_auto_config_install(TS_AUTO_CONFIG_INTERNAL,json,NULL)==ESP_OK);cJSON_Delete(json);
    ts_auto_source_t next;assert(ts_source_get_copy("source",&next)==ESP_OK&&next.instance!=source.instance);
    assert(!ts_variable_exists("source.observed")&&ts_variable_instance("system.temperature")==sdk);
    assert(ts_variable_get("mode_value",&value)==ESP_OK&&value.int_val==1);cancelled_entry(&action);
    /* Pause-and-force does no new runtime allocation, but retires old ownership. */
    lifecycle_seed();action=frozen_rule();ts_auto_rule_t held;assert(ts_rule_acquire("auto",&held)==ESP_OK);
    fail_prepare=true;
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"cmd",TS_AUTO_DELETE_FORCE,NULL,true,NULL,NULL,NULL)==ESP_OK);
    assert(status().paused&&ts_rule_count()==0&&ts_source_count()==0&&!ts_variable_exists("mode_value"));
    assert(ts_variable_exists("system.temperature")&&!strcmp(held.id,"auto"));cancelled_entry(&action);ts_rule_release(&held);
    fail_prepare=false;lifecycle_seed();
    puts("PASS action lifecycle: current dependencies, queued/delayed/repeated cancellation, recreated IDs, late reply isolation, source reset and paused force cleanup");
}
