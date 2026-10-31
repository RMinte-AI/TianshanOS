"""Real power workers and service adapters; all ADC/UART/device/network outputs are local fixtures."""
from pathlib import Path
from test_console_lifecycle import RTOS, body, run_native, service_source

monitor = 'components/ts_drivers/src/ts_power_monitor.c'
policy = 'components/ts_drivers/src/ts_power_policy.c'


def state(path, name, variable):
    text = Path(path).read_text()
    start = text.index('typedef struct {')
    end = text.index('} ' + name + ';', start) + len(name) + 3
    return text[start:end] + '\nstatic ' + name + ' ' + variable + '={0};\n'


code = RTOS + r'''
#include <math.h>
#include "ts_power_monitor.h"
#include "ts_power_policy.h"
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef void *QueueHandle_t;
#define TS_EVENT_BASE_POWER 2
#define TS_POWER_POLICY_MIN_VALID_VOLTAGE 5.0f
#define LPMU_IP "fixture-only"
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 0
#define ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED 0
#define TAG "fixture"
#define POWER_ADC_GPIO 18
#define POWER_UART_NUM 1
#define POWER_UART_RX_GPIO 47
#define POWER_UART_BAUD_RATE 9600
'''
code += state(monitor, 'power_monitor_state_t', 's_pm')
code += state(policy, 'power_policy_state_t', 's_pp')
code += r'''
static SemaphoreHandle_t held_entered,held_release,ping_entered,ping_release,recovery_entered,recovery_release;
static atomic_bool hold_policy,hold_monitor,hold_ping,hold_recovery,stop_done;
static atomic_int voltage_samples,policy_samples,agx_off,lpmu_off,restarts;
static int stop_result;
static float fixture_voltage=20.0f;
static bool persisted_enabled=true,monitor_init_failure;
static char order[128];static int order_length;
static void record(char event) {assert(order_length+1<sizeof(order));order[order_length++]=event;order[order_length]=0;}
static void power_monitor_task(void *);
static void power_policy_task(void *);
static int stop_pm_task(void);
static int stop_pp_task(void);
static void monitor_trigger_event(ts_power_event_type_t,void *);
static void update_statistics(void);
static void trigger_event(ts_power_policy_event_t);
static void check_voltage(float);
static void check_device_status(void);
static void execute_shutdown(void);
static void execute_recovery(void);
static int voltage_monitor_init(void) {return monitor_init_failure?ESP_FAIL:ESP_OK;}
static int power_chip_init(void) {return ESP_OK;}
static int adc_oneshot_del_unit(void *p) {(void)p;return ESP_OK;}
static int uart_driver_delete(int n) {(void)n;return ESP_OK;}
static void vQueueDelete(void *q) {(void)q;}
static int read_voltage_sample(ts_power_voltage_data_t *data) {memset(data,0,sizeof(*data));data->supply_voltage=fixture_voltage;++voltage_samples;return ESP_OK;}
static int read_power_chip_packet(ts_power_chip_data_t *data) {memset(data,0,sizeof(*data));data->crc_valid=!hold_monitor;return ESP_OK;}
static int load_config_values(const char *key) {(void)key;s_pp.enabled_on_boot=persisted_enabled;s_pp.config.low_voltage_threshold=12.7f;return ESP_OK;}
static int save_config_values(bool enabled) {(void)enabled;return ESP_OK;}
esp_err_t ts_power_policy_register_variables(void) {return ESP_OK;}
esp_err_t ts_power_policy_update_variables(void) {++policy_samples;return ESP_OK;}
static void update_led_status(void) {}
typedef struct {int state;} ts_device_status_t;
enum {TS_DEVICE_AGX,TS_DEVICE_LPMU,TS_DEVICE_STATE_ON=1};
static int ts_device_get_status(int device,ts_device_status_t *status) {(void)device;status->state=TS_DEVICE_STATE_ON;return ESP_OK;}
static int ts_device_power_off(int device) {if(device==TS_DEVICE_AGX){++agx_off;record('A');}else{++lpmu_off;record('L');}return ESP_OK;}
static bool ping_host(const char *ip,int timeout) {(void)ip;assert(timeout==2000);record('P');if(hold_ping){xSemaphoreGive(ping_entered);assert(xSemaphoreTake(ping_release,portMAX_DELAY));}return true;}
static void stop_all_fans(void) {record('F');}
static void esp_restart(void) {++restarts;record('R');s_pp.running=false;}
static void fixture_delay(unsigned ticks) {
    if(current_task==s_pp.monitor_task_handle&&ticks==100){record('D');if(hold_recovery){xSemaphoreGive(recovery_entered);assert(xSemaphoreTake(recovery_release,portMAX_DELAY));}}
    vTaskDelay(ticks);
}
#define vTaskDelay fixture_delay
'''
for name in ['ts_power_monitor_get_default_config', 'ts_power_monitor_init', 'ts_power_monitor_deinit',
             'ts_power_monitor_start', 'stop_pm_task', 'ts_power_monitor_stop',
             'ts_power_monitor_is_initialized', 'ts_power_monitor_is_running', 'power_monitor_task',
             'update_statistics', 'trigger_event', 'ts_power_monitor_get_voltage_data',
             'ts_power_monitor_read_voltage_now']:
    code += body(monitor, name).replace('trigger_event(', 'monitor_trigger_event(')
