"""Production font/overlay and GIF/render ownership, with real temporary media and local hardware stubs."""
from pathlib import Path
import re
from test_console_lifecycle import RTOS, IDF, body, run_native

LED = Path('components/ts_led/src')
CLI = 'components/ts_console/commands/ts_cmd_led.c'
API = 'components/ts_api/src/ts_api_led.c'


def whole(path):
    text = Path(path).read_text()
    text = re.sub(r'^#include[^\n]*\n|^#define TAG[^\n]*\n|^static const char \*TAG[^\n]*\n', '', text, flags=re.M)
    return '\n#undef TAG\n#define TAG "fixture"\n' + text


# A valid two-frame, 2x2 GIF. Clear before each pixel keeps the fixture's
# three-bit LZW codes constant; the decoder under test is the production one.
gif = bytearray(b'GIF89a\x02\x00\x02\x00\x80\x00\x00\xff\x00\x00\x00\x00\xff')
for color in [0, 1]:
    codes = [4, color, 4, color, 4, color, 4, color, 5]
    packed = sum(code << (3 * index) for index, code in enumerate(codes)).to_bytes(4, 'little')
    gif += b'\x21\xf9\x04\x00\x0a\x00\x00\x00\x2c\x00\x00\x00\x00\x02\x00\x02\x00\x00\x02\x04' + packed + b'\x00'
gif += b'\x3b'

rtos = RTOS.replace('static _Thread_local TaskHandle_t current_task;',
                    'static SemaphoreHandle_t watched_mutex;static atomic_int mutex_waits;\nstatic _Thread_local TaskHandle_t current_task;')
rtos = rtos.replace('while(!s->value){', 'while(!s->value){if(s==watched_mutex)++mutex_waits;')
code = rtos + r'''
#include <ctype.h>
#include <strings.h>
#include <math.h>
#include <sys/stat.h>
#include "ts_led_private.h"
#include "ts_led_image.h"
#include "ts_led_font.h"
#include "ts_led_text.h"
#include "ts_api.h"
#define TS_LOGV(...) ((void)0)
#define MALLOC_CAP_DMA 4
#define portTICK_PERIOD_MS 1
#define TS_MALLOC_PSRAM malloc
#define TS_CALLOC_PSRAM calloc
static atomic_int caps_created,caps_deleted,lit_frames,glyph_reads,gif_frames;
static atomic_bool hold_glyph,hold_frame,operation_done,operation_started;
static SemaphoreHandle_t glyph_entered,glyph_release,frame_entered,frame_release;
static int operation_result;
static void overlay_render_task(void *);
static void render_task(void *);
static struct {void *pointer;atomic_int frees;} watched[32];static int watched_count;
static void *builtin_color_pointer;
static int watch(void *pointer) {assert(pointer&&watched_count<32);int i=watched_count++;watched[i].pointer=pointer;return i;}
static void tracked_free(void *pointer) {
    assert(!pointer||pointer!=builtin_color_pointer);
    for(int i=0;i<watched_count;i++)if(pointer==watched[i].pointer)assert(atomic_fetch_add(&watched[i].frees,1)==0);
    free(pointer);
}
static FILE *media_open(const char *path,const char *mode) {return fopen(!strncmp(path,"/sdcard/",8)?path+8:path,mode);}
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,unsigned caps) {
    (void)caps;int result=xTaskCreate(fn,name,stack,arg,priority,task);if(result==pdPASS)++caps_created;return result;
}
static void vTaskDeleteWithCaps(TaskHandle_t task) {++caps_deleted;vTaskDelete(task);}
static int ts_led_cc_init(void) {return ESP_OK;}
esp_err_t ts_led_driver_init(ts_led_device_impl_t *dev) {(void)dev;return ESP_OK;}
void ts_led_driver_deinit(ts_led_device_impl_t *dev) {(void)dev;}
esp_err_t ts_led_driver_send(ts_led_device_impl_t *dev) {for(int i=0;i<dev->config.led_count;i++)if(dev->framebuffer[i].r||dev->framebuffer[i].g||dev->framebuffer[i].b){++lit_frames;break;}return ESP_OK;}
void ts_led_effect_process(ts_led_layer_t layer,ts_led_rgb_t *pixels,size_t count,uint16_t width,uint16_t height,uint32_t time) {(void)layer;(void)pixels;(void)count;(void)width;(void)height;(void)time;}
#define free tracked_free
#define fopen media_open
'''
code += whole(LED / 'ts_led.c')
code += whole(LED / 'ts_led_layer.c')
code += whole(LED / 'ts_led_color.c')
font = whole(LED / 'ts_led_font.c').replace('esp_err_t ts_font_get_glyph(', 'static esp_err_t production_get_glyph(', 1)
code += font
code += r'''
esp_err_t ts_font_get_glyph(ts_font_t *font,uint16_t point,const uint8_t **bitmap) {
    if(current_task&&current_task->fn==overlay_render_task&&atomic_exchange(&hold_glyph,false)){
        xSemaphoreGive(glyph_entered);assert(xSemaphoreTake(glyph_release,portMAX_DELAY));
    }
    int result=production_get_glyph(font,point,bitmap);if(current_task&&current_task->fn==overlay_render_task)++glyph_reads;return result;
}
'''
code += whole(LED / 'ts_led_text.c')
# Use the real image representation, GIF/BMP decode and display pipeline. JPEG
# and PNG loading are explicit unused boundaries in this lifecycle fixture.
image = (LED / 'ts_led_image.c').read_text()
start = image.index('struct ts_led_image {')
end = image.index('/* Forward declarations */', start)
code += image[start:end]
code += '\n'.join(line for line in image.splitlines() if line.startswith('#define GIF_') or line.startswith('#define TS_LED_IMAGE_'))+'\n'
code += body(LED / 'ts_led_image.c','read_le16')
gif_start=image.index('typedef struct {',image.index('static esp_err_t load_jpg'))
code += image[gif_start:image.index('static int lzw_decode')]
code += r'''
static void gif_animation_effect(ts_led_layer_t,uint32_t,void *);
static ssize_t ts_storage_size(const char *path) {FILE *file=fopen(path,"rb");if(!file)return -1;fseek(file,0,SEEK_END);long n=ftell(file);fclose(file);return n;}
static ssize_t ts_storage_read_file(const char *path,void *out,size_t n) {FILE *file=fopen(path,"rb");if(!file)return -1;size_t got=fread(out,1,n,file);fclose(file);return got;}
static esp_err_t load_png(const uint8_t *data,size_t n,ts_led_image_t *out) {(void)data;(void)n;(void)out;return ESP_ERR_NOT_SUPPORTED;}
static esp_err_t load_jpg(const uint8_t *data,size_t n,ts_led_image_t *out) {(void)data;(void)n;(void)out;return ESP_ERR_NOT_SUPPORTED;}
'''
for name in ['detect_format', 'load_bmp', 'lzw_decode', 'load_gif', 'ts_led_image_load',
             'ts_led_image_load_mem', 'ts_led_image_create', 'ts_led_image_free', 'ts_led_image_get_info',
             'ts_led_image_get_pixel', 'get_content_bounds', 'get_scaled_pixel_bilinear', 'get_scaled_pixel_area',
             'ts_led_image_display', 'ts_led_image_display_frame']:
    code += body(LED / 'ts_led_image.c', name).replace('esp_err_t ts_led_image_display_frame(', 'static esp_err_t production_display_frame(', 1)
