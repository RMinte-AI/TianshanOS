#include "ts_ssh_service.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ts_keystore.h"
#include "ts_ssh_hosts_config.h"
#include "ts_ssh_log_watch.h"
#include "ts_ssh_probe.h"
#include "ts_variable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Configuration supports 64 commands; preserve that capacity without 64 tasks. */
typedef struct {
    char id[32], host[64], host_id[32], name[32], variable[32];
    char owner_rule[32];
    char instance[96]; /* boot ID + PID + /proc start ticks */
    uint32_t registration, operation;
    unsigned recovery_left;
    bool deleting, ready_evidence, recovery_worker;
    uint16_t port;
    unsigned pins;
    ts_ssh_service_status_t status;
} service_t;
static service_t *services; /* PSRAM; no allocation in normal rule evaluation */
static SemaphoreHandle_t mutex, binding_gate;
/* Unique for this boot, fail closed on exhaustion. Never reuse a retired ticket. */
static uint32_t serial;
static uint32_t ticket(void) { return serial == UINT32_MAX ? 0 : ++serial; }
static bool occupied(const service_t *s) {
    return s->deleting || s->pins || s->status.busy || strcmp(s->status.state, "stopped");
}
static bool complete(service_t *s, uint32_t registration, uint32_t operation) {
    if (!s || s->registration != registration || s->operation != operation || !operation)
        return false;
    s->operation = 0;
    s->status.busy = false;
    return true;
}
void ts_ssh_binding_lock(void) {
    if (binding_gate)
        xSemaphoreTake(binding_gate, portMAX_DELAY);
}
void ts_ssh_binding_unlock(void) {
    if (binding_gate)
        xSemaphoreGive(binding_gate);
}

esp_err_t ts_ssh_service_init(void) {
    if (!binding_gate)
        binding_gate = xSemaphoreCreateMutex();
    if (!mutex)
        mutex = xSemaphoreCreateMutex();
    if (!services)
        services = heap_caps_calloc(TS_SSH_COMMANDS_MAX, sizeof(*services),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return binding_gate && mutex && services ? ESP_OK : ESP_ERR_NO_MEM;
}
void ts_ssh_service_safe_name(const ts_ssh_command_config_t *cmd, char out[32]) {
    unsigned n = 0;
    const char *src = cmd->name;
    for (unsigned pass = 0; pass < 2 && !n; pass++, src = cmd->id)
        for (unsigned i = 0; src[i] && n < 20; i++)
            if ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z') ||
                (src[i] >= '0' && src[i] <= '9'))
                out[n++] = src[i];
    if (!n) {
        strcpy(out, "cmd");
        return;
    }
    out[n] = 0;
}
static service_t *find(const char *id) {
    for (unsigned i = 0; i < TS_SSH_COMMANDS_MAX; i++)
        if (!strcmp(services[i].id, id))
            return &services[i];
    return NULL;
}
/* Caller holds mutex. Command id plus actual configured target is authoritative;
 * a legacy PID-path collision between different ids is rejected. */