for name in ['ts_power_policy_get_default_config', 'ts_power_policy_init', 'ts_power_policy_deinit',
             'ts_power_policy_start', 'stop_pp_task', 'ts_power_policy_stop',
             'ts_power_policy_is_initialized', 'ts_power_policy_is_running', 'ts_power_policy_should_auto_start',
             'ts_power_policy_get_status', 'trigger_event', 'check_voltage', 'check_device_status',
             'execute_shutdown', 'execute_recovery', 'power_policy_task']:
    code += body(policy, name)
code += '\n#undef vTaskDelay\n#undef TAG\n' + service_source()
for name in ['power_service_init', 'power_service_start', 'power_service_stop']:
    code += body('main/ts_services.c', name)
code += r'''
/* Retained LED rendering and the actual public initializer/accessor. */
static struct {bool initialized,render_running;SemaphoreHandle_t mutex;int devices[8];TaskHandle_t render_task;} s_led;
static atomic_int render_ticks;
static int ts_led_cc_init(void) {return ESP_OK;}
static void render_task(void *arg) {(void)arg;while(s_led.render_running){++render_ticks;vTaskDelay(1);}vTaskDelete(NULL);}
#define CONFIG_TS_LED_MAX_DEVICES 8
'''
code += body('components/ts_led/src/ts_led.c', 'ts_led_init')
code += body('components/ts_led/src/ts_led.c', 'ts_led_is_initialized')
code += r'''
static int touch,board,matrix,created,boot_loads;
static void *ts_led_touch_get(void) {return touch?&touch:NULL;}
static void *ts_led_board_get(void) {return board?&board:NULL;}
static void *ts_led_matrix_get(void) {return matrix?&matrix:NULL;}
static int ts_led_touch_init(void) {assert(!touch);touch=1;++created;return ESP_OK;}
static int ts_led_board_init(void) {assert(!board);board=1;++created;return ESP_OK;}
static int ts_led_matrix_init(void) {assert(!matrix);matrix=1;++created;return ESP_OK;}
static int ts_led_load_all_boot_config(void) {++boot_loads;return ESP_OK;}
'''
for name in ['led_service_init', 'led_service_start', 'led_service_stop']:
    code += body('main/ts_services.c', name)