code += r'''
esp_err_t ts_led_image_display_frame(ts_led_layer_t layer,ts_led_image_t image,uint16_t frame,const ts_led_image_options_t *options) {
    if(current_task&&current_task->fn==render_task&&atomic_exchange(&hold_frame,false)){
        xSemaphoreGive(frame_entered);assert(xSemaphoreTake(frame_release,portMAX_DELAY));
    }
    int result=production_display_frame(layer,image,frame,options);if(current_task&&current_task->fn==render_task)++gif_frames;return result;
}
'''
for name in ['gif_animation_effect', 'ts_led_image_animate_start', 'ts_led_image_release_animation',
             'ts_led_image_animate_stop', 'ts_led_image_is_playing']:
    code += body(LED / 'ts_led_image.c', name)
for name in ['ts_led_animation_start', 'ts_led_animation_stop', 'ts_led_animation_is_running',
             'ts_led_animation_get_state', 'anim_solid', 'anim_breathing']:
    code += body(LED / 'ts_led_animation.c', name)
code += r'''
static ts_led_image_t s_current_image,s_api_current_image;
static ts_led_layer_t s_current_image_layer;
static ts_font_t *s_current_font,*s_api_current_font;
static char s_current_font_name[32],s_api_font_name[32];
static int ts_console_error(const char *format,...) {(void)format;return 0;}
static int ts_console_printf(const char *format,...) {(void)format;return 0;}
static int ts_console_success(const char *format,...) {(void)format;return 0;}
static const char *ts_console_get_raw_cmdline(void) {return NULL;}
static void ts_led_preset_set_current_effect(const char *device,const char *effect,int speed) {(void)device;(void)effect;(void)speed;}
static void ts_led_preset_set_current_qrcode(const char *device,const char *text) {(void)device;(void)text;}
static void ts_led_preset_clear_current_image(const char *name) {(void)name;}
static void ts_led_preset_set_current_image(const char *name,const char *path) {(void)name;(void)path;}
void ts_api_result_error(ts_api_result_t *result,ts_api_result_code_t code,const char *message) {result->code=code;result->message=(char *)message;}
void ts_api_result_ok(ts_api_result_t *result,cJSON *data) {result->code=TS_API_OK;result->data=data;}
'''
for name in ['resolve_device_name', 'parse_text_align', 'extract_param_from_cmdline',
             'extract_text_from_cmdline', 'parse_escape_sequences', 'build_font_path', 'do_led_draw_text',
             'do_led_clear', 'do_led_off', 'do_led_image']:
    code += body(CLI, name)
