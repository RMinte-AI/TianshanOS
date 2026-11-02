"""Strict production BMP/GIF decoding: pixels, ordinary LZW and declared bounds."""
from pathlib import Path
import argparse
import base64
import re
import struct
from test_console_lifecycle import RTOS, run_native

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=Path('components/ts_led/src/ts_led_image.c'))
parser.add_argument('--case', choices=['gif-continuous', 'bmp-valid', 'bmp-bad', 'gif-valid', 'gif-bad-min', 'gif-bad-block', 'gif-bad-code'])
args = parser.parse_args()
source = args.source.read_text()


def function(name):
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'


def bmp(top_down=False, width=2, offset=54):
    colors = [(0, 0, 255), (255, 255, 0), (255, 0, 0), (0, 255, 0)]
    rows = [colors[:2], colors[2:]]
    if width == 3:
        rows = [[(255, 0, 0), (0, 255, 0), (0, 0, 255)]] * 2
    pixels = b''.join(b''.join(bytes(reversed(color)) for color in row) + b'\0' * ((-width * 3) % 4)
                      for row in (rows if top_down else reversed(rows)))
    return (struct.pack('<2sIHHI', b'BM', offset + len(pixels), 0, 0, offset)
            + struct.pack('<IiiHHIIiiII', 40, width, -2 if top_down else 2, 1, 24, 0, len(pixels), 0, 0, 0, 0)
            + b'\x55' * (offset - 54) + pixels)