code += r'''
static void policy_callback(ts_power_policy_event_t event,const ts_power_policy_status_t *status,void *user) {
    (void)event;(void)status;(void)user;if(atomic_exchange(&hold_policy,false)){xSemaphoreGive(held_entered);assert(xSemaphoreTake(held_release,portMAX_DELAY));}
}
static void monitor_callback(ts_power_event_type_t event,void *data,void *user) {
    (void)data;(void)user;if(event==TS_POWER_EVENT_CRC_ERROR&&atomic_exchange(&hold_monitor,false)){xSemaphoreGive(held_entered);assert(xSemaphoreTake(held_release,portMAX_DELAY));}
}
static void *policy_stop_thread(void *arg) {(void)arg;stop_result=ts_power_policy_stop();stop_done=true;return NULL;}
static void *monitor_stop_thread(void *arg) {(void)arg;stop_result=ts_power_monitor_stop();stop_done=true;return NULL;}
static void stop_during(bool policy,SemaphoreHandle_t release) {
    pthread_t stop;stop_done=false;assert(!pthread_create(&stop,NULL,policy?policy_stop_thread:monitor_stop_thread,NULL));
    if(policy)WAIT_UNTIL(!ts_power_policy_is_running());else WAIT_UNTIL(!ts_power_monitor_is_running());
    assert(!stop_done);assert((policy?ts_power_policy_start():ts_power_monitor_start())==ESP_ERR_NOT_FINISHED);
    xSemaphoreGive(release);assert(!pthread_join(stop,NULL));assert(stop_done&&stop_result==ESP_OK);
}
int main(void) {
    held_entered=xSemaphoreCreateBinary();held_release=xSemaphoreCreateBinary();ping_entered=xSemaphoreCreateBinary();ping_release=xSemaphoreCreateBinary();recovery_entered=xSemaphoreCreateBinary();recovery_release=xSemaphoreCreateBinary();
    assert(ts_service_init()==ESP_OK);
    ts_service_def_t definition={.name="power",.capabilities=TS_SERVICE_CAP_RESTARTABLE,.init=power_service_init,.start=power_service_start,.stop=power_service_stop};ts_service_handle_t service;
    assert(ts_service_register(&definition,&service)==ESP_OK);assert(ts_service_start(service)==ESP_OK);
    for(int i=0;i<3;i++){int before=voltage_samples;WAIT_UNTIL(voltage_samples>before);assert(ts_service_restart(service)==ESP_OK);assert(ts_power_policy_is_running()&&ts_power_monitor_is_running());assert(s_pp.config.low_voltage_threshold==12.7f);}
    assert(ts_service_stop(service)==ESP_OK);join_tasks();
    /* State mutex is held by the actual worker callback while stop is requested. */
    s_pp.callback=policy_callback;s_pp.debug_mode=true;hold_policy=true;assert(ts_power_policy_start()==ESP_OK);assert(xSemaphoreTake(held_entered,portMAX_DELAY));
    stop_during(true,held_release);assert(xSemaphoreTake(s_pp.state_mutex,0));xSemaphoreGive(s_pp.state_mutex);join_tasks();s_pp.debug_mode=false;
    s_pm.callback=monitor_callback;hold_monitor=true;assert(ts_power_monitor_start()==ESP_OK);assert(xSemaphoreTake(held_entered,portMAX_DELAY));
    stop_during(false,held_release);assert(xSemaphoreTake(s_pm.data_mutex,0));xSemaphoreGive(s_pm.data_mutex);join_tasks();
    /* Actual service must preserve failures from either owner, including busy. */
    assert(ts_service_start(service)==ESP_OK);int events=stopped_events;
    atomic_store(&s_pp.control_busy,true);assert(ts_service_stop(service)==ESP_ERR_NOT_FINISHED);atomic_store(&s_pp.control_busy,false);
    assert(ts_service_get_state(service)==TS_SERVICE_STATE_RUNNING&&stopped_events==events);
    atomic_store(&s_pm.control_busy,true);assert(ts_service_stop(service)==ESP_ERR_NOT_FINISHED);atomic_store(&s_pm.control_busy,false);
    assert(ts_service_get_state(service)==TS_SERVICE_STATE_RUNNING&&stopped_events==events);
    assert(ts_service_stop(service)==ESP_OK);join_tasks();
    /* No new LPMU command can follow a ping that was in flight when disabled. */
    s_pp.state=TS_POWER_POLICY_STATE_SHUTDOWN;s_pp.config.enable_device_shutdown=true;s_pp.config.lpmu_ping_before_shutdown=true;s_pp.config.enable_fan_control=true;
    order_length=0;hold_ping=true;int old_lpmu=lpmu_off;assert(ts_power_policy_start()==ESP_OK);assert(xSemaphoreTake(ping_entered,portMAX_DELAY));
    stop_during(true,ping_release);join_tasks();assert(!strcmp(order,"AP")&&lpmu_off==old_lpmu&&s_pp.state==TS_POWER_POLICY_STATE_SHUTDOWN);hold_ping=false;
    order_length=0;assert(ts_power_policy_start()==ESP_OK);WAIT_UNTIL(lpmu_off>old_lpmu);assert(ts_power_policy_stop()==ESP_OK);join_tasks();assert(!strcmp(order,"APL"));
    /* Recovery delay is also an operation boundary; without stop it still restarts. */
    s_pp.state=TS_POWER_POLICY_STATE_RECOVERY;s_pp.recovery_timer_sec=1;hold_recovery=true;order_length=0;assert(ts_power_policy_start()==ESP_OK);assert(xSemaphoreTake(recovery_entered,portMAX_DELAY));
    stop_during(true,recovery_release);join_tasks();assert(restarts==0&&!strcmp(order,"D"));
    s_pp.recovery_timer_sec=1;hold_recovery=false;order_length=0;assert(ts_power_policy_start()==ESP_OK);WAIT_UNTIL(restarts==1);assert(ts_power_policy_stop()==ESP_OK);join_tasks();assert(!strcmp(order,"DR"));
    /* Failed task creation doesn't leave a running flag, handle, or busy owner. */
    fail_next_create=true;assert(ts_power_policy_start()==ESP_FAIL&&!ts_power_policy_is_running());
    fail_next_create=true;assert(ts_power_monitor_start()==ESP_ERR_NO_MEM&&!ts_power_monitor_is_running());
    assert(ts_power_monitor_start()==ESP_OK);fail_next_create=true;
    assert(ts_service_start(service)==ESP_FAIL&&ts_service_get_state(service)==TS_SERVICE_STATE_ERROR);
    assert(!ts_power_policy_is_running());assert(ts_power_monitor_stop()==ESP_OK);join_tasks();
    assert(ts_power_policy_deinit()==ESP_OK);assert(ts_power_monitor_deinit()==ESP_OK);
    persisted_enabled=false;assert(power_service_init(NULL,NULL)==ESP_OK);assert(power_service_start(NULL,NULL)==ESP_OK);
    assert(!ts_power_policy_is_running()&&ts_power_monitor_is_running());int before=voltage_samples;WAIT_UNTIL(voltage_samples>before);
    assert(power_service_stop(NULL,NULL)==ESP_OK);join_tasks();assert(ts_power_monitor_deinit()==ESP_OK);assert(ts_power_policy_deinit()==ESP_OK);
    /* Optional monitor init failure remains nonfatal, including stop. */
    monitor_init_failure=true;assert(power_service_init(NULL,NULL)==ESP_OK);assert(power_service_start(NULL,NULL)==ESP_OK);assert(power_service_stop(NULL,NULL)==ESP_OK);assert(ts_power_policy_deinit()==ESP_OK);
    /* Retained LED resources are reused, presets are not allocated a second time. */
    ts_service_handle_t led;definition=(ts_service_def_t){.name="led",.capabilities=TS_SERVICE_CAP_RESTARTABLE,.init=led_service_init,.start=led_service_start,.stop=led_service_stop};
    assert(ts_service_register(&definition,&led)==ESP_OK);assert(ts_service_start(led)==ESP_OK);for(int i=0;i<4;i++)assert(ts_service_restart(led)==ESP_OK);
    WAIT_UNTIL(render_ticks>0);assert(created==3&&boot_loads==5);assert(ts_service_stop(led)==ESP_OK);s_led.render_running=false;join_tasks();vSemaphoreDelete(s_led.mutex);
    assert(ts_service_deinit()==ESP_OK);
    vSemaphoreDelete(held_entered);vSemaphoreDelete(held_release);vSemaphoreDelete(ping_entered);vSemaphoreDelete(ping_release);vSemaphoreDelete(recovery_entered);vSemaphoreDelete(recovery_release);
    puts("PASS actual power workers/adapters: repeated restart, locks, new samples, stop during ping/recovery, optional monitor, saved disable, retained LED");
}
'''
run_native(code)
