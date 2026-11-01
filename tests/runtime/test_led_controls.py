"""Actual LED composition/driver, explicit controls and NVS round trips; local HAL only."""
from pathlib import Path
import re
from test_console_lifecycle import RTOS, IDF, body, run_native

LED = Path('components/ts_led/src')
CLI = 'components/ts_console/commands/ts_cmd_led.c'
API = 'components/ts_api/src/ts_api_led.c'


def whole(path):
    source = Path(path).read_text()
    source = re.sub(r'^#include[^\n]*\n|^#define TAG[^\n]*\n|^static const char \*TAG[^\n]*\n', '', source, flags=re.M)
    return '\n#undef TAG\n#define TAG "fixture"\n' + source


code = RTOS + r'''
#include <math.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include "ts_led_private.h"
#include "ts_led_preset.h"
#include "ts_led_image.h"
#include "ts_led_qrcode.h"
#include "ts_led_color_correction.h"
#include "ts_api.h"
#include "ts_led_font.h"
#include "ts_led_text.h"
#include "ts_automation_types.h"
#include "qrcodegen.h"
#define TS_LOGV(...) ((void)0)
#define MALLOC_CAP_DMA 4
#define portTICK_PERIOD_MS 1
#define TS_MALLOC_PSRAM malloc
#define TS_CALLOC_PSRAM calloc
#define LED_NVS_NAMESPACE "led_boot"
typedef struct {ts_led_rgb_t pixels[1024];unsigned frames,lit_frames;} fake_strip;
static int led_strip_set_pixel(void *p,unsigned i,unsigned r,unsigned g,unsigned b) {assert(i<1024);((fake_strip*)p)->pixels[i]=(ts_led_rgb_t){r,g,b};return ESP_OK;}
static int led_strip_refresh(void *p) {fake_strip *s=p;++s->frames;for(int i=0;i<1024;i++)if(s->pixels[i].r||s->pixels[i].g||s->pixels[i].b){++s->lit_frames;break;}return ESP_OK;}
esp_err_t ts_led_driver_init(ts_led_device_impl_t *dev) {dev->strip_handle=calloc(1,sizeof(fake_strip));return dev->strip_handle?ESP_OK:ESP_ERR_NO_MEM;}
void ts_led_driver_deinit(ts_led_device_impl_t *dev) {free(dev->strip_handle);dev->strip_handle=NULL;}
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,unsigned caps) {(void)caps;return xTaskCreate(fn,name,stack,arg,priority,task);}
static void vTaskDeleteWithCaps(TaskHandle_t task) {vTaskDelete(task);}
static uint32_t esp_log_timestamp(void) {return esp_timer_get_time()/1000;}
static uint32_t esp_random(void) {return 12345;}
static ts_led_cc_config_t g_config;
static bool g_initialized=true,g_gamma_lut_valid;
static uint8_t g_gamma_lut[256];
static float g_gamma_lut_value;
esp_err_t ts_led_cc_init(void) {return ESP_OK;}
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
static struct {char key[16];uint8_t value[512];size_t size;} nvs_values[64];
static int nvs_find(const char *key) {for(int i=0;i<64;i++)if(!strcmp(nvs_values[i].key,key))return i;return -1;}
static int nvs_put(const char *key,const void *value,size_t n) {assert(strlen(key)<16&&n<=512);int i=nvs_find(key);if(i<0)for(i=0;i<64&&nvs_values[i].key[0];i++);assert(i<64);strcpy(nvs_values[i].key,key);memcpy(nvs_values[i].value,value,n);nvs_values[i].size=n;return ESP_OK;}
static int nvs_get(const char *key,void *value,size_t *n) {int i=nvs_find(key);if(i<0)return ESP_ERR_NVS_NOT_FOUND;size_t size=nvs_values[i].size;if(value){assert(*n>=size);memcpy(value,nvs_values[i].value,size);}*n=size;return ESP_OK;}
static int nvs_open(const char *name,int mode,nvs_handle_t *out) {assert(!strcmp(name,"led_boot"));(void)mode;*out=1;return ESP_OK;}
static void nvs_close(nvs_handle_t h) {(void)h;}
static int nvs_commit(nvs_handle_t h) {(void)h;return ESP_OK;}
static int nvs_erase_key(nvs_handle_t h,const char *key) {(void)h;int i=nvs_find(key);if(i>=0)memset(&nvs_values[i],0,sizeof(nvs_values[i]));return ESP_OK;}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t n) {(void)h;return nvs_put(key,value,n);}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *value,size_t *n) {(void)h;return nvs_get(key,value,n);}
static int nvs_set_str(nvs_handle_t h,const char *key,const char *value) {return nvs_set_blob(h,key,value,strlen(value)+1);}
static int nvs_get_str(nvs_handle_t h,const char *key,char *value,size_t *n) {return nvs_get_blob(h,key,value,n);}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value) {return nvs_set_blob(h,key,&value,sizeof(value));}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value) {size_t n=sizeof(*value);return nvs_get_blob(h,key,value,&n);}
static int nvs_set_u32(nvs_handle_t h,const char *key,uint32_t value) {return nvs_set_blob(h,key,&value,sizeof(value));}
static int nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *value) {size_t n=sizeof(*value);return nvs_get_blob(h,key,value,&n);}
typedef struct {void (*callback)(void*);void *arg;int dispatch_method;const char *name;} esp_timer_create_args_t;
typedef esp_timer_create_args_t *esp_timer_handle_t;
#define ESP_TIMER_TASK 0
static int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *timer) {*timer=malloc(sizeof(**timer));**timer=*args;return ESP_OK;}
static int esp_timer_start_once(esp_timer_handle_t timer,uint64_t delay) {(void)timer;(void)delay;return ESP_OK;}
static FILE *media_open(const char *path,const char *mode) {return fopen(!strncmp(path,"/sdcard/",8)?path+8:path,mode);}
static int media_stat(const char *path,struct stat *st) {return stat(!strncmp(path,"/sdcard/",8)?path+8:path,st);}
#define fopen media_open
#define stat(path,st) media_stat(path,st)
'''
for name in ['clamp_float','clamp_uint8','init_gamma_lut','apply_gamma_lut','ts_led_cc_rgb_to_hsl','hue_to_rgb','ts_led_cc_hsl_to_rgb','ts_led_cc_get_default_config','ts_led_cc_is_enabled','ts_led_cc_apply_pixel']:
    code += body(LED / 'ts_led_color_correction.c', name)