def gif(codes, minimum=2, local_palette=False):
    bits = minimum + 1
    packed = sum(code << (bits * i) for i, code in enumerate(codes)).to_bytes((len(codes) * bits + 7) // 8, 'little')
    return (b'GIF89a\x02\x00\x02\x00\x80\x00\x00\xff\x00\x00\x00\x00\xff'
            + b'\x2c\x00\x00\x00\x00\x02\x00\x02\x00' + bytes([0x80 if local_palette else 0])
            + (b'\xff\x00\x00\x00\x00\xff' if local_palette else b'')
            + bytes([minimum, len(packed)]) + packed + b'\x00\x3b')


# Pillow 10.3 encoded this ordinary interlaced GIF once, independently of the
# decoder. No encoder dependency is needed at runtime. Its original pixel index
# is y<8 ? 0 : (x*17+y*11+(x*y)%23)%16; palette i=(17*i,255-17*i,37*i)%256.
CONTINUOUS = base64.b64decode('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')

code = RTOS + r'''
#include <limits.h>
#include "ts_led_image.h"
#define TAG "image-bounds"
static int live_images;
static unsigned special_hits, width_growths;
static void *image_malloc(size_t n) {void *p=malloc(n);if(p)++live_images;return p;}
static void *image_calloc(size_t n,size_t s) {void *p=calloc(n,s);if(p)++live_images;return p;}
static void image_free(void *p) {if(p){assert(live_images>0);--live_images;free(p);}}
#define TS_MALLOC_PSRAM image_malloc
#define TS_CALLOC_PSRAM image_calloc
#define free image_free
'''
start = source.index('struct ts_led_image {')
code += source[start:source.index('\n};', start) + 3] + '\n'
code += '\n'.join(line for line in source.splitlines() if line.startswith('#define GIF_') or line.startswith('#define TS_LED_IMAGE_')) + '\n'
for name in ['read_le16', 'read_le32', 'gif_skip_sub_blocks']:
    if re.search(r'\b' + name + r'\([^;]+?\)\s*\{', source):
        code += function(name)
start = source.index('typedef struct {', source.index('static esp_err_t load_jpg'))
code += source[start:source.index('} lzw_entry_t;', start) + len('} lzw_entry_t;')] + '\n'
code += r'''
static esp_err_t load_png(const uint8_t *d,size_t n,ts_led_image_t *out) {(void)d;(void)n;(void)out;return ESP_ERR_NOT_SUPPORTED;}
static esp_err_t load_jpg(const uint8_t *d,size_t n,ts_led_image_t *out) {(void)d;(void)n;(void)out;return ESP_ERR_NOT_SUPPORTED;}
'''
for name in ['detect_format', 'load_bmp', 'lzw_decode', 'load_gif', 'ts_led_image_load_mem', 'ts_led_image_free', 'ts_led_image_get_info', 'ts_led_image_get_pixel']:
    body = function(name)
    if name == 'lzw_decode':
        # Coverage counters inside actual production branches, without copying
        # the decoding algorithm. The expected pixels come from the source image.
        body = body.replace('/* Special case: code not yet in table */', '/* Special case: code not yet in table */ ++special_hits;')
        body = body.replace('code_size++;', '++width_growths; code_size++;')
    code += body
code += '\n#undef free\n'


def array(name, data):
    return 'static const unsigned char ' + name + '[]={' + ','.join(map(str, data)) + '};\n'


code += array('bmp_bottom', bmp()) + array('bmp_top', bmp(True)) + array('bmp_three', bmp(width=3)) + array('bmp_offset', bmp(offset=60))
code += array('bmp_extra', bmp() + b'ignored tail')
code += array('continuous', CONTINUOUS)
single = gif([4, 0, 4, 1, 4, 0, 4, 1, 5])
code += array('gif_single', single)
code += array('gif_extra', single + b'ignored tail')
code += array('gif_no_trailer', single[:-1])
first = gif([4, 1, 4, 1, 4, 1, 4, 1, 5])
# First frame blue; transparent red in the next frame leaves that canvas blue.
second = b'\x21\xf9\x04\x01\x0f\x00\x00\x00' + single[19:-1]
code += array('gif_transparent', first[:-1] + second + b'\x3b')
code += array('gif_min13', gif([0], minimum=13))
code += array('gif_bad_code', gif([4, 0, 7, 5]))
code += array('gif_short_pixels', gif([4, 0, 5]))
code += array('gif_bad_palette', gif([4, 2, 4, 2, 4, 2, 4, 2, 5], local_palette=True))
code += array('gif_short_local_palette', gif([4, 0, 5], local_palette=True)[:32])
code += r'''
static ts_led_image_t decode(const void *data,size_t size) {
    ts_led_image_t image=NULL;assert(ts_led_image_load_mem(data,size,TS_LED_IMG_FMT_AUTO,&image)==ESP_OK&&image);
    return image;
}
static void release(ts_led_image_t image) {assert(ts_led_image_free(image)==ESP_OK);assert(!live_images);}
static void invalid(const void *data,size_t size) {
    /* BMP's old +10/+18/+22 reads are aligned at raw+2, so the old pixel
       overread is independently observable without disabling alignment checks. */
    unsigned char *raw=malloc(size+2);memcpy(raw+2,data,size);
    ts_led_image_t image=NULL;
    assert(ts_led_image_load_mem(raw+2,size,TS_LED_IMG_FMT_AUTO,&image)==ESP_ERR_INVALID_SIZE);
    assert(!image&&!live_images);
    free(raw);
}
static void rgb(ts_led_rgb_t color,int r,int g,int b) {assert(color.r==r&&color.g==g&&color.b==b);}
static void check_bmp(const unsigned char *data,size_t size,int width) {
    /* Start the whole input unaligned as well as exercising header offsets10/18/22. */
    unsigned char *raw=malloc(size+1);memcpy(raw+1,data,size);
    ts_led_image_t image=decode(raw+1,size);ts_led_image_info_t info;
    assert(ts_led_image_get_info(image,&info)==ESP_OK&&info.width==width&&info.height==2&&info.frame_count==1);
    if(width==2){rgb(image->pixels[0],0,0,255);rgb(image->pixels[1],255,255,0);rgb(image->pixels[2],255,0,0);rgb(image->pixels[3],0,255,0);}
    else for(int y=0;y<2;y++){rgb(image->pixels[y*3],255,0,0);rgb(image->pixels[y*3+1],0,255,0);rgb(image->pixels[y*3+2],0,0,255);}
    release(image);free(raw);
}
static void le32(unsigned char *p,uint32_t value) {for(int i=0;i<4;i++)p[i]=(value>>(i*8))&255;}
int main(int argc,char **argv) {
    const char *which=argc>1?argv[1]:"all";
    if(!strcmp(which,"all")||!strcmp(which,"gif-continuous")){
        ts_led_image_t image=decode(continuous,sizeof(continuous));ts_led_image_info_t info;
        assert(ts_led_image_get_info(image,&info)==ESP_OK&&info.width==96&&info.height==64&&info.frame_count==1);
        for(int y=0;y<64;y++)for(int x=0;x<96;x++){
            int i=y<8?0:(x*17+y*11+(x*y)%23)%16;ts_led_rgb_t color;
            assert(ts_led_image_get_pixel(image,x,y,&color)==ESP_OK);rgb(color,i*17,(255-i*17)%256,i*37%256);
        }
        printf("PASS ordinary continuous interlaced GIF: special=%u width-growth=%u\n",special_hits,width_growths);
        assert(special_hits>0&&width_growths>0);release(image);
    }
    if(!strcmp(which,"all")||!strcmp(which,"bmp-valid")){
        check_bmp(bmp_bottom,sizeof(bmp_bottom),2);check_bmp(bmp_top,sizeof(bmp_top),2);
        check_bmp(bmp_three,sizeof(bmp_three),3);check_bmp(bmp_offset,sizeof(bmp_offset),2);
        check_bmp(bmp_extra,sizeof(bmp_extra),2);
        check_bmp(bmp_bottom,sizeof(bmp_bottom)-2,2);puts("PASS BMP exact pixels, direction, unaligned input, padding and offset");
    }
    if(!strcmp(which,"all")||!strcmp(which,"bmp-bad")){
        invalid(bmp_bottom,53);invalid(bmp_bottom,54);invalid(bmp_bottom,sizeof(bmp_bottom)-3);
        unsigned char bad[sizeof(bmp_bottom)];
        memcpy(bad,bmp_bottom,sizeof(bad));le32(bad+10,UINT32_MAX);invalid(bad,sizeof(bad));
        memcpy(bad,bmp_bottom,sizeof(bad));le32(bad+18,UINT32_MAX);invalid(bad,sizeof(bad));
        memcpy(bad,bmp_bottom,sizeof(bad));le32(bad+18,0);invalid(bad,sizeof(bad));
        memcpy(bad,bmp_bottom,sizeof(bad));le32(bad+22,0);invalid(bad,sizeof(bad));
        memcpy(bad,bmp_bottom,sizeof(bad));le32(bad+22,0x80000000);invalid(bad,sizeof(bad));
        memcpy(bad,bmp_bottom,sizeof(bad));le32(bad+18,INT32_MAX);invalid(bad,sizeof(bad));
        puts("PASS BMP truncated/declared bounds and size arithmetic");
    }
    if(!strcmp(which,"all")||!strcmp(which,"gif-valid")){
        ts_led_image_t image=decode(gif_single,sizeof(gif_single));assert(image->frame_count==1);
        rgb(image->pixels[0],255,0,0);rgb(image->pixels[1],0,0,255);release(image);
        image=decode(gif_extra,sizeof(gif_extra));release(image);
        image=decode(gif_no_trailer,sizeof(gif_no_trailer));release(image);
        image=decode(gif_transparent,sizeof(gif_transparent));assert(image->frame_count==2);
        assert(image->frame_delays[0]==100&&image->frame_delays[1]==150);
        for(int f=0;f<2;f++)for(int p=0;p<4;p++)rgb(image->frames[f][p],0,0,255);
        release(image);puts("PASS normal GIF single/multiple frames, transparency and delays");
    }
    if(!strcmp(which,"all")||!strcmp(which,"gif-bad-min")){invalid(gif_min13,sizeof(gif_min13));puts("PASS GIF invalid minimum code size");}
    if(!strcmp(which,"all")||!strcmp(which,"gif-bad-block")){
        unsigned char bad[sizeof(gif_single)];memcpy(bad,gif_single,sizeof(bad));bad[30]=255;invalid(bad,sizeof(bad));
        invalid(gif_single,sizeof(gif_single)-2);invalid(gif_single,24);invalid(gif_single,15);
        invalid(gif_short_local_palette,sizeof(gif_short_local_palette));
        puts("PASS GIF truncated sub-block, descriptor and global/local palette");
    }
    if(!strcmp(which,"all")||!strcmp(which,"gif-bad-code")){
        invalid(gif_bad_code,sizeof(gif_bad_code));invalid(gif_short_pixels,sizeof(gif_short_pixels));
        invalid(gif_bad_palette,sizeof(gif_bad_palette));puts("PASS GIF undefined code, incomplete pixels and local palette bounds");
    }
    if(!strcmp(which,"all")){
        /* API/CLI clear their released output slot before the next load. */
        ts_led_image_t slot=NULL;
        assert(ts_led_image_load_mem(gif_single,sizeof(gif_single),TS_LED_IMG_FMT_AUTO,&slot)==ESP_OK);release(slot);slot=NULL;
        assert(ts_led_image_load_mem(gif_bad_code,sizeof(gif_bad_code),TS_LED_IMG_FMT_AUTO,&slot)==ESP_ERR_INVALID_SIZE);
        assert(!slot&&!live_images);
        assert(ts_led_image_load_mem(bmp_bottom,sizeof(bmp_bottom),TS_LED_IMG_FMT_AUTO,&slot)==ESP_OK);
        rgb(slot->pixels[0],0,0,255);release(slot);slot=NULL;
        puts("PASS reused public load_mem output slot: release, reject, reload, read and release");
    }
    assert(!live_images);puts("PASS image decoder bounds with strict ASan/UBSan");
}
'''
if args.case:
    code = code.replace('const char *which=argc>1?argv[1]:"all";', 'const char *which="' + args.case + '";')
headers = {p.name: p.read_text() for p in Path('components/ts_led/include').glob('*.h')}
run_native(code, extra_headers=headers)