for name in ['parse_color_param', 'api_led_text', 'api_led_image']:
    code += body(API, name)
code += 'static const unsigned char gif_fixture[]={' + ','.join(map(str, gif)) + '};\n'
code += r'''
static void make_font(const char *path,unsigned char glyph) {
    FILE *file=fopen(path,"wb");assert(file);ts_font_header_t header={.magic={'T','F','N','T'},.version=1,.width=3,.height=5,.glyph_count=2,.index_offset=sizeof(header)};
    ts_font_index_entry_t index[]={{.codepoint='A',.offset=sizeof(header)+2*sizeof(ts_font_index_entry_t)},{.codepoint=0x4e2d,.offset=sizeof(header)+2*sizeof(ts_font_index_entry_t)+2}};
    unsigned char pixels[]={glyph,glyph,glyph,glyph};fwrite(&header,sizeof(header),1,file);fwrite(index,sizeof(index),1,file);fwrite(pixels,sizeof(pixels),1,file);fclose(file);
}
static int api_text(const char *font) {
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"text","A中");cJSON_AddStringToObject(params,"font",font);
    ts_api_result_t result={0};int ret=api_led_text(params,&result);cJSON_Delete(params);cJSON_Delete(result.data);return ret;
}
static int api_image(void) {
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"path","/sdcard/images/movie.gif");
    ts_api_result_t result={0};int ret=api_led_image(params,&result);cJSON_Delete(params);cJSON_Delete(result.data);return ret;
}
static int scroll_position(void) {xSemaphoreTake(s_led.mutex,portMAX_DELAY);int x=s_overlays[0].scroll_x;xSemaphoreGive(s_led.mutex);return x;}
static int cli_text(const char *font) {return do_led_draw_text("matrix","A中",font,"red","left",NULL,"none",1,2,false,false,30);}
static void *font_request(void *mode) {operation_started=true;operation_result=mode?cli_text("missing"):api_text("missing");operation_done=true;return NULL;}
static void font_failure(bool cli) {
    assert(ts_led_text_overlay_stop("matrix")==ESP_OK);
    hold_glyph=true;assert((cli?cli_text("C"):api_text("A"))==ESP_OK);assert(xSemaphoreTake(glyph_entered,portMAX_DELAY));
    ts_font_t *font=cli?s_current_font:s_api_current_font;int resource=watch(font);watched_mutex=s_led.mutex;int before=mutex_waits;
    operation_done=operation_started=false;pthread_t request;assert(!pthread_create(&request,NULL,font_request,cli?(void *)1:NULL));
    WAIT_UNTIL(mutex_waits>before||operation_done);if(operation_done)fprintf(stderr,"Font ownership failure: %d frees while glyph reader is paused\n",(int)watched[resource].frees);assert(!operation_done&&watched[resource].frees==0);
    xSemaphoreGive(glyph_release);assert(!pthread_join(request,NULL));assert(operation_result==(cli?1:ESP_ERR_NOT_FOUND)&&watched[resource].frees==1);
    assert(!ts_led_text_overlay_is_active("matrix"));assert((cli?cli_text("B"):api_text("B"))==ESP_OK);int frames=lit_frames;WAIT_UNTIL(lit_frames>frames);
}
static void *image_request(void *replace) {operation_started=true;operation_result=replace?do_led_image("matrix","/sdcard/images/movie.gif",NULL):do_led_clear("matrix");operation_done=true;return NULL;}
static void image_handoff(bool replace,ts_led_layer_t layer) {
    int image_resource=watch(s_current_image),ctx_resource=watch(((ts_led_layer_impl_t *)layer)->anim_data);
    hold_frame=true;assert(xSemaphoreTake(frame_entered,portMAX_DELAY));watched_mutex=((ts_led_layer_impl_t *)layer)->device->mutex;int before=mutex_waits;
    operation_done=operation_started=false;pthread_t request;assert(!pthread_create(&request,NULL,image_request,replace?(void *)1:NULL));
    WAIT_UNTIL(mutex_waits>before||operation_done);assert(!operation_done&&watched[image_resource].frees==0&&watched[ctx_resource].frees==0);
    xSemaphoreGive(frame_release);assert(!pthread_join(request,NULL));assert(operation_result==0);
    assert(watched[image_resource].frees==1&&watched[ctx_resource].frees==1);
    assert(ts_led_image_is_playing(layer)==replace);
}
int main(void) {
    mkdir("fonts",0700);mkdir("images",0700);make_font("/sdcard/fonts/A.fnt",0xf7);make_font("/sdcard/fonts/B.fnt",0xab);make_font("/sdcard/fonts/C.fnt",0xf7);make_font("/sdcard/fonts/cjk.fnt",0xf7);
    FILE *file=fopen("/sdcard/images/movie.gif","wb");assert(file);fwrite(gif_fixture,sizeof(gif_fixture),1,file);fclose(file);
    glyph_entered=xSemaphoreCreateBinary();glyph_release=xSemaphoreCreateBinary();frame_entered=xSemaphoreCreateBinary();frame_release=xSemaphoreCreateBinary();
    assert(ts_led_init()==ESP_OK);ts_led_device_t devices[3];const char *names[]={"led_matrix","led_touch","led_board"};
    for(int i=0;i<3;i++){ts_led_config_t config=TS_LED_DEFAULT_CONFIG();config.name=names[i];config.led_count=64;config.width=config.height=8;config.layout=TS_LED_LAYOUT_MATRIX;assert(ts_led_device_create(&config,&devices[i])==ESP_OK);}
    ts_led_layer_t matrix=ts_led_layer_get(devices[0],0),touch=ts_led_layer_get(devices[1],0),board=ts_led_layer_get(devices[2],0);
    assert(api_text("default")==ESP_OK&&!strcmp(s_api_font_name,"cjk"));
    font_failure(false);font_failure(true);
    /* A cached API font is not necessarily the font actually displayed by CLI. */
    assert(cli_text("C")==0);ts_font_t *cli_font=s_current_font;assert(api_text("missing")==ESP_ERR_NOT_FOUND);
    assert(ts_led_text_overlay_is_active("matrix"));xSemaphoreTake(s_led.mutex,portMAX_DELAY);assert(s_overlays[0].font==cli_font);xSemaphoreGive(s_led.mutex);
    assert(ts_led_text_overlay_update("matrix","中A")==ESP_OK);assert(ts_led_text_overlay_set_position("matrix",2,3)==ESP_OK);
    /* Existing alignment, manual coordinates, and scroll still use the real renderer. */
    cJSON *text=cJSON_CreateObject();cJSON_AddStringToObject(text,"text","A");cJSON_AddStringToObject(text,"font","B");cJSON_AddStringToObject(text,"align","center");
    ts_api_result_t result={0};assert(api_led_text(text,&result)==ESP_OK);cJSON_Delete(result.data);int reads=glyph_reads;WAIT_UNTIL(glyph_reads>reads+1);
    xSemaphoreTake(s_led.mutex,portMAX_DELAY);ts_led_layer_impl_t *overlay=s_overlays[0].overlay_layer;assert(overlay->buffer[2].r==255&&overlay->buffer[0].r==0);xSemaphoreGive(s_led.mutex);
    cJSON_AddStringToObject(text,"scroll","left");cJSON_AddBoolToObject(text,"loop",true);cJSON_AddNumberToObject(text,"speed",100);assert(api_led_text(text,&result)==ESP_OK);cJSON_Delete(result.data);cJSON_Delete(text);WAIT_UNTIL(scroll_position()<8);
    /* GIF on matrix survives off/clear of other devices and still advances frames. */
    assert(do_led_image("matrix","/sdcard/images/movie.gif",NULL)==0);assert(s_current_image->frame_count==2);
    int image_resource=watch(s_current_image),ctx_resource=watch(((ts_led_layer_impl_t *)matrix)->anim_data);
    hold_frame=true;assert(xSemaphoreTake(frame_entered,portMAX_DELAY));assert(do_led_off("touch")==0&&do_led_clear("board")==0);
    if(watched[image_resource].frees||watched[ctx_resource].frees)fprintf(stderr,"Cross-layer ownership failure: image frees=%d context frees=%d while matrix frame is paused\n",(int)watched[image_resource].frees,(int)watched[ctx_resource].frees);
    assert(watched[image_resource].frees==0&&watched[ctx_resource].frees==0);int frames=gif_frames;xSemaphoreGive(frame_release);WAIT_UNTIL(gif_frames>frames);assert(ts_led_image_is_playing(matrix));
    /* Each layer owns its own GIF context, as boot restoration already expects. */
    ts_led_image_t other;assert(ts_led_image_load("/sdcard/images/movie.gif",TS_LED_IMG_FMT_AUTO,&other)==ESP_OK);assert(ts_led_image_animate_start(touch,other,NULL)==ESP_OK);
    assert(ts_led_image_is_playing(touch)&&ts_led_image_is_playing(matrix));assert(ts_led_image_animate_stop(touch)==ESP_OK);assert(ts_led_image_free(other)==ESP_OK);assert(ts_led_image_is_playing(matrix));
    /* Ordinary animations keep their caller-owned color through replacement and stop. */
    ts_led_rgb_t color={25,40,55};builtin_color_pointer=&color;ts_led_animation_def_t animation={.func=anim_solid,.user_data=&color,.frame_interval_ms=1};
    assert(ts_led_animation_start(touch,&animation)==ESP_OK);assert(ts_led_animation_get_state(touch)==TS_LED_ANIMATION_PLAYING&&!ts_led_image_is_playing(touch));animation.func=anim_breathing;assert(ts_led_animation_start(touch,&animation)==ESP_OK);assert(ts_led_animation_stop(touch)==ESP_OK);assert(ts_led_animation_get_state(touch)==TS_LED_ANIMATION_STOPPED);
    animation.func=anim_solid;assert(ts_led_animation_start(board,&animation)==ESP_OK);assert(ts_led_animation_stop(board)==ESP_OK);assert(color.r==25&&color.g==40&&color.b==55);
    /* Remove the first watch pair before watching the same live allocation again. */
    watched_count-=2;image_handoff(true,matrix);image_handoff(false,matrix);assert(!s_current_image&&!s_current_image_layer);
    ts_led_rgb_t static_pixels[4]={{255,0,0},{0,255,0},{0,0,255},{255,255,0}};
    assert(ts_led_image_create(static_pixels,2,2,&s_current_image)==ESP_OK);s_current_image_layer=matrix;
    assert(ts_led_image_display(matrix,s_current_image,NULL)==ESP_OK&&!ts_led_image_is_playing(matrix));assert(do_led_clear("matrix")==0);
    assert(api_image()==ESP_OK&&ts_led_image_is_playing(matrix));
    int api_resource=watch(s_api_current_image);assert(do_led_off("touch")==0&&watched[api_resource].frees==0&&ts_led_image_is_playing(matrix));
    assert(api_image()==ESP_OK&&watched[api_resource].frees==1&&ts_led_image_is_playing(matrix));
    assert(ts_led_image_animate_stop(matrix)==ESP_OK);assert(ts_led_image_free(s_api_current_image)==ESP_OK);s_api_current_image=NULL;
    assert(ts_led_text_overlay_is_active("matrix"));assert(ts_led_text_overlay_stop("matrix")==ESP_OK);
    if(s_api_current_font){ts_font_unload(s_api_current_font);s_api_current_font=NULL;}if(s_current_font){ts_font_unload(s_current_font);s_current_font=NULL;}
    s_led.render_running=false;join_tasks();assert(caps_created>0&&caps_created==caps_deleted);
    for(int i=0;i<3;i++){ts_led_device_impl_t *dev=(ts_led_device_impl_t *)devices[i];for(int j=0;j<dev->layer_count;j++)assert(ts_led_layer_destroy(dev->layers[j])==ESP_OK);assert(ts_led_device_destroy(dev)==ESP_OK);}
    vSemaphoreDelete(s_led.mutex);s_led.initialized=false;
    vSemaphoreDelete(glyph_entered);vSemaphoreDelete(glyph_release);vSemaphoreDelete(frame_entered);vSemaphoreDelete(frame_release);
    puts("PASS actual font/API/CLI/overlay/GIF/animation/render: readers join, failed font safely stops, owner matching, cross-device clear, per-layer GIF, caller-owned color, single release and Caps exit");
}
'''
headers = {path.name: path.read_text() for path in Path('components/ts_led/include').glob('*.h')}
headers['ts_led_private.h'] = (LED / 'ts_led_private.h').read_text()
headers.update({'led_strip.h': '#pragma once\ntypedef void *led_strip_handle_t;\n',
                'freertos/FreeRTOS.h': '#pragma once\n', 'freertos/semphr.h': '#pragma once\n'})
run_native(code, [IDF / 'components/json/cJSON/cJSON.c'], headers)