code += body(LED / 'ts_led_driver.c', 'ts_led_driver_send')
for name in ['ts_led.c','ts_led_layer.c','ts_led_color.c','ts_led_effect.c']:
    code += whole(LED / name)
# Retain the real image representation, BMP/GIF decoder and render ownership.
# JPEG/PNG are unused format boundaries in this normal-content fixture.
image = (LED / 'ts_led_image.c').read_text()
code += image[image.index('struct ts_led_image {'):image.index('/* Forward declarations */')]
code += '\n'.join(line for line in image.splitlines() if line.startswith('#define GIF_') or line.startswith('#define TS_LED_IMAGE_')) + '\n'
for name in ['read_le16','read_le32','gif_skip_sub_blocks']:
    code += body(LED / 'ts_led_image.c',name)
code += image[image.index('typedef struct {',image.index('static esp_err_t load_jpg')):image.index('static int lzw_decode')]
code += r'''
static void gif_animation_effect(ts_led_layer_t,uint32_t,void *);
static ssize_t ts_storage_size(const char *path) {FILE *f=fopen(path,"rb");if(!f)return -1;fseek(f,0,SEEK_END);long n=ftell(f);fclose(f);return n;}
static ssize_t ts_storage_read_file(const char *path,void *out,size_t n) {FILE *f=fopen(path,"rb");if(!f)return -1;size_t got=fread(out,1,n,f);fclose(f);return got;}
static esp_err_t load_png(const uint8_t *d,size_t n,ts_led_image_t *o) {(void)d;(void)n;(void)o;return ESP_ERR_NOT_SUPPORTED;}
static esp_err_t load_jpg(const uint8_t *d,size_t n,ts_led_image_t *o) {(void)d;(void)n;(void)o;return ESP_ERR_NOT_SUPPORTED;}
'''
for name in ['detect_format','load_bmp','lzw_decode','load_gif','ts_led_image_load','ts_led_image_load_mem','ts_led_image_create','ts_led_image_free','ts_led_image_get_info','ts_led_image_get_pixel','get_content_bounds','get_scaled_pixel_bilinear','get_scaled_pixel_area','ts_led_image_display','ts_led_image_display_frame','gif_animation_effect','ts_led_image_animate_start','ts_led_image_release_animation','ts_led_image_animate_stop','ts_led_image_is_playing']:
    code += body(LED / 'ts_led_image.c',name).replace('esp_err_t ts_led_image_load(', 'static esp_err_t production_image_load(',1).replace('esp_err_t ts_led_image_free(', 'static esp_err_t production_image_free(',1)