static service_t *register_service(const ts_ssh_command_config_t *cmd, const char *host,
                                   uint16_t port) {
    service_t *entry = find(cmd->id), *empty = NULL;
    char name[32];
    ts_ssh_service_safe_name(cmd, name);
    for (unsigned i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        service_t *s = &services[i];
        if (!s->id[0] && !empty)
            empty = s;
        if (s != entry && s->id[0] && !strcmp(s->host, host) && s->port == port &&
            !strcmp(s->name, name))
            return NULL;
    }
    if (!entry)
        entry = empty;
    if (!entry)
        return NULL;
    bool changed =
        entry->id[0] && (strcmp(entry->host, host) || entry->port != port ||
                         strcmp(entry->name, name) || strcmp(entry->variable, cmd->var_name));
    if (changed) {
        if (occupied(entry))
            return NULL;
        memset(entry, 0, sizeof(*entry));
    }
    if (entry->deleting)
        return NULL;
    if (!entry->id[0]) {
        entry->registration = ticket();
        if (!entry->registration) return NULL;
        entry->status.generation = entry->registration;
        strcpy(entry->host_id, cmd->host_id);
        snprintf(entry->id, sizeof(entry->id), "%s", cmd->id);
        snprintf(entry->host, sizeof(entry->host), "%s", host);
        strcpy(entry->name, name);
        entry->port = port;
        strcpy(entry->variable, cmd->var_name);
        strcpy(entry->status.state, "unknown");
        strcpy(entry->status.source, "none");
    }
    return entry;
}
esp_err_t ts_ssh_service_pin(const ts_ssh_command_config_t *cmd, const char *host, uint16_t port, uint32_t *registration) {
    if (!registration || !mutex || !services)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = register_service(cmd, host, port ? port : 22);
    if (s) {
        ++s->pins;
        *registration = s->registration;
    }
    xSemaphoreGive(mutex);
    return s ? ESP_OK : ESP_ERR_INVALID_STATE;
}
void ts_ssh_service_unpin(const char *id, uint32_t registration) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && s->registration == registration && s->pins)
        --s->pins;
    xSemaphoreGive(mutex);
}
bool ts_ssh_service_start_admissible(const char *id) {
    if (!mutex || !services)
        return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool allowed =
        !s || (!s->deleting && !s->pins && !s->status.busy &&
               (!strcmp(s->status.state, "stopped") ||
                (s->status.generation != UINT32_MAX && !strcmp(s->status.source, "none"))));
    xSemaphoreGive(mutex);
    return allowed;
}
static void state_set(service_t *s, const char *state, const char *source) {
    if (strcmp(s->status.state, "unknown") && strcmp(s->status.state, state))
        strcpy(s->status.last_known, s->status.state);
    snprintf(s->status.state, sizeof(s->status.state), "%s", state);
    snprintf(s->status.source, sizeof(s->status.source), "%s", source);
    s->status.confirmed_ms = esp_timer_get_time() / 1000;
    /* ts_variable_set is local, synchronous and has no callbacks (notify_change
     * currently only logs). Lock order: service -> variable; never inverse. */
    if (s->variable[0]) {
        char key[64];
        snprintf(key, sizeof(key), "%s.status", s->variable);
        ts_variable_set_string(key, state);
        if (!strcmp(state, "ready")) {
            snprintf(key, sizeof(key), "%s.ready_time", s->variable);
            ts_variable_set_int(key, (int32_t)time(NULL));
        }
    }
}
static bool instance_receipt(const char *text, const char *prefix, char out[96]) {
    if (!text || strncmp(text, prefix, strlen(prefix))) return false;
    const char *v = text + strlen(prefix);
    size_t len = strcspn(v, "\r\n");
    if (!len || len >= 96 || strspn(v, "0123456789abcdefABCDEF-:") != len ||
        (v[len] && strcmp(v + len, "\n") && strcmp(v + len, "\r\n"))) return false;
    unsigned separators = 0;
    for (size_t i = 0; i < len; ++i) separators += v[i] == ':';
    if (separators != 2) return false;
    if (out) { memcpy(out, v, len); out[len] = 0; }
    return true;
}
static esp_err_t probe(ts_ssh_session_t session, const char *name, bool stop, bool *running, char instance[96]) {
    /* PID file v2 contains exact session leader PID + /proc start time. Group
     * signals are used only after both are verified, never PID arithmetic. */
    char *command = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!command)
        return ESP_ERR_NO_MEM;
    int n = snprintf(command, 2048,
                     "d=/tmp/ts_nohup_%s.lock; f=$d/pid; "
                     "if [ -d \"$d\" ]; then [ -f \"$f\" ] || exit 2; else f=${d%%.lock}.pid; fi; "
                     "if [ ! -f \"$f\" ]; then echo STOPPED; exit 0; fi; "
                     "read p stamp < \"$f\" || exit 2; case $p in ''|*[!0-9]*) exit 2;; esac; "
                     "[ \"$p\" -gt 1 ] || exit 2; "
                     "if [ ! -r /proc/$p/stat ]; then if kill -0 -- -$p 2>/dev/null; then exit 2; "
                     "else rm -f -- \"$f\"; [ ! -d \"$d\" ] || rmdir -- \"$d\" || exit 2; echo "
                     "STOPPED; exit 0; fi; fi; "
                     "info=$(sed 's/^.*) //' /proc/$p/stat) || exit 2; set -- $info; "
                     "pg=$3; shift 19; actual=$1; ",
                     name);
    if (n < 0 || n >= 2048) {
        free(command);
        return ESP_ERR_INVALID_SIZE;
    }
    const char *tail = stop ? "[ -n \"$stamp\" ] && [ \"$stamp\" = \"$actual\" ] && [ \"$pg\" = "
                              "\"$p\" ] || exit 2; "
                              "kill -TERM -- -$p || exit 2; i=0; while [ $i -lt 30 ]; do "
                              "if ! kill -0 -- -$p 2>/dev/null; then rm -f -- \"$f\"; [ ! -d "
                              "\"$d\" ] || rmdir -- \"$d\" || exit 2; echo STOPPED; exit 0; fi; "
                              "sleep 0.1; i=$((i+1)); done; exit 3"
                            : "if [ -n \"$stamp\" ] && [ \"$stamp\" != \"$actual\" ]; then exit 2; "
                              "fi; boot=$(cat /proc/sys/kernel/random/boot_id) || exit 2; "
                              "printf 'RUNNING %s:%s:%s\\n' \"$boot\" \"$p\" \"$actual\"";
    if (strlen(tail) >= 2048 - n) {
        free(command);
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(command + n, tail);
    ts_ssh_exec_result_t result = {0};
    esp_err_t ret = ts_ssh_exec(session, command, &result);
    free(command);
    if (ret == ESP_OK) {
        if (result.exit_code == 0 && ts_ssh_probe_token(result.stdout_data, "STOPPED"))
            *running = false;
        else if (!stop && result.exit_code == 0 && instance_receipt(result.stdout_data, "RUNNING ", instance))
            *running = true;
        else
            ret = ESP_ERR_INVALID_RESPONSE;
    }
    ts_ssh_exec_result_free(&result);
    return ret;
}
esp_err_t ts_ssh_service_begin(const ts_ssh_command_config_t *cmd, ts_ssh_session_t session,
                               uint32_t *generation) {
    if (!services || !mutex || !cmd || !session || !generation)
        return ESP_ERR_INVALID_STATE;
    *generation = 0;
    ts_ssh_binding_lock();
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = register_service(cmd, ts_ssh_get_host(session), ts_ssh_get_port(session));
    if (!s || s->status.busy || s->status.generation == UINT32_MAX ||
        !strcmp(s->status.state, "checking") || !strcmp(s->status.state, "starting") ||
        !strcmp(s->status.state, "stopping")) {
        xSemaphoreGive(mutex);
        ts_ssh_binding_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t operation = ticket(), registration = s->registration;
    if (!operation) { xSemaphoreGive(mutex); ts_ssh_binding_unlock(); return ESP_ERR_INVALID_STATE; }
    s->operation = operation;
    s->status.busy = true;
    s->status.generation = operation;
    s->ready_evidence = false;
    s->instance[0] = 0;
    s->recovery_left = 3;
    s->recovery_worker = false;
    xSemaphoreGive(mutex);
    ts_ssh_binding_unlock();
    bool running = true;
    esp_err_t ret = probe(session, s->name, false, &running, NULL);
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (ret == ESP_OK && !running && s->status.generation == operation) {
        *generation = s->status.generation;
        state_set(s, "starting", "pid");
    } else {
        complete(s, registration, operation);
        state_set(s, ret == ESP_OK ? "running" : "unknown", "pid");
        if (ret == ESP_OK)
            ret = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(mutex);
    return ret;
}
bool ts_ssh_service_finish(const char *id, uint32_t generation, const char *receipt,
                           ts_ssh_session_t session) {
    char launched_instance[96];
    bool launched = instance_receipt(receipt, "STARTED ", launched_instance);
    char identity[96] = {0}, name[32] = {0};
    uint32_t registration = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && s->operation == generation) {
        registration = s->registration;
        strcpy(name, s->name);
    }
    xSemaphoreGive(mutex);
    if (!registration) return false;
    bool running = false;
    if (launched)
        launched = probe(session, name, false, &running, identity) == ESP_OK && running &&
                   !strcmp(identity, launched_instance);
    xSemaphoreTake(mutex, portMAX_DELAY);
    s = find(id);
    bool current = complete(s, registration, generation) && s->status.generation == generation;
    if (current) {
        if (launched) strcpy(s->instance, identity);
        state_set(s, launched ? "starting" : "unknown", "launch");
    }
    xSemaphoreGive(mutex);
    return current && launched;
}
/* A log result must bracket its read with verification of the captured instance. */
esp_err_t ts_ssh_service_verify_instance(const char *id, uint32_t generation,
                                         ts_ssh_session_t session) {
    char name[32], expected[96], actual[96] = {0};
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool valid = s && s->status.generation == generation && s->instance[0] &&
                 !s->deleting && strcmp(s->status.state, "stopped") &&
                 strcmp(s->status.state, "stopping") && strcmp(s->status.source, "cancelled");
    if (valid) { strcpy(name, s->name); strcpy(expected, s->instance); }
    xSemaphoreGive(mutex);
    if (!valid) return ESP_ERR_INVALID_STATE;
    bool running = false;
    esp_err_t ret = probe(session, name, false, &running, actual);
    if (ret == ESP_OK && (!running || strcmp(actual, expected))) ret = ESP_ERR_INVALID_STATE;
    return ret;
}
bool ts_ssh_service_observation_valid(const char *id, uint32_t generation) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool valid = s && s->status.generation == generation && s->instance[0] &&
                 !s->deleting && strcmp(s->status.source, "cancelled") &&
                 strcmp(s->status.state, "stopped") && strcmp(s->status.state, "stopping");
    xSemaphoreGive(mutex);
    return valid;
}
void ts_ssh_service_observe(const char *id, uint32_t generation, const char *state,
                            const char *var_name) {
    if (!mutex || !services || !id)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && generation == s->status.generation && strcmp(s->status.state, "stopping") &&
        strcmp(s->status.state, "stopped") && strcmp(s->status.source, "cancelled") && s->instance[0]) {
        if (!strcmp(state, "ready")) s->ready_evidence = true;
        state_set(s, state, "log");
    }
    xSemaphoreGive(mutex);
}
static esp_err_t connect_host(const ts_ssh_host_config_t *snapshot, ts_ssh_session_t *session, char **key) {
    const ts_ssh_host_config_t host = *snapshot;
    esp_err_t ret;
    size_t len = 0;
    if (!host.keyid[0])
        return ESP_ERR_INVALID_STATE;
    ret = ts_keystore_load_private_key(host.keyid, key, &len);
    if (ret != ESP_OK)
        return ret;
    ts_ssh_config_t cfg = TS_SSH_DEFAULT_CONFIG();
    cfg.host = host.host;
    cfg.port = host.port;
    cfg.username = host.username;
    cfg.auth_method = TS_SSH_AUTH_PUBLICKEY;
    cfg.auth.key.private_key = (const uint8_t *)*key;
    cfg.auth.key.private_key_len = len;
    cfg.timeout_ms = 5000;
    cfg.max_output_bytes = 128;
    ret = ts_ssh_session_create(&cfg, session);
    return ret == ESP_OK ? ts_ssh_connect(*session) : ret;
}
static esp_err_t inspect(const char *id, bool stop, uint32_t recovery, ts_ssh_service_status_t *out) {
    if (!mutex || !services || !id || !out)
        return ESP_ERR_INVALID_ARG;
    ts_ssh_command_config_t *cmd =
        heap_caps_malloc(sizeof(*cmd), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmd)
        return ESP_ERR_NO_MEM;
    ts_ssh_binding_lock();
    esp_err_t ret = ts_ssh_commands_config_get(id, cmd);
    ts_ssh_host_config_t host;
    if (ret == ESP_OK && (!cmd->nohup || !cmd->service_mode))
        ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK)
        ret = ts_ssh_hosts_config_get(cmd->host_id, &host);
    if (ret != ESP_OK) {
        ts_ssh_binding_unlock();
        free(cmd);
        return ret;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = register_service(cmd, host.host, host.port ? host.port : 22);
    if (!s) {
        xSemaphoreGive(mutex);
        ts_ssh_binding_unlock();
        free(cmd);
        return ESP_ERR_INVALID_STATE;
    }
    if (s->deleting || s->status.busy || serial == UINT32_MAX ||
        (recovery && (s->status.generation != recovery || !s->ready_evidence ||
                      !s->recovery_left || strcmp(s->status.state, "unknown")))) {
        *out = s->status;
        xSemaphoreGive(mutex);
        ts_ssh_binding_unlock();
        free(cmd);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t operation = ticket(), registration = s->registration;
    s->operation = operation;
    s->status.busy = true;
    if (recovery) --s->recovery_left;
    if (stop) {
        s->status.generation = operation;
        s->ready_evidence = false;
        s->recovery_left = 0;
        state_set(s, "stopping", "request");
    }
    uint32_t generation = s->status.generation;
    xSemaphoreGive(mutex);
    ts_ssh_binding_unlock();
    if (stop)
        ts_ssh_log_watch_cancel_name(cmd->var_name);
    ts_ssh_session_t session = NULL;
    char *key = NULL;
    bool running = true;
    char identity[96] = {0};
    ret = connect_host(&host, &session, &key);
    if (ret == ESP_OK)
        ret = probe(session, s->name, stop, &running, identity);
    if (session)
        ts_ssh_session_destroy(session);
    free(key);
    free(cmd);
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (complete(s, registration, operation) && s->status.generation == generation) {
        if (ret != ESP_OK)
            state_set(s, "unknown", "pid");
        else if (!running) {
            s->ready_evidence = false;
            s->instance[0] = 0;
            state_set(s, "stopped", "pid");
        } else if (s->ready_evidence && !strcmp(identity, s->instance))
            state_set(s, "ready", "pid+log");
        else if (strcmp(identity, s->instance)) {
            s->ready_evidence = false;
            s->instance[0] = 0;
            state_set(s, "running", "pid");
        } else if (strcmp(s->status.state, "checking") && strcmp(s->status.state, "ready"))
            state_set(s, "running", "pid");
        else {
            strcpy(s->status.source, "pid");
            s->status.confirmed_ms = esp_timer_get_time() / 1000;
        }
    }
    *out = s->status;
    xSemaphoreGive(mutex);
    return ret;
}
esp_err_t ts_ssh_service_query(const char *id, ts_ssh_service_status_t *out) {
    esp_err_t ret = inspect(id, false, 0, out);
    ts_ssh_service_recovery_kick(id);
    return ret;
}
esp_err_t ts_ssh_service_stop(const char *id, ts_ssh_service_status_t *out) {
    return inspect(id, true, 0, out);
}
bool ts_ssh_service_binding_busy(const char *id) {
    if (!mutex || !services)
        return true;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool busy = !s || occupied(s);
    xSemaphoreGive(mutex);
    return busy;
}
bool ts_ssh_service_launch_command(const ts_ssh_command_config_t *cmd, const char *expanded,
                                   char *out, size_t cap) {
    char name[32];
    ts_ssh_service_safe_name(cmd, name);
    int n = snprintf(out, cap, "mkdir /tmp/ts_nohup_%s.lock || exit 2; nohup setsid sh -c ", name);
    if (n < 0 || (size_t)n >= cap || !ts_ssh_shell_quote(expanded, out + n, cap - n))
        return false;
    n += strlen(out + n);
    int more = snprintf(
        out + n, cap - n,
        " > /tmp/ts_nohup_%s.log 2>&1 < /dev/null & p=$!; "
        "sleep 0.1; info=$(sed 's/^.*) //' /proc/$p/stat) || exit 2; "
        "set -- $info; [ \"$3\" = \"$p\" ] || exit 2; shift 19; "
        "printf '%%s %%s\\n' \"$p\" \"$1\" > /tmp/ts_nohup_%s.lock/pid || exit 2; "
        "boot=$(cat /proc/sys/kernel/random/boot_id) || exit 2; "
        "printf 'STARTED %%s:%%s:%%s\\n' \"$boot\" \"$p\" \"$1\"",
        name, name);
    return more >= 0 && (size_t)more < cap - n;
}

bool ts_ssh_service_command_protected(const char *id) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool protected = s && occupied(s);
    xSemaphoreGive(mutex);
    if (protected) return true;
    ts_ssh_command_config_t *cmd =
        heap_caps_malloc(sizeof(*cmd), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmd)
        return true;
    esp_err_t ret = ts_ssh_commands_config_get(id, cmd);
    bool busy = ret == ESP_OK && cmd->nohup && cmd->service_mode && ts_ssh_service_binding_busy(id);
    free(cmd);
    return ret != ESP_OK && ret != ESP_ERR_NOT_FOUND ? true : busy;
}
static bool protect_host(const ts_ssh_command_config_t *cmd, size_t index, void *context) {
    bool *busy = context;
    if (cmd->nohup && cmd->service_mode && ts_ssh_service_binding_busy(cmd->id))
        *busy = true;
    return !*busy;
}
bool ts_ssh_service_host_protected(const char *host_id) {
    bool busy = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < TS_SSH_COMMANDS_MAX; ++i)
        if (services[i].id[0] && (!host_id || !strcmp(services[i].host_id, host_id)) && occupied(&services[i]))
            busy = true;
    xSemaphoreGive(mutex);
    if (busy) return true;
    esp_err_t ret =
        host_id ? ts_ssh_commands_config_iterate_by_host(host_id, protect_host, &busy, 0, 0, NULL)
                : ts_ssh_commands_config_iterate(protect_host, &busy, 0, 0, NULL);
    return busy || ret != ESP_OK;
}

