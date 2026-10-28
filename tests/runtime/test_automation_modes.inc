static cJSON *mode_seed(void)
{
    cJSON *json=cJSON_Parse("{\"format_version\":1,\"migration_version\":1,\"revision\":1,\"saved_at\":10,\"enabled\":true,"
        "\"rules\":[{\"id\":\"auto\",\"name\":\"SD rule\",\"enabled\":true,\"cooldown_ms\":0,\"conditions\":[{\"variable\":\"mode_value\",\"value\":1}],\"actions\":[{\"template_id\":\"action\"}]},"
        "{\"id\":\"manual\",\"name\":\"Manual rule\",\"enabled\":true,\"manual_trigger\":true,\"allow_manual_trigger\":true,\"actions\":[{\"type\":\"log\",\"message\":\"manual\"}]}],"
        "\"actions\":[{\"id\":\"action\",\"name\":\"Action\",\"enabled\":true,\"type\":\"ssh_cmd_ref\",\"ssh_ref\":{\"cmd_id\":\"cmd\"}}],"
        "\"commands\":[{\"id\":\"cmd\",\"name\":\"Command\",\"host_id\":\"host\",\"command\":\"true\",\"enabled\":true}],"
        "\"hosts\":[{\"id\":\"host\",\"host\":\"192.0.2.1\",\"username\":\"operator\",\"enabled\":true}],"
        "\"sources\":[{\"id\":\"source\",\"label\":\"Source\",\"type\":0,\"enabled\":false}],"
        "\"variables\":[{\"name\":\"mode_value\",\"source_id\":\"user\",\"value_type\":2,\"value\":1}]}");
    assert(json);return json;
}
static void mode_reboot(void)
{ ts_auto_config_close();s_config.control_loaded=false; }
static void mode_name(const char *name)
{
    ts_auto_rule_t rule;assert(ts_rule_definition_get("auto",&rule)==ESP_OK);
    snprintf(rule.name,sizeof(rule.name),"%s",name);
    assert(ts_rule_commit(&rule,rule.id,rule.revision,NULL)==ESP_OK);ts_rule_dispose(&rule);
}
static void mode_assert_name(const char *name)
{
    ts_auto_rule_t rule;assert(ts_rule_definition_get("auto",&rule)==ESP_OK);
    assert(!strcmp(rule.name,name));ts_rule_dispose(&rule);
}
static esp_err_t apply_preview(ts_auto_config_operation_t operation,cJSON *preview,ts_auto_config_error_t *error)
{
    return ts_auto_config_apply(operation,cJSON_GetObjectItem(preview,"active_version")->valuestring,
        cJSON_GetObjectItem(preview,"target_version")->valuestring,error);
}
static void mode_cases(void)
{
    ts_auto_config_error_t error={0};cJSON *seed=mode_seed();
    assert(ts_auto_config_install(TS_AUTO_CONFIG_INTERNAL,seed,&error)==ESP_OK);
    assert(ts_auto_config_install(TS_AUTO_CONFIG_SD,seed,&error)==ESP_OK);cJSON_Delete(seed);
    ts_auto_control_t control={.source=TS_AUTO_CONFIG_SD};assert(ts_auto_control_save(&control)==ESP_OK);
    mode_reboot();assert(ts_auto_config_startup(&error)==ESP_OK);
    assert(status().source==TS_AUTO_CONFIG_SD && status().configured && status().internal_valid && status().backup_matches);
    unsigned probes=sd_probes;ts_auto_config_status_t before=status();assert(sd_probes==probes); /* GET does no I/O. */
    char internal[65],sd[65],changed[65];assert(ts_auto_file_fingerprint(TS_AUTO_FILE_INTERNAL,internal)==ESP_OK);
    mode_name("Edited on SD");assert(!status().backup_matches && status().comparison_known);
    assert(ts_auto_file_fingerprint(TS_AUTO_FILE_INTERNAL,changed)==ESP_OK&&!strcmp(internal,changed));
    assert(ts_auto_config_backup(before.active_version,&error)==ESP_ERR_INVALID_STATE&&!strcmp(error.reason,"configuration_changed"));
    before=status();assert(ts_auto_config_backup(before.active_version,&error)==ESP_OK);
    assert(status().backup_matches&&!strcmp(status().active_version,before.active_version));
    char *backup=NULL;assert(ts_auto_file_load(TS_AUTO_FILE_INTERNAL,&backup)==ESP_OK);
    cJSON *all=cJSON_Parse(backup);free(backup);
    for(unsigned kind=0;kind<TS_AUTO_CONFIG_KIND_COUNT;++kind)assert(cJSON_GetArraySize(cJSON_GetObjectItem(all,ts_auto_config_collection(kind)))>0);
    cJSON_Delete(all);
    assert(ts_auto_file_fingerprint(TS_AUTO_FILE_SD,sd)==ESP_OK);
    sd_present=false;assert(ts_auto_config_refresh(&error)==ESP_OK);
    assert(status().source==TS_AUTO_CONFIG_INTERNAL&&!status().sd_available&&status().enabled);
    assert(ts_rule_evaluate_all()==1); /* The manual rule stays manual. */
    mode_name("Edited without a card");
    assert(ts_auto_file_fingerprint(TS_AUTO_FILE_SD,changed)==ESP_OK&&!strcmp(sd,changed));
    sd_present=true;assert(ts_auto_config_refresh(&error)==ESP_OK);
    assert(status().source==TS_AUTO_CONFIG_INTERNAL&&status().awaiting_sd_choice);
    mode_reboot();assert(ts_auto_config_startup(&error)==ESP_OK&&status().source==TS_AUTO_CONFIG_INTERNAL);
    mode_assert_name("Edited without a card");
    cJSON *preview=NULL;assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_OK);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(preview,"diff"),"changed"))==1);
    /* An external card edit after confirmation cannot be silently overwritten. */
    char *bytes=NULL;assert(ts_auto_file_load(TS_AUTO_FILE_SD,&bytes)==ESP_OK);
    cJSON *external=cJSON_Parse(bytes);free(bytes);cJSON_AddStringToObject(external,"external_note","changed after preview");
    bytes=cJSON_PrintUnformatted(external);cJSON_Delete(external);assert(ts_auto_file_save(TS_AUTO_FILE_SD,bytes)==ESP_OK);free(bytes);
    assert(apply_preview(TS_AUTO_USE_SD,preview,&error)==ESP_ERR_INVALID_STATE&&!strcmp(error.reason,"configuration_target_changed"));cJSON_Delete(preview);
    assert(ts_auto_file_fingerprint(TS_AUTO_FILE_INTERNAL,internal)==ESP_OK);
    assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_OK);
    assert(apply_preview(TS_AUTO_USE_SD,preview,&error)==ESP_OK&&status().source==TS_AUTO_CONFIG_SD);cJSON_Delete(preview);
    mode_assert_name("Edited on SD");assert(ts_auto_file_fingerprint(TS_AUTO_FILE_INTERNAL,changed)==ESP_OK&&!strcmp(internal,changed));
    sd_present=false;assert(ts_auto_config_refresh(&error)==ESP_OK);mode_assert_name("Edited without a card");
    sd_present=true;assert(ts_auto_config_refresh(&error)==ESP_OK);
    assert(ts_auto_config_preview(TS_AUTO_SAVE_TO_SD,&preview,&error)==ESP_OK);
    before=status();fail_control=true;error=(ts_auto_config_error_t){0};
    cJSON *params=cJSON_Duplicate(preview,true);assert(params&&cJSON_AddStringToObject(params,"operation","save_to_sd"));
    ts_api_result_t response={0};assert(api_config_apply(params,&response)==ESP_OK);
    assert(response.code!=TS_API_OK&&!strcmp(response.message,"saved_to_sd_not_switched")&&cJSON_IsTrue(cJSON_GetObjectItem(response.data,"saved")));
    assert(!strcmp(cJSON_GetObjectItem(response.data,"source")->valuestring,"internal"));
    free(response.message);cJSON_Delete(response.data);cJSON_Delete(params);
    assert(status().source==TS_AUTO_CONFIG_INTERNAL&&!strcmp(status().active_version,before.active_version));
    fail_control=false;
    assert(apply_preview(TS_AUTO_SAVE_TO_SD,preview,&error)==ESP_ERR_INVALID_STATE&&!strcmp(error.reason,"configuration_target_changed"));cJSON_Delete(preview);
    assert(ts_auto_file_fingerprint(TS_AUTO_FILE_SD,sd)==ESP_OK);
    assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_OK&&apply_preview(TS_AUTO_USE_SD,preview,&error)==ESP_OK);cJSON_Delete(preview);
    assert(ts_auto_file_fingerprint(TS_AUTO_FILE_SD,changed)==ESP_OK&&!strcmp(sd,changed)); /* Retry only switches. */
    mode_assert_name("Edited without a card");mode_name("New SD edit");
    assert(ts_auto_config_preview(TS_AUTO_RESTORE_INTERNAL,&preview,&error)==ESP_OK);
    before=status();fail_install=true;
    assert(apply_preview(TS_AUTO_RESTORE_INTERNAL,preview,&error)==ESP_FAIL&&!strcmp(status().active_version,before.active_version));
    fail_install=false;assert(apply_preview(TS_AUTO_RESTORE_INTERNAL,preview,&error)==ESP_OK);cJSON_Delete(preview);
    mode_assert_name("Edited without a card");assert(status().backup_matches);
    assert(ts_auto_config_enable(false,NULL,&error)==ESP_OK);
    unsigned checked=sd_probes;sd_mounted=false;++sd_generation; /* An explicit unmount leaves the physical card inserted. */
    assert(ts_auto_config_storage_changed(&error)==ESP_OK&&sd_probes==checked&&status().source==TS_AUTO_CONFIG_INTERNAL&&status().paused);
    assert(ts_auto_config_refresh(&error)==ESP_OK&&status().awaiting_sd_choice);
    assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_OK&&cJSON_IsTrue(cJSON_GetObjectItem(preview,"will_remain_paused")));
    assert(apply_preview(TS_AUTO_USE_SD,preview,&error)==ESP_OK&&status().paused);cJSON_Delete(preview);
    assert(ts_auto_config_preview(TS_AUTO_RESTORE_INTERNAL,&preview,&error)==ESP_OK&&apply_preview(TS_AUTO_RESTORE_INTERNAL,preview,&error)==ESP_OK&&status().paused);cJSON_Delete(preview);
    assert(ts_auto_config_enable(true,NULL,&error)==ESP_OK&&status().enabled);
    /* A driver-visible card with unreadable configuration also uses the internal copy. */
    mode_reboot();fail_sd_read=true;
    assert(ts_auto_config_startup(&error)==ESP_OK&&status().source==TS_AUTO_CONFIG_INTERNAL);
    fail_sd_read=false;
    assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_OK&&apply_preview(TS_AUTO_USE_SD,preview,&error)==ESP_OK);cJSON_Delete(preview);
    /* Damaged JSON is shown as damaged content, and an explicit restore can replace it. */
    FILE *broken=test_open(s_paths[TS_AUTO_FILE_SD].current,"wb");assert(broken&&fputs("{broken",broken)>=0&&!fclose(broken));
    mode_reboot();assert(ts_auto_config_startup(&error)==ESP_ERR_INVALID_ARG&&status().source==TS_AUTO_CONFIG_SD&&!status().loaded&&!strcmp(error.reason,"configuration_invalid"));
    ts_api_result_t unavailable={0};
    ts_api_auto_list(TS_AUTO_CONFIG_ACTION,NULL,"templates",&unavailable);
    assert(!strcmp(unavailable.message,"configuration_invalid"));ts_api_result_free(&unavailable);
    ts_api_auto_get(TS_AUTO_CONFIG_ACTION,NULL,&unavailable);
    assert(!strcmp(unavailable.message,"configuration_invalid"));ts_api_result_free(&unavailable);
    response=(ts_api_result_t){0};assert(api_automation_reload(NULL,&response)==ESP_OK&&response.code!=TS_API_OK&&!strcmp(response.message,"configuration_invalid"));
    free(response.message);cJSON_Delete(response.data);
    assert(ts_auto_config_preview(TS_AUTO_RESTORE_INTERNAL,&preview,&error)==ESP_OK&&apply_preview(TS_AUTO_RESTORE_INTERNAL,preview,&error)==ESP_OK);cJSON_Delete(preview);
    /* Choosing an empty card is a deliberate empty configuration. */
    sd_present=false;assert(ts_auto_config_refresh(&error)==ESP_OK);assert(test_unlink(s_paths[TS_AUTO_FILE_SD].current)==0);
    sd_present=true;assert(ts_auto_config_refresh(&error)==ESP_OK);
    assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_OK&&apply_preview(TS_AUTO_USE_SD,preview,&error)==ESP_OK);cJSON_Delete(preview);
    assert(status().source==TS_AUTO_CONFIG_SD&&!status().configured&&ts_rule_count()==0);
    mode_reboot();assert(ts_auto_config_startup(&error)==ESP_OK&&!status().configured&&ts_rule_count()==0);
    /* A first boot without any old automation creates no fictitious backup. */
    mode_reboot();assert(test_unlink(s_paths[TS_AUTO_FILE_INTERNAL].current)==0);
    has_auto_v=false;has_control=false;sd_present=sd_mounted=false;legacy_reads=0;
    assert(ts_auto_config_startup(&error)==ESP_OK&&legacy_reads==1&&auto_v==1);
    assert(status().source==TS_AUTO_CONFIG_INTERNAL&&!status().configured&&!status().internal_exists&&ts_rule_count()==0);
    assert(ts_auto_file_load(TS_AUTO_FILE_INTERNAL,&bytes)==ESP_ERR_NOT_FOUND);
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"draft\",\"name\":\"Draft\",\"enabled\":false,\"actions\":[]}",NULL,&error)==ESP_OK&&status().configured);
    mode_reboot();assert(ts_auto_config_startup(&error)==ESP_OK&&legacy_reads==1&&status().configured);
    /* A later boot never imports old data because a new file was removed. */
    mode_reboot();assert(test_unlink(s_paths[TS_AUTO_FILE_INTERNAL].current)==0);
    assert(ts_auto_config_startup(&error)==ESP_OK&&legacy_reads==1&&!status().configured);
    /* First upgrade with a card writes only the card and commits the marker. */
    mode_reboot();has_auto_v=false;has_control=false;sd_present=sd_mounted=true;legacy_document=mode_seed();
    cJSON_AddBoolToObject(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(legacy_document,"commands"),0),"nohup",true);
    fail_service_import=true;unsigned imports_before=service_imports;
    assert(ts_auto_config_startup(&error)==ESP_FAIL&&!has_auto_v&&status().configured&&service_imports==imports_before+1);
    assert(!strcmp(error.reason,"legacy_service_records_not_saved"));
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"cmd",TS_AUTO_DELETE_FORCE,NULL,false,status().active_version,NULL,&error)==ESP_OK);
    mode_reboot();fail_service_import=false;
    assert(ts_auto_config_startup(&error)==ESP_OK&&status().source==TS_AUTO_CONFIG_SD&&status().configured);
    assert(service_imports==imports_before+2&&auto_v==1);
    ts_ssh_command_config_t deleted;assert(ts_ssh_commands_config_get("cmd",&deleted)==ESP_ERR_NOT_FOUND);
    assert(ts_auto_file_load(TS_AUTO_FILE_INTERNAL,&bytes)==ESP_ERR_NOT_FOUND);
    cJSON_Delete((cJSON*)legacy_document);legacy_document=NULL;
    assert(ts_auto_config_backup(status().active_version,&error)==ESP_OK); /* Both files exist for fixture cleanup. */
    ts_auto_config_shutdown();
    assert(ts_auto_config_preview(TS_AUTO_USE_SD,&preview,&error)==ESP_ERR_INVALID_STATE&&!strcmp(error.reason,"configuration_unavailable"));
    puts("PASS configuration modes: manual whole-graph backup, emergency edits, persisted card choice, preview conflicts, partial switch, empty card and one-time startup migration");
}