# Observe image allocations so a simulated reboot can reclaim host heap just as
# RAM loss would, including the preset loader's otherwise inaccessible image.
code += r'''
static ts_led_image_t loaded_images[32];
esp_err_t ts_led_image_load(const char *path,ts_led_image_format_t format,ts_led_image_t *out) {int ret=production_image_load(path,format,out);if(ret==ESP_OK){int i=0;while(i<32&&loaded_images[i])i++;assert(i<32);loaded_images[i]=*out;}return ret;}
esp_err_t ts_led_image_free(ts_led_image_t image) {for(int i=0;i<32;i++)if(loaded_images[i]==image)loaded_images[i]=NULL;return production_image_free(image);}
'''
code += whole(LED / 'ts_led_font.c')
code += whole(LED / 'ts_led_text.c')
code += whole(LED / 'ts_led_animation.c')
code += whole(LED / 'ts_led_qrcode.c')
preset = whole(LED / 'ts_led_preset.c')
# Hardware pin assignment and status events do not participate in boot content.
code += preset[preset.index('static ts_led_device_t s_touch'):preset.index('/* Status indicator colors */')].replace('ts_pin_manager_get_gpio(TS_PIN_FUNC_LED_MATRIX)','9')
code += preset[preset.index('/* 当前运行的动画'):]
code += r'''
static ts_led_image_t s_current_image;
static ts_font_t *s_current_font;
static char s_current_font_name[32];
static const char *ts_console_get_raw_cmdline(void) {return NULL;}
static ts_led_layer_t s_current_image_layer;
static int ts_console_error(const char *format,...) {(void)format;return 0;}
static int ts_console_printf(const char *format,...) {(void)format;return 0;}
static int ts_console_success(const char *format,...) {(void)format;return 0;}
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t code,const char *message) {r->code=code;r->message=(char*)message;}
void ts_api_result_ok(ts_api_result_t *r,cJSON *data) {r->code=TS_API_OK;r->data=data;}
'''
for name in ['resolve_device_name','do_led_on','do_led_off','do_led_effect','do_led_image','do_led_qrcode','parse_text_align','extract_param_from_cmdline','extract_text_from_cmdline','parse_escape_sequences','build_font_path','do_led_draw_text','do_led_clear']:
    code += body(CLI,name)
for name in ['parse_color_param','api_led_fill','api_led_clear','api_led_effect_start','api_led_qrcode']:
    code += body(API,name)
action_header = Path('components/ts_automation/include/ts_action_manager.h').read_text()
code += action_header[action_header.index('typedef enum {'):action_header.index('} ts_action_status_t;')+len('} ts_action_status_t;')]
result_start = action_header.index('typedef struct {',action_header.index('@brief Action execution result'))
code += action_header[result_start:action_header.index('} ts_action_result_t;',result_start)+len('} ts_action_result_t;')]
code += r'''
static struct {SemaphoreHandle_t stats_mutex;struct {unsigned led_actions;} stats;} action_context,*s_ctx=&action_context;
static int ts_action_expand_variables(const char *in,char *out,size_t n) {assert(strlen(in)<n);strcpy(out,in);return strlen(out);}
static bool ts_action_filter_api_params(const ts_auto_filter_params_t *p,cJSON *out) {(void)p;(void)out;assert(false);return false;}
esp_err_t ts_api_call(const char *method,const cJSON *params,ts_api_result_t *result) {(void)method;(void)params;(void)result;assert(false);return ESP_FAIL;}
void ts_api_result_free(ts_api_result_t *result) {cJSON_Delete(result->data);}
int ts_console_exec(const char *command,int *exit_code) {
    (void)exit_code;assert(strstr(command,"led --qrcode --device matrix --text \"B07\""));return do_led_qrcode("matrix","B07",NULL,NULL,NULL);
}
'''
for name in ['action_resolve_led_device_name','require_action_resource','ts_action_exec_led']:
    code += body('components/ts_automation/src/ts_action_manager.c',name)