void ts_ssh_service_cancel_observation(const char *id, uint32_t generation) {
    if (!mutex || !services)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && s->status.generation == generation && strcmp(s->status.state, "stopping")) {
        s->status.generation = ticket();
        s->ready_evidence = false;
        s->recovery_left = 0;
        state_set(s, "unknown", "cancelled");
    }
    xSemaphoreGive(mutex);
}
bool ts_ssh_service_any_in_use(void) {
    if (!mutex || !services)
        return false;
    bool busy = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        service_t *s = &services[i];
        if (s->id[0] && (s->pins || s->status.busy ||
                         (strcmp(s->status.source, "none") && strcmp(s->status.state, "stopped"))))
            busy = true;
    }
    xSemaphoreGive(mutex);
    return busy;
}

/* Called with binding_gate held: deletion is an admission barrier through storage I/O. */
esp_err_t ts_ssh_service_delete_begin(const char *id, uint32_t *credential) {
    *credential = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    esp_err_t ret = ESP_OK;
    if (s && occupied(s)) ret = ESP_ERR_INVALID_STATE;
    else if (s) {
        *credential = ticket();
        if (!*credential) ret = ESP_ERR_INVALID_STATE;
        else { s->deleting = true; s->operation = *credential; }
    }
    xSemaphoreGive(mutex);
    return ret;
}
void ts_ssh_service_delete_finish(const char *id, uint32_t credential, bool removed) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && s->deleting && complete(s, s->registration, credential)) {
        if (removed) memset(s, 0, sizeof(*s));
        else s->deleting = false;
    }
    xSemaphoreGive(mutex);
}
void ts_ssh_service_forget_stopped(void) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < TS_SSH_COMMANDS_MAX; ++i)
        if (services[i].id[0] && !occupied(&services[i])) memset(&services[i], 0, sizeof(services[i]));
    xSemaphoreGive(mutex);
}
esp_err_t ts_ssh_service_cached(const char *id, ts_ssh_service_status_t *out) {
    if (!mutex || !id || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    memset(out, 0, sizeof(*out));
    if (s) *out = s->status;
    else { strcpy(out->state, "unknown"); strcpy(out->source, "none"); }
    xSemaphoreGive(mutex);
    return ESP_OK;
}
/* Three retries per launch, never replenished by query, UI, or recovery success. */
void ts_ssh_service_recovery_kick(const char *id) {
    ts_ssh_log_watch_config_t cfg = {.recovery_only = true, .timeout_sec = 20, .check_interval_ms = 5000};
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool start = s && !s->recovery_worker && !s->deleting && !s->status.busy &&
                 s->ready_evidence && s->recovery_left && !strcmp(s->status.state, "unknown");
    if (start) {
        s->recovery_worker = true;
        cfg.run_generation = s->status.generation;
        strcpy(cfg.command_id, s->id); strcpy(cfg.host_id, s->host_id); strcpy(cfg.var_name, s->variable);
    }
    xSemaphoreGive(mutex);
    if (start && ts_ssh_log_watch_start(&cfg, NULL) != ESP_OK) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        s = find(id);
        if (s && s->status.generation == cfg.run_generation) s->recovery_worker = false;
        xSemaphoreGive(mutex);
    }
}
bool ts_ssh_service_recover(const char *id, uint32_t generation) {
    ts_ssh_service_status_t status;
    inspect(id, false, generation, &status);
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    bool again = s && s->status.generation == generation && s->ready_evidence &&
                 s->recovery_left && !strcmp(s->status.state, "unknown");
    xSemaphoreGive(mutex);
    return again;
}
void ts_ssh_service_recovery_done(const char *id, uint32_t generation) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && s->status.generation == generation) s->recovery_worker = false;
    xSemaphoreGive(mutex);
}

/* Rule ownership survives queue completion and even missing/replaced templates. */
void ts_ssh_service_set_owner(const char *id, uint32_t registration, const char *rule) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    service_t *s = find(id);
    if (s && s->registration == registration && s->pins)
        snprintf(s->owner_rule, sizeof(s->owner_rule), "%s", rule);
    xSemaphoreGive(mutex);
}
bool ts_ssh_service_rule_protected(const char *rule) {
    bool busy = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < TS_SSH_COMMANDS_MAX; ++i)
        if (services[i].id[0] && !strcmp(services[i].owner_rule, rule) && occupied(&services[i]))
            busy = true;
    xSemaphoreGive(mutex);
    return busy;
}