code += r'''
static void frames(ts_led_device_t device,unsigned count,bool black) {
    ts_led_device_impl_t *dev=device;fake_strip *strip=dev->strip_handle;
    xSemaphoreTake(dev->mutex,portMAX_DELAY);unsigned start=strip->frames,lit=strip->lit_frames;xSemaphoreGive(dev->mutex);
    for(;;){xSemaphoreTake(dev->mutex,portMAX_DELAY);bool done=strip->frames>=start+count;if(black)assert(strip->lit_frames==lit);xSemaphoreGive(dev->mutex);if(done)break;vTaskDelay(1);}
}
static void stop_render(void) {s_led.render_running=false;join_tasks();}
static void start_render(void) {s_led.render_running=true;assert(xTaskCreate(render_task,"render",4096,NULL,1,&s_led.render_task)==pdPASS);}
static void teardown(void) {
    assert(ts_led_text_overlay_stop("matrix")==ESP_OK);stop_render();if(s_current_font){ts_font_unload(s_current_font);s_current_font=NULL;}if(s_current_image){ts_led_image_free(s_current_image);s_current_image=NULL;s_current_image_layer=NULL;}for(int i=0;i<CONFIG_TS_LED_MAX_DEVICES;i++){ts_led_device_impl_t *dev=&s_led.devices[i];if(!dev->used)continue;for(int j=0;j<dev->layer_count;j++){ts_led_animation_stop(dev->layers[j]);assert(ts_led_layer_destroy(dev->layers[j])==ESP_OK);}assert(ts_led_device_destroy(dev)==ESP_OK);}
    for(int i=0;i<32;i++)if(loaded_images[i])assert(ts_led_image_free(loaded_images[i])==ESP_OK);vSemaphoreDelete(s_led.mutex);memset(&s_led,0,sizeof(s_led));memset(s_overlays,0,sizeof(s_overlays));s_touch=s_board=s_matrix=NULL;s_status_layer=NULL;memset(s_current_font_name,0,sizeof(s_current_font_name));free(s_delayed_image_timer);s_delayed_image_timer=NULL;s_delayed_brightness=128;
    free(s_anim_state);s_anim_state=NULL;memset(s_current_animation,0,sizeof(s_current_animation));memset(s_current_filter,0,sizeof(s_current_filter));memset(s_current_speed,0,sizeof(s_current_speed));memset(s_current_color,0,sizeof(s_current_color));memset(s_current_color_valid,0,sizeof(s_current_color_valid));memset(s_current_image_path,0,sizeof(s_current_image_path));memset(s_current_qrcode,0,sizeof(s_current_qrcode));memset(s_current_qrcode_bg,0,sizeof(s_current_qrcode_bg));memset(s_current_text,0,sizeof(s_current_text));memset(s_current_filter_config,0,sizeof(s_current_filter_config));memset(s_current_filter_speed,50,sizeof(s_current_filter_speed));memset(s_delayed_image_path,0,sizeof(s_delayed_image_path));s_delayed_device_idx=-1;
}
static void fresh(void) {assert(ts_led_init()==ESP_OK);assert(ts_led_preset_init_all()==ESP_OK);}
static void api(esp_err_t (*fn)(const cJSON*,ts_api_result_t*),const char *json) {cJSON *p=cJSON_Parse(json);assert(p);ts_api_result_t r={0};int ret=fn(p,&r);assert(ret==ESP_OK&&r.code==TS_API_OK);cJSON_Delete(r.data);cJSON_Delete(p);}
static void api_failure(esp_err_t (*fn)(const cJSON*,ts_api_result_t*),const char *json) {cJSON *p=cJSON_Parse(json);assert(p);ts_api_result_t r={0};assert(fn(p,&r)!=ESP_OK&&r.code!=TS_API_OK);cJSON_Delete(r.data);cJSON_Delete(p);}
static void assert_choice(const char *device,const char *animation,bool enabled) {ts_led_boot_config_t state;assert(ts_led_get_current_state(device,&state)==ESP_OK);assert(!strcmp(state.animation,animation)&&state.enabled==enabled);}
static const uint8_t gif_fixture[]={71,73,70,56,57,97,2,0,2,0,128,0,0,255,0,0,0,0,255,33,249,4,0,10,0,0,0,44,0,0,0,0,2,0,2,0,0,2,4,4,65,16,5,0,33,249,4,0,10,0,0,0,44,0,0,0,0,2,0,2,0,0,2,4,12,195,48,5,0,59};
static void make_font(void) {FILE *f=fopen("fonts/normal.fnt","wb");assert(f);ts_font_header_t h={.magic={'T','F','N','T'},.version=1,.width=3,.height=5,.glyph_count=1,.index_offset=sizeof(h)};ts_font_index_entry_t index={.codepoint='A',.offset=sizeof(h)+sizeof(index)};uint8_t pixels[2]={0xff,0xff};fwrite(&h,sizeof(h),1,f);fwrite(&index,sizeof(index),1,f);fwrite(pixels,sizeof(pixels),1,f);fclose(f);}
static void make_bmp(void) {uint8_t bmp[70]={0};bmp[0]='B';bmp[1]='M';bmp[2]=70;bmp[10]=54;bmp[14]=40;bmp[18]=2;bmp[22]=2;bmp[26]=1;bmp[28]=24;for(int i=54;i<70;i++)bmp[i]=i%3==0?255:0;FILE *f=fopen("/sdcard/images/normal.bmp","wb");assert(f);assert(fwrite(bmp,1,sizeof(bmp),f)==sizeof(bmp));fclose(f);}
static void custom_animation(ts_led_layer_t layer,uint32_t time,void *data) {(void)time;assert(data);ts_led_fill(layer,TS_LED_GREEN);}
int main(void) {
    mkdir("images",0700);mkdir("fonts",0700);make_bmp();make_font();FILE *gif_file=fopen("images/normal.gif","wb");assert(gif_file);fwrite(gif_fixture,sizeof(gif_fixture),1,gif_file);fclose(gif_file);action_context.stats_mutex=xSemaphoreCreateMutex();assert(ts_led_cc_get_default_config(&g_config)==ESP_OK);g_config.enabled=true;g_config.white_point.enabled=true;g_config.white_point.red_scale=0.75f;g_config.gamma.enabled=true;g_config.gamma.gamma=1.4f;g_config.saturation.enabled=true;g_config.saturation.factor=0.7f;
    fresh();ts_led_layer_t layers[3]={ts_led_layer_get(s_touch,0),ts_led_layer_get(s_board,0),ts_led_layer_get(s_matrix,0)};
    stop_render();assert(do_led_effect("touch","breathing",50,"red")==0);assert(do_led_effect("board","breathing",50,"blue")==0);assert(do_led_effect("matrix","breathing",50,"green")==0);
    for(int i=0;i<3;i++){ts_led_layer_impl_t *l=layers[i];assert(l->anim_data==&l->anim_color);l->anim_fn(l,500,l->anim_data);assert((l->buffer[0].r>0)==(i==0)&&(l->buffer[0].b>0)==(i==1)&&(l->buffer[0].g>0)==(i==2));}
    assert(do_led_effect("board","solid",50,"white")==0);assert(((ts_led_layer_impl_t*)layers[0])->anim_color.r==255&&((ts_led_layer_impl_t*)layers[2])->anim_color.g==255);
    ts_led_animation_def_t def=*ts_led_animation_get_builtin("solid");assert(!def.user_data);assert(ts_led_animation_start(layers[0],&def)==ESP_OK);assert(!((ts_led_layer_impl_t*)layers[0])->anim_data);
    for(int i=0;i<3;i++){const char *names[]={"breathing","solid","rain"};def=*ts_led_animation_get_builtin(names[i]);assert(!def.user_data&&ts_led_animation_start(layers[2],&def)==ESP_OK&&!((ts_led_layer_impl_t*)layers[2])->anim_data);ts_led_rgb_t borrowed={19,37,59};def.user_data=&borrowed;assert(ts_led_animation_start(layers[2],&def)==ESP_OK);borrowed=(ts_led_rgb_t){0};assert(((ts_led_layer_impl_t*)layers[2])->anim_color.r==19&&((ts_led_layer_impl_t*)layers[2])->anim_color.g==37&&((ts_led_layer_impl_t*)layers[2])->anim_color.b==59);}
    api(api_led_effect_start,"{\"device\":\"touch\",\"effect\":\"solid\",\"color\":\"red\"}");api(api_led_effect_start,"{\"device\":\"board\",\"effect\":\"solid\",\"color\":\"blue\"}");api(api_led_effect_start,"{\"device\":\"matrix\",\"effect\":\"solid\",\"color\":\"green\"}");assert(((ts_led_layer_impl_t*)layers[0])->anim_color.r==255&&((ts_led_layer_impl_t*)layers[1])->anim_color.b==255&&((ts_led_layer_impl_t*)layers[2])->anim_color.g==255);
    int custom=7;def.func=custom_animation;def.user_data=&custom;assert(ts_led_animation_start(layers[0],&def)==ESP_OK&&((ts_led_layer_impl_t*)layers[0])->anim_data==&custom);assert(ts_led_animation_stop(layers[0])==ESP_OK&&custom==7);
    assert(do_led_off("touch")==0&&do_led_on("touch","red")==0);start_render();frames(s_touch,3,false);assert(((fake_strip*)((ts_led_device_impl_t*)s_touch)->strip_handle)->pixels[0].r>0);
    assert(do_led_draw_text("matrix","A","normal","green","left",NULL,"left",1,2,true,false,50)==0);ts_led_layer_t overlay=s_overlays[0].overlay_layer;assert(ts_led_text_overlay_is_active("matrix"));
    ts_led_effect_config_t invert={.type=TS_LED_EFFECT_INVERT};assert(ts_led_layer_set_effect(layers[2],&invert)==ESP_OK);
    api(api_led_clear,"{\"device\":\"matrix\"}");assert(((ts_led_layer_impl_t*)overlay)->visible);frames(s_matrix,3,false);assert(((fake_strip*)((ts_led_device_impl_t*)s_matrix)->strip_handle)->lit_frames>0);
    api(api_led_clear,"{\"device\":\"matrix\",\"off\":true}");assert(ts_led_device_set_brightness(s_matrix,201)==ESP_OK);frames(s_matrix,4,true);api_failure(api_led_fill,"{\"device\":\"matrix\",\"color\":\"invalid\"}");api_failure(api_led_effect_start,"{\"device\":\"matrix\",\"effect\":\"missing\"}");api_failure(api_led_qrcode,"{\"device\":\"matrix\"}");assert(do_led_effect("matrix","missing",50,NULL)==1);frames(s_matrix,3,true);frames(s_touch,3,false);assert(ts_led_device_is_enabled(s_touch));assert(((ts_led_layer_impl_t*)overlay)->visible);
    api(api_led_fill,"{\"device\":\"matrix\",\"color\":\"red\"}");assert(ts_led_device_is_enabled(s_matrix));frames(s_matrix,3,false);
    api(api_led_effect_start,"{\"device\":\"matrix\",\"effect\":\"solid\",\"color\":\"blue\"}");
    api(api_led_qrcode,"{\"device\":\"matrix\",\"text\":\"B07\"}");assert(!ts_led_animation_is_running(layers[2])&&((ts_led_layer_impl_t*)overlay)->visible);frames(s_matrix,3,false);
    assert(do_led_effect("matrix","solid",50,"red")==0);assert(do_led_qrcode("matrix","B07","M","red",NULL)==0);assert(!ts_led_animation_is_running(layers[2]));
    ts_led_image_t movie=NULL;assert(ts_led_image_load("/sdcard/images/normal.gif",TS_LED_IMG_FMT_AUTO,&movie)==ESP_OK);assert(ts_led_image_animate_start(layers[2],movie,NULL)==ESP_OK&&ts_led_image_is_playing(layers[2]));ts_led_qr_config_t qr=TS_LED_QR_DEFAULT_CONFIG();qr.text="B07";assert(ts_led_qrcode_show(layers[2],&qr,NULL)==ESP_OK&&!ts_led_image_is_playing(layers[2])&&!ts_led_animation_is_running(layers[2]));assert(ts_led_image_free(movie)==ESP_OK);ts_led_rgb_t qr_pixels[1024];memcpy(qr_pixels,((ts_led_layer_impl_t*)layers[2])->buffer,sizeof(qr_pixels));frames(s_matrix,4,false);assert(!memcmp(qr_pixels,((ts_led_layer_impl_t*)layers[2])->buffer,sizeof(qr_pixels))&&ts_led_text_overlay_is_active("matrix"));
    assert(do_led_effect("matrix","solid",50,"red")==0);qr.text=NULL;assert(ts_led_qrcode_show(layers[2],&qr,NULL)==ESP_ERR_INVALID_ARG&&ts_led_animation_is_running(layers[2]));
    ts_auto_action_led_t action={.ctrl_type=TS_LED_CTRL_QRCODE};strcpy(action.device,"matrix");strcpy(action.qr_text,"B07");ts_action_result_t result={0};assert(do_led_effect("matrix","solid",50,"blue")==0);assert(ts_action_exec_led(&action,&result)==ESP_OK&&!ts_led_animation_is_running(layers[2])&&ts_led_text_overlay_is_active("matrix"));
    action.ctrl_type=TS_LED_CTRL_FILL;strcpy(action.device,"board");action.r=77;action.g=33;action.b=11;action.index=0xff;assert(ts_action_exec_led(&action,&result)==ESP_OK);action.index=3;action.r=9;action.g=88;action.b=5;assert(ts_action_exec_led(&action,&result)==ESP_OK);frames(s_board,3,false);assert(((ts_led_layer_impl_t*)layers[1])->buffer[3].g==88&&((ts_led_layer_impl_t*)layers[1])->buffer[2].r==77);assert(((fake_strip*)((ts_led_device_impl_t*)s_board)->strip_handle)->pixels[3].g>0);
    strcpy(action.effect,"breathing");assert(ts_action_exec_led(&action,&result)==ESP_OK&&ts_led_animation_is_running(layers[1]));action.ctrl_type=TS_LED_CTRL_OFF;assert(ts_action_exec_led(&action,&result)==ESP_OK);action.ctrl_type=TS_LED_CTRL_BRIGHTNESS;action.brightness=222;assert(ts_action_exec_led(&action,&result)==ESP_OK);frames(s_board,4,true);
    /* Old image/QR choice cannot reappear after an explicit solid save. */
    assert(do_led_image("matrix","/sdcard/images/normal.bmp",NULL)==0);api(api_led_fill,"{\"device\":\"matrix\",\"color\":\"red\"}");assert(ts_led_save_boot_config("matrix")==ESP_OK);assert(nvs_find("led.mat.img")<0&&nvs_find("led.mat.qr")<0);
    teardown();fresh();assert(ts_led_load_boot_config("matrix")==ESP_OK);assert_choice("matrix","solid",true);frames(s_matrix,3,false);
    assert(do_led_effect("matrix","breathing",73,"blue")==0);ts_led_effect_config_t filter={.type=TS_LED_EFFECT_PULSE,.params.pulse={.frequency=0.7f,.min_level=0,.max_level=111}};assert(ts_led_layer_set_effect(ts_led_layer_get(s_matrix,0),&filter)==ESP_OK);ts_led_preset_set_current_filter("matrix","pulse",29);ts_led_preset_set_current_filter_config("matrix",&filter);
    assert(do_led_off("matrix")==0);assert(ts_led_device_set_brightness(s_matrix,187)==ESP_OK);assert(ts_led_save_boot_config("matrix")==ESP_OK);
    assert(do_led_on("matrix","green")==0); /* unsaved changes must disappear */
    teardown();fresh();assert(ts_led_load_boot_config("matrix")==ESP_OK);assert_choice("matrix","breathing",false);assert(ts_led_device_get_brightness(s_matrix)==187);frames(s_matrix,5,true);ts_led_boot_config_t state;assert(ts_led_get_current_state("matrix",&state)==ESP_OK&&state.speed==73&&state.filter_speed==29&&state.color.b==255);ts_led_effect_config_t restored;restored=((ts_led_layer_impl_t*)ts_led_layer_get(s_matrix,0))->post_effect;assert(!memcmp(&restored,&filter,sizeof(filter)));assert(ts_led_get_boot_config("matrix",&state)==ESP_OK&&!state.enabled);assert(ts_led_load_boot_config("touch")==ESP_ERR_NOT_FOUND&&ts_led_device_is_enabled(s_touch));
    /* Image and QR selection survive saved off and brightness edits, from RAM loss. */
    assert(do_led_image("matrix","/sdcard/images/normal.bmp",NULL)==0);assert(do_led_off("matrix")==0);assert(ts_led_device_set_brightness(s_matrix,163)==ESP_OK);assert(ts_led_save_boot_config("matrix")==ESP_OK);
    teardown();fresh();assert(ts_led_load_boot_config("matrix")==ESP_OK);frames(s_matrix,4,true);assert(ts_led_get_current_state("matrix",&state)==ESP_OK&&!state.enabled&&!strcmp(state.image_path,"/sdcard/images/normal.bmp")&&!state.animation[0]&&!state.qrcode_text[0]&&state.brightness==163&&state.filter_speed==29);assert(!memcmp(&((ts_led_layer_impl_t*)ts_led_layer_get(s_matrix,0))->post_effect,&filter,sizeof(filter)));
    /* Delayed image restore is also a low-level operation; its callback cannot open output. */
    assert(rename("images/normal.bmp","images/not-ready.bmp")==0);teardown();fresh();assert(ts_led_load_boot_config("matrix")==ESP_OK&&s_delayed_device_idx==2);assert(rename("images/not-ready.bmp","images/normal.bmp")==0);assert(s_delayed_image_timer);s_delayed_image_timer->callback(s_delayed_image_timer->arg);frames(s_matrix,4,true);assert(ts_led_get_current_state("matrix",&state)==ESP_OK&&!state.enabled&&!strcmp(state.image_path,"/sdcard/images/normal.bmp"));
    assert(do_led_qrcode("matrix","B07","M",NULL,NULL)==0);assert(do_led_off("matrix")==0);assert(ts_led_device_set_brightness(s_matrix,149)==ESP_OK);assert(ts_led_save_boot_config("matrix")==ESP_OK);
    teardown();fresh();assert(ts_led_load_boot_config("matrix")==ESP_OK);frames(s_matrix,4,true);assert(ts_led_get_current_state("matrix",&state)==ESP_OK&&!state.enabled&&!strcmp(state.qrcode_text,"B07")&&!state.image_path[0]&&!state.animation[0]&&state.brightness==149&&state.filter_speed==29);
    assert(do_led_effect("matrix","solid",50,"red")==0);assert(ts_led_save_boot_config("matrix")==ESP_OK&&nvs_find("led.mat.clr")>=0);assert(do_led_effect("matrix","solid",50,NULL)==0);assert(ts_led_save_boot_config("matrix")==ESP_OK&&nvs_find("led.mat.clr")<0);
    teardown();fresh();assert(ts_led_load_boot_config("matrix")==ESP_OK);assert(!((ts_led_layer_impl_t*)ts_led_layer_get(s_matrix,0))->anim_data);
    teardown();free(s_delayed_image_timer);s_delayed_image_timer=NULL;vSemaphoreDelete(action_context.stats_mutex);free(s_anim_state);puts("PASS actual LED controls, independent RGB ownership, composition/CC/driver HAL output gate, QR layer handoff and NVS save/reset/rebuild/load");
}
'''
headers = {path.name: path.read_text() for path in Path('components/ts_led/include').glob('*.h')}
headers['ts_led_private.h'] = (LED / 'ts_led_private.h').read_text()
headers['ts_automation_types.h'] = Path('components/ts_automation/include/ts_automation_types.h').read_text()
headers['qrcodegen.h'] = (LED / 'qrcodegen/qrcodegen.h').read_text()
headers.update({'led_strip.h':'#pragma once\ntypedef void *led_strip_handle_t;\n','freertos/FreeRTOS.h':'#pragma once\n','freertos/semphr.h':'#pragma once\n'})
run_native(code,[IDF/'components/json/cJSON/cJSON.c',LED/'qrcodegen/qrcodegen.c'],headers)
