/**
 * @file ts_led_image.c
 * @brief Image Loading and Display
 * 
 * 图像数据优先分配到 PSRAM 以节省 DRAM
 */

#include "ts_led_image.h"
#include "ts_led_private.h"
#include "ts_storage.h"
#include "ts_log.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM, TS_CALLOC_PSRAM */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "led_image"

/* Enable format support based on available libraries */
#define TS_LED_IMAGE_PNG_SUPPORT    1
#define TS_LED_IMAGE_JPG_SUPPORT    1  
#define TS_LED_IMAGE_GIF_SUPPORT    1

struct ts_led_image {
    ts_led_rgb_t *pixels;
    uint8_t *alpha;              /**< Alpha channel (NULL if no transparency) */
    uint16_t width;
    uint16_t height;
    ts_led_image_format_t format;
    uint16_t frame_count;
    uint16_t current_frame;
    ts_led_rgb_t **frames;
    uint32_t *frame_delays;
    bool has_alpha;              /**< True if image has alpha channel */
};

/**
 * @brief Animation context for GIF playback
 */
typedef struct {
    ts_led_image_t image;
    ts_led_image_options_t options;
    uint16_t current_frame;
    uint32_t last_frame_time;
    ts_led_layer_t layer;
} ts_led_anim_ctx_t;


/* Forward declarations */
static void gif_animation_effect(ts_led_layer_t layer, uint32_t time_ms, void *user_data);

static ts_led_image_format_t detect_format(const uint8_t *data, size_t size)
{
    if (size < 4) return TS_LED_IMG_FMT_RAW;
    
    if (data[0] == 'B' && data[1] == 'M') return TS_LED_IMG_FMT_BMP;
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') 
        return TS_LED_IMG_FMT_PNG;
    if (data[0] == 0xFF && data[1] == 0xD8) return TS_LED_IMG_FMT_JPG;
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return TS_LED_IMG_FMT_GIF;
    
    return TS_LED_IMG_FMT_RAW;
}

static uint16_t read_le16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Simple BMP loader for 24-bit uncompressed BMP */
static esp_err_t load_bmp(const uint8_t *data, size_t size, ts_led_image_t *out)
{
    if (size < 54) return ESP_ERR_INVALID_SIZE;
    
    uint32_t offset = read_le32(data + 10);
    int32_t width = (int32_t)read_le32(data + 18);
    int32_t signed_height = (int32_t)read_le32(data + 22);
    uint16_t bpp = read_le16(data + 28);
    
    if (bpp != 24) {
        TS_LOGE(TAG, "Only 24-bit BMP supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (width <= 0 || signed_height == 0 || offset > size ||
        (size_t)width > (SIZE_MAX - 3) / 3) return ESP_ERR_INVALID_SIZE;
    bool flip = signed_height > 0;
    size_t height = signed_height < 0 ? -(int64_t)signed_height : signed_height;
    size_t row_bytes = (size_t)width * 3;
    size_t row_size = (row_bytes + 3) & ~(size_t)3;
    size_t available = size - offset;
    /* Only bytes actually read are required; final-row padding is unused. */
    if (row_bytes > available || height - 1 > (available - row_bytes) / row_size)
        return ESP_ERR_INVALID_SIZE;
    
    struct ts_led_image *img = TS_CALLOC_PSRAM(1, sizeof(struct ts_led_image));
    if (!img) return ESP_ERR_NO_MEM;
    
    img->width = width;
    img->height = height;
    img->format = TS_LED_IMG_FMT_BMP;
    img->frame_count = 1;
    
    img->pixels = TS_CALLOC_PSRAM((size_t)width * height, sizeof(ts_led_rgb_t));
    if (!img->pixels) {
        free(img);
        return ESP_ERR_NO_MEM;
    }
    
    const uint8_t *px = data + offset;
    
    for (size_t y = 0; y < height; y++) {
        size_t dst_y = flip ? (height - 1 - y) : y;
        for (int x = 0; x < width; x++) {
            size_t src_idx = y * row_size + (size_t)x * 3;
            size_t dst_idx = dst_y * width + x;
            img->pixels[dst_idx].b = px[src_idx];
            img->pixels[dst_idx].g = px[src_idx + 1];
            img->pixels[dst_idx].r = px[src_idx + 2];
        }
    }
    
    *out = img;
    return ESP_OK;
}

/*===========================================================================*/
/*                          PNG Loader                                        */
/*===========================================================================*/

#if TS_LED_IMAGE_PNG_SUPPORT

/* 
 * Simple PNG decoder - handles 8-bit RGB/RGBA PNGs
 * This is a minimal implementation for LED matrices
 */

#include "esp_rom_crc.h"
#include <zlib.h>

/* PNG chunk types */
#define PNG_IHDR    0x49484452
#define PNG_IDAT    0x49444154
#define PNG_IEND    0x49454E44
#define PNG_PLTE    0x504C5445

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | 
           ((uint32_t)p[2] << 8) | p[3];
}

/* Paeth predictor for PNG filtering */
static uint8_t paeth_predictor(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

static esp_err_t load_png(const uint8_t *data, size_t size, ts_led_image_t *out)
{
    /* Verify PNG signature */
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || memcmp(data, png_sig, 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const uint8_t *ptr = data + 8;
    const uint8_t *end = data + size;
    
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0;
    uint8_t *idat_data = NULL;
    size_t idat_size = 0;
    ts_led_rgb_t palette[256];
    int palette_size = 0;
    
    /* Parse chunks */
    while (ptr + 12 <= end) {
        uint32_t chunk_len = read_be32(ptr);
        uint32_t chunk_type = read_be32(ptr + 4);
        const uint8_t *chunk_data = ptr + 8;
        
        if (ptr + 12 + chunk_len > end) break;
        
        if (chunk_type == PNG_IHDR) {
            if (chunk_len < 13) return ESP_ERR_INVALID_SIZE;
            width = read_be32(chunk_data);
            height = read_be32(chunk_data + 4);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
            
            if (bit_depth != 8) {
                TS_LOGE(TAG, "PNG: Only 8-bit depth supported");
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (color_type != 0 && color_type != 2 && color_type != 3 && color_type != 6) {
                TS_LOGE(TAG, "PNG: Unsupported color type %d", color_type);
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
        else if (chunk_type == PNG_PLTE) {
            palette_size = chunk_len / 3;
            for (int i = 0; i < palette_size && i < 256; i++) {
                palette[i].r = chunk_data[i * 3];
                palette[i].g = chunk_data[i * 3 + 1];
                palette[i].b = chunk_data[i * 3 + 2];
            }
        }
        else if (chunk_type == PNG_IDAT) {
            /* Accumulate IDAT chunks */
            uint8_t *new_data = TS_REALLOC_PSRAM(idat_data, idat_size + chunk_len);
            if (!new_data) {
                free(idat_data);
                return ESP_ERR_NO_MEM;
            }
            idat_data = new_data;
            memcpy(idat_data + idat_size, chunk_data, chunk_len);
            idat_size += chunk_len;
        }
        else if (chunk_type == PNG_IEND) {
            break;
        }
        
        ptr += 12 + chunk_len;
    }
    
    if (width == 0 || height == 0 || !idat_data) {
        free(idat_data);
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Determine bytes per pixel and row */
    int bpp;
    switch (color_type) {
        case 0: bpp = 1; break;  /* Grayscale */
        case 2: bpp = 3; break;  /* RGB */
        case 3: bpp = 1; break;  /* Indexed */
        case 6: bpp = 4; break;  /* RGBA */
        default: bpp = 3; break;
    }
    
    size_t row_bytes = width * bpp + 1;  /* +1 for filter byte */
    size_t raw_size = row_bytes * height;
    
    uint8_t *raw_data = TS_MALLOC_PSRAM(raw_size);
    if (!raw_data) {
        free(idat_data);
        return ESP_ERR_NO_MEM;
    }
    
    /* Decompress */
    z_stream strm = {0};
    strm.next_in = idat_data;
    strm.avail_in = idat_size;
    strm.next_out = raw_data;
    strm.avail_out = raw_size;
    
    if (inflateInit(&strm) != Z_OK) {
        free(idat_data);
        free(raw_data);
        return ESP_FAIL;
    }
    
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    free(idat_data);
    
    if (ret != Z_STREAM_END) {
        free(raw_data);
        TS_LOGE(TAG, "PNG: Decompression failed");
        return ESP_FAIL;
    }
    
    /* Create image */
    struct ts_led_image *img = TS_CALLOC_PSRAM(1, sizeof(struct ts_led_image));
    if (!img) {
        free(raw_data);
        return ESP_ERR_NO_MEM;
    }
    
    img->width = width;
    img->height = height;
    img->format = TS_LED_IMG_FMT_PNG;
    img->frame_count = 1;
    img->has_alpha = (color_type == 6);  /* RGBA has alpha */
    
    img->pixels = TS_CALLOC_PSRAM(width * height, sizeof(ts_led_rgb_t));
    if (!img->pixels) {
        free(img);
        free(raw_data);
        return ESP_ERR_NO_MEM;
    }
    
    /* Allocate alpha channel for RGBA images */
    if (img->has_alpha) {
        img->alpha = TS_CALLOC_PSRAM(width * height, sizeof(uint8_t));
        if (!img->alpha) {
            free(img->pixels);
            free(img);
            free(raw_data);
            return ESP_ERR_NO_MEM;
        }
    }
    
    /* Unfilter and convert to RGB */
    uint8_t *prev_row = TS_CALLOC_PSRAM(width * bpp, 1);
    if (!prev_row) {
        free(img->pixels);
        free(img);
        free(raw_data);
        return ESP_ERR_NO_MEM;
    }
    
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = raw_data + y * row_bytes;
        uint8_t filter = row[0];
        uint8_t *pixels = row + 1;
        
        /* Apply filter */
        for (uint32_t i = 0; i < width * bpp; i++) {
            uint8_t a = (i >= (uint32_t)bpp) ? pixels[i - bpp] : 0;
            uint8_t b = prev_row[i];
            uint8_t c = (i >= (uint32_t)bpp) ? prev_row[i - bpp] : 0;
            
            switch (filter) {
                case 0: break;  /* None */
                case 1: pixels[i] += a; break;  /* Sub */
                case 2: pixels[i] += b; break;  /* Up */
                case 3: pixels[i] += (a + b) / 2; break;  /* Average */
                case 4: pixels[i] += paeth_predictor(a, b, c); break;  /* Paeth */
            }
        }
        
        memcpy(prev_row, pixels, width * bpp);
        
        /* Convert to RGB */
        for (uint32_t x = 0; x < width; x++) {
            ts_led_rgb_t *px = &img->pixels[y * width + x];
            
            switch (color_type) {
                case 0:  /* Grayscale */
                    px->r = px->g = px->b = pixels[x];
                    break;
                case 2:  /* RGB */
                    px->r = pixels[x * 3];
                    px->g = pixels[x * 3 + 1];
                    px->b = pixels[x * 3 + 2];
                    break;
                case 3:  /* Indexed */
                    if (pixels[x] < palette_size) {
                        *px = palette[pixels[x]];
                    }
                    break;
                case 6:  /* RGBA */
                    px->r = pixels[x * 4];
                    px->g = pixels[x * 4 + 1];
                    px->b = pixels[x * 4 + 2];
                    if (img->alpha) {
                        img->alpha[y * width + x] = pixels[x * 4 + 3];
                    }
                    break;
            }
        }
    }
    
    free(prev_row);
    free(raw_data);
    
    *out = img;
    TS_LOGI(TAG, "PNG loaded: %lux%lu", width, height);
    return ESP_OK;
}

#endif /* TS_LED_IMAGE_PNG_SUPPORT */

/*===========================================================================*/
/*                          JPG Loader                                        */
/*===========================================================================*/

#if TS_LED_IMAGE_JPG_SUPPORT

#include "jpeg_decoder.h"

static esp_err_t load_jpg(const uint8_t *data, size_t size, ts_led_image_t *out)
{
    /* First, get image info */
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)data,
        .indata_size = size,
        .outbuf = NULL,
        .outbuf_size = 0,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    
    esp_jpeg_image_output_t img_info;
    esp_err_t ret = esp_jpeg_get_image_info(&cfg, &img_info);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to get JPEG info");
        return ret;
    }
    
    uint32_t width = img_info.width;
    uint32_t height = img_info.height;
    
    struct ts_led_image *img = TS_CALLOC_PSRAM(1, sizeof(struct ts_led_image));
    if (!img) {
        return ESP_ERR_NO_MEM;
    }
    
    img->width = width;
    img->height = height;
    img->format = TS_LED_IMG_FMT_JPG;
    img->frame_count = 1;
    
    /* Allocate output buffer for RGB888 data */
    size_t outbuf_size = width * height * 3;
    uint8_t *outbuf = TS_MALLOC_PSRAM(outbuf_size);
    if (!outbuf) {
        free(img);
        return ESP_ERR_NO_MEM;
    }
    
    img->pixels = TS_CALLOC_PSRAM(width * height, sizeof(ts_led_rgb_t));
    if (!img->pixels) {
        free(outbuf);
        free(img);
        return ESP_ERR_NO_MEM;
    }
    
    /* Decode JPEG */
    cfg.outbuf = outbuf;
    cfg.outbuf_size = outbuf_size;
    
    ret = esp_jpeg_decode(&cfg, &img_info);
    if (ret != ESP_OK) {
        free(outbuf);
        free(img->pixels);
        free(img);
        TS_LOGE(TAG, "Failed to decode JPEG");
        return ret;
    }
    
    /* Convert RGB888 to our pixel format */
    for (uint32_t i = 0; i < width * height; i++) {
        img->pixels[i].r = outbuf[i * 3];
        img->pixels[i].g = outbuf[i * 3 + 1];
        img->pixels[i].b = outbuf[i * 3 + 2];
    }
    
    free(outbuf);
    
    *out = img;
    TS_LOGI(TAG, "JPG loaded: %lux%lu", width, height);
    return ESP_OK;
}

#endif /* TS_LED_IMAGE_JPG_SUPPORT */

/*===========================================================================*/
/*                          GIF Loader                                        */
/*===========================================================================*/

#if TS_LED_IMAGE_GIF_SUPPORT

/* Simple GIF decoder - single frame support */

static bool gif_skip_sub_blocks(const uint8_t **ptr_io, const uint8_t *end)
{
    const uint8_t *ptr = *ptr_io;
    while (ptr < end) {
        uint8_t length = *ptr++;
        if (length == 0) { *ptr_io = ptr; return true; }
        if ((size_t)(end - ptr) < length) return false;
        ptr += length;
    }
    return false;
}

/* LZW 解码器 */
typedef struct {
    uint16_t prefix;
    uint8_t suffix;
    uint8_t first;
} lzw_entry_t;

static int lzw_decode(const uint8_t *data, size_t size, const uint8_t **ptr_io,
                      uint8_t *output, size_t out_size, int min_code_size)
{
    const uint8_t *ptr = *ptr_io;
    const uint8_t *data_end = data + size;
    if (min_code_size < 2 || min_code_size > 8) return -ESP_ERR_INVALID_SIZE;
    
    int code_size = min_code_size + 1;
    int clear_code = 1 << min_code_size;
    int end_code = clear_code + 1;
    int next_code = end_code + 1;
    int max_code = (1 << code_size) - 1;
    
    /* Allocate LZW table and stack once */
    lzw_entry_t *table = TS_MALLOC_PSRAM(4096 * sizeof(lzw_entry_t));
    uint8_t *stack = TS_MALLOC_PSRAM(4096);
    if (!table || !stack) {
        free(table);
        free(stack);
        return -ESP_ERR_NO_MEM;
    }
    
    /* Initialize base table */
    for (int i = 0; i < clear_code; i++) {
        table[i].prefix = 0xFFFF;
        table[i].suffix = i;
        table[i].first = i;
    }
    
    uint32_t bit_buffer = 0;
    int bits_in_buffer = 0;
    int block_size = 0;
    const uint8_t *block_ptr = NULL;
    int block_pos = 0;
    size_t out_pos = 0;
    int prev_code = -1;
    
    while (out_pos < out_size) {
        /* Read enough bits */
        while (bits_in_buffer < code_size) {
            /* Get next byte from sub-block */
            if (block_pos >= block_size) {
                if (ptr >= data_end) goto invalid;
                block_size = *ptr++;
                if (block_size == 0 || (size_t)(data_end - ptr) < (size_t)block_size) goto invalid;
                block_ptr = ptr;
                block_pos = 0;
                ptr += block_size;  /* Skip to next block header */
            }
            if (block_ptr && block_pos < block_size) {
                bit_buffer |= ((uint32_t)block_ptr[block_pos++]) << bits_in_buffer;
                bits_in_buffer += 8;
            } else {
                goto invalid;
            }
        }
        
        int code = bit_buffer & ((1 << code_size) - 1);
        bit_buffer >>= code_size;
        bits_in_buffer -= code_size;
        
        if (code == clear_code) {
            code_size = min_code_size + 1;
            next_code = end_code + 1;
            max_code = (1 << code_size) - 1;
            prev_code = -1;
            continue;
        }
        
        if (code == end_code) break;
        
        /* Build output string on stack */
        int stack_top = 0;
        int cur = code;
        
        if (cur >= next_code) {
            if (cur != next_code || prev_code < 0) goto invalid;
            /* Special case: code not yet in table */
            stack[stack_top++] = table[prev_code].first;
            cur = prev_code;
        }
        
        while (cur >= clear_code) {
            if (cur >= next_code || stack_top >= 4096) goto invalid;
            stack[stack_top++] = table[cur].suffix;
            cur = table[cur].prefix;
            if (cur == 0xFFFF) goto invalid;
        }
        if (cur < clear_code) {
            if (stack_top >= 4096) goto invalid;
            stack[stack_top++] = cur;
        }
        
        /* Output in reverse order */
        uint8_t first_char = (stack_top > 0) ? stack[stack_top - 1] : 0;
        while (stack_top > 0 && out_pos < out_size) {
            output[out_pos++] = stack[--stack_top];
        }
        
        /* Add new table entry */
        if (prev_code >= 0 && next_code < 4096) {
            table[next_code].prefix = prev_code;
            table[next_code].suffix = first_char;
            table[next_code].first = table[prev_code].first;
            next_code++;
            
            if (next_code > max_code && code_size < 12) {
                code_size++;
                max_code = (1 << code_size) - 1;
            }
        }
        
        prev_code = code;
    }
    
    /* Skip remaining sub-blocks */
    if (out_pos != out_size || !gif_skip_sub_blocks(&ptr, data_end)) goto invalid;
    
    *ptr_io = ptr;
    free(table);
    free(stack);
    return (int)out_pos;
invalid:
    free(table);
    free(stack);
    return -ESP_ERR_INVALID_SIZE;
}

/* Maximum memory for GIF frames (leave room for other allocations) */
#define GIF_MAX_FRAME_MEMORY    (4 * 1024 * 1024)  /* 4MB max for all frames */
#define GIF_MAX_FRAMES          32                  /* Max frames to keep */

static esp_err_t load_gif(const uint8_t *data, size_t size, ts_led_image_t *out)
{
    /* Verify GIF signature */
    if (size < 13 || (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint16_t orig_width = read_le16(data + 6);
    uint16_t orig_height = read_le16(data + 8);
    if (orig_width == 0 || orig_height == 0) return ESP_ERR_INVALID_SIZE;
    uint8_t flags = data[10];
    uint8_t bg_index = data[11];
    bool has_gct = (flags & 0x80) != 0;
    int gct_size = has_gct ? (1 << ((flags & 0x07) + 1)) : 0;
    
    const uint8_t *ptr = data + 13;
    const uint8_t *end = data + size;
    
    /* Read global color table */
    ts_led_rgb_t gct[256] = {0};
    if (has_gct) {
        if ((size_t)(end - ptr) < (size_t)gct_size * 3) return ESP_ERR_INVALID_SIZE;
        for (int i = 0; i < gct_size; i++) {
            gct[i].r = ptr[0];
            gct[i].g = ptr[1];
            gct[i].b = ptr[2];
            ptr += 3;
        }
    }
    
    /* First pass: count frames */
    int frame_count = 0;
    const uint8_t *scan = ptr;
    while (scan < end) {
        if (*scan == 0x2C) {  /* Image Descriptor */
            scan++;
            if ((size_t)(end - scan) < 9) return ESP_ERR_INVALID_SIZE;
            uint8_t img_flags = scan[8];
            scan += 9;
            /* Skip local color table */
            if (img_flags & 0x80) {
                int lct_size = 1 << ((img_flags & 0x07) + 1);
                if ((size_t)(end - scan) < (size_t)lct_size * 3) return ESP_ERR_INVALID_SIZE;
                scan += lct_size * 3;
            }
            /* Skip LZW data */
            if (scan >= end || *scan < 2 || *scan > 8) return ESP_ERR_INVALID_SIZE;
            scan++;  /* LZW min code size */
            if (!gif_skip_sub_blocks(&scan, end)) return ESP_ERR_INVALID_SIZE;
            frame_count++;
        } else if (*scan == 0x21) {  /* Extension */
            scan++;
            if (scan >= end) return ESP_ERR_INVALID_SIZE;
            scan++;  /* extension type */
            if (!gif_skip_sub_blocks(&scan, end)) return ESP_ERR_INVALID_SIZE;
        } else if (*scan == 0x3B) {  /* Trailer */
            break;
        } else {
            scan++;
        }
    }
    
    if (frame_count == 0) frame_count = 1;
    if (frame_count > GIF_MAX_FRAMES) {
        TS_LOGW(TAG, "GIF has %d frames, limiting to %d", frame_count, GIF_MAX_FRAMES);
        frame_count = GIF_MAX_FRAMES;
    }
    
    /* Calculate memory needed and determine scale factor */
    size_t orig_frame_size = (size_t)orig_width * orig_height * sizeof(ts_led_rgb_t);
    size_t total_needed = orig_frame_size * frame_count;
    
    int scale = 1;
    while (total_needed / (scale * scale) > GIF_MAX_FRAME_MEMORY && scale < 16) {
        scale++;
    }
    
    uint16_t width = orig_width / scale;
    uint16_t height = orig_height / scale;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    
    if (scale > 1) {
        TS_LOGI(TAG, "GIF: %ux%u -> %ux%u (1/%d scale), %d frames", 
                orig_width, orig_height, width, height, scale, frame_count);
    } else {
        TS_LOGI(TAG, "GIF: %ux%u, %d frames detected", width, height, frame_count);
    }
    
    /* Create image structure */
    struct ts_led_image *img = TS_CALLOC_PSRAM(1, sizeof(struct ts_led_image));
    if (!img) return ESP_ERR_NO_MEM;
    
    img->width = width;
    img->height = height;
    img->format = TS_LED_IMG_FMT_GIF;
    img->frame_count = frame_count;
    img->has_alpha = false;
    
    /* Allocate frame arrays */
    img->frames = TS_CALLOC_PSRAM(frame_count, sizeof(ts_led_rgb_t *));
    img->frame_delays = TS_CALLOC_PSRAM(frame_count, sizeof(uint32_t));
    if (!img->frames || !img->frame_delays) {
        free(img->frames);
        free(img->frame_delays);
        free(img);
        return ESP_ERR_NO_MEM;
    }
    
    /* Allocate each frame (at scaled size) */
    size_t frame_size = (size_t)width * height;
    for (int i = 0; i < frame_count; i++) {
        img->frames[i] = TS_CALLOC_PSRAM(frame_size, sizeof(ts_led_rgb_t));
        if (!img->frames[i]) {
            for (int j = 0; j < i; j++) free(img->frames[j]);
            free(img->frames);
            free(img->frame_delays);
            free(img);
            return ESP_ERR_NO_MEM;
        }
        /* Fill with background color */
        for (size_t p = 0; p < frame_size; p++) {
            img->frames[i][p] = gct[bg_index];
        }
        img->frame_delays[i] = 100;  /* Default 100ms */
    }
    
    /* Allocate alpha channel */
    img->alpha = TS_CALLOC_PSRAM(frame_size, sizeof(uint8_t));
    if (!img->alpha) {
        for (int i = 0; i < frame_count; i++) free(img->frames[i]);
        free(img->frames);
        free(img->frame_delays);
        free(img);
        return ESP_ERR_NO_MEM;
    }
    memset(img->alpha, 255, frame_size);
    
    /* Second pass: decode frames */
    int current_frame = 0;
    int transparent_index = -1;
    uint16_t delay_cs = 10;  /* delay in centiseconds */
    uint8_t disposal = 0;
    
    /* Canvas for frame composition (at original size) */
    size_t orig_canvas_size = (size_t)orig_width * orig_height;
    ts_led_rgb_t *canvas = TS_CALLOC_PSRAM(orig_canvas_size, sizeof(ts_led_rgb_t));
    if (!canvas) {
        free(img->alpha);
        for (int i = 0; i < frame_count; i++) free(img->frames[i]);
        free(img->frames);
        free(img->frame_delays);
        free(img);
        return ESP_ERR_NO_MEM;
    }
    
    /* Initialize canvas with background */
    for (size_t p = 0; p < orig_canvas_size; p++) {
        canvas[p] = gct[bg_index];
    }
    
    ts_led_rgb_t *lct = NULL;
    uint8_t *indices = NULL;
    esp_err_t error = ESP_ERR_INVALID_SIZE;
    while (ptr < end && current_frame < frame_count) {
        if (*ptr == 0x2C) {  /* Image Descriptor */
            ptr++;
            if ((size_t)(end - ptr) < 9) goto invalid_gif;
            
            uint16_t left = read_le16(ptr);
            uint16_t top = read_le16(ptr + 2);
            uint16_t img_width = read_le16(ptr + 4);
            uint16_t img_height = read_le16(ptr + 6);
            uint8_t img_flags = ptr[8];
            ptr += 9;
            
            /* Local color table */
            ts_led_rgb_t *palette = gct;
            int palette_size = 256;
            if (img_flags & 0x80) {
                int lct_size = 1 << ((img_flags & 0x07) + 1);
                if ((size_t)(end - ptr) < (size_t)lct_size * 3) goto invalid_gif;
                lct = TS_MALLOC_PSRAM(lct_size * sizeof(ts_led_rgb_t));
                if (!lct) { error = ESP_ERR_NO_MEM; goto invalid_gif; }
                for (int i = 0; i < lct_size; i++) {
                    lct[i].r = ptr[0];
                    lct[i].g = ptr[1];
                    lct[i].b = ptr[2];
                    ptr += 3;
                }
                palette = lct;
                palette_size = lct_size;
            }
            
            /* LZW minimum code size */
            if (ptr >= end) goto invalid_gif;
            uint8_t lzw_min = *ptr++;
            
            /* Decode LZW data */
            size_t pixel_count = (size_t)img_width * img_height;
            if (pixel_count == 0) goto invalid_gif;
            indices = TS_MALLOC_PSRAM(pixel_count);
            if (!indices) { error = ESP_ERR_NO_MEM; goto invalid_gif; }
            int decoded = lzw_decode(data, size, &ptr, indices, pixel_count, lzw_min);
            if (decoded < 0) { error = -decoded; goto invalid_gif; }
            if ((size_t)decoded != pixel_count) goto invalid_gif;
            /* Handle disposal of previous frame */
            if (current_frame > 0 && disposal == 2) {
                /* Restore to background */
                for (size_t p = 0; p < orig_canvas_size; p++) {
                    canvas[p] = gct[bg_index];
                }
            }
            /* disposal == 1: leave in place, disposal == 3: restore to previous (not impl) */

            /* Decode indices to canvas (original size) */
            bool interlaced = (img_flags & 0x40) != 0;
            int pass_start[] = {0, 4, 2, 1};
            int pass_step[] = {8, 8, 4, 2};

            int src_idx = 0;
            for (int pass = 0; pass < (interlaced ? 4 : 1); pass++) {
                int y_start = interlaced ? pass_start[pass] : 0;
                int y_step = interlaced ? pass_step[pass] : 1;

                for (int y = y_start; y < img_height && src_idx < decoded; y += y_step) {
                    for (int x = 0; x < img_width && src_idx < decoded; x++) {
                        int dst_x = left + x;
                        int dst_y = top + y;
                        if (dst_x < orig_width && dst_y < orig_height) {
                            int dst_idx = dst_y * orig_width + dst_x;
                            int color_idx = indices[src_idx];

                            if (color_idx != transparent_index) {
                                if (color_idx >= palette_size) goto invalid_gif;
                                canvas[dst_idx] = palette[color_idx];
                            }
                        }
                        src_idx++;
                    }
                }
            }

            /* Scale canvas to frame (if needed) */
            if (scale == 1) {
                memcpy(img->frames[current_frame], canvas, frame_size * sizeof(ts_led_rgb_t));
            } else {
                /* Nearest-neighbor downscale */
                for (int dy = 0; dy < height; dy++) {
                    for (int dx = 0; dx < width; dx++) {
                        int sx = dx * scale;
                        int sy = dy * scale;
                        img->frames[current_frame][dy * width + dx] = canvas[sy * orig_width + sx];
                    }
                }
            }
            
            img->frame_delays[current_frame] = delay_cs * 10;  /* Convert to ms */
            if (img->frame_delays[current_frame] < 20) {
                img->frame_delays[current_frame] = 100;  /* Min 100ms for very fast GIFs */
            }
            free(indices);
            indices = NULL;

            free(lct);
            lct = NULL;
            
            /* Reset per-frame state */
            transparent_index = -1;
            delay_cs = 10;
            disposal = 0;
            current_frame++;
        }
        else if (*ptr == 0x21) {  /* Extension */
            ptr++;
            if (ptr >= end) goto invalid_gif;
            uint8_t ext_type = *ptr++;
            
            if (ext_type == 0xF9 && ptr < end) {
                /* Graphic Control Extension */
                int block_size = *ptr;
                if (block_size >= 4 && (size_t)(end - ptr) >= 5) {
                    uint8_t gce_flags = ptr[1];
                    delay_cs = read_le16(ptr + 2);
                    disposal = (gce_flags >> 2) & 0x07;
                    if (gce_flags & 0x01) {
                        transparent_index = ptr[4];
                    }
                }
            }
            if (!gif_skip_sub_blocks(&ptr, end)) goto invalid_gif;
        }
        else if (*ptr == 0x3B) {  /* Trailer */
            break;
        }
        else {
            ptr++;
        }
    }
    
    free(canvas);
    
    /* Set pixels to first frame */
    img->pixels = img->frames[0];
    img->current_frame = 0;
    
    /* For multi-frame GIFs, don't use alpha (frames are composited) */
    if (frame_count > 1) {
        img->has_alpha = false;
    }
    
    /* Free alpha channel if not used */
    if (!img->has_alpha) {
        free(img->alpha);
        img->alpha = NULL;
    }
    
    *out = img;
    TS_LOGI(TAG, "GIF loaded: %ux%u, %d frame(s)%s", width, height, frame_count,
            img->has_alpha ? " (with transparency)" : "");
    return ESP_OK;
invalid_gif:
    free(lct);
    free(indices);
    free(canvas);
    ts_led_image_free(img);
    return error;
}

#endif /* TS_LED_IMAGE_GIF_SUPPORT */

esp_err_t ts_led_image_load(const char *path, ts_led_image_format_t format,
                             ts_led_image_t *image)
{
    if (!path || !image) return ESP_ERR_INVALID_ARG;
    
    ssize_t size = ts_storage_size(path);
    if (size < 0) return ESP_ERR_NOT_FOUND;
    
    /* 文件数据加载到 PSRAM（临时缓冲区） */
    uint8_t *data = TS_MALLOC_PSRAM(size);
    if (!data) return ESP_ERR_NO_MEM;
    
    if (ts_storage_read_file(path, data, size) != size) {
        free(data);
        return ESP_FAIL;
    }
    
    esp_err_t ret = ts_led_image_load_mem(data, size, format, image);
    free(data);
    return ret;
}

esp_err_t ts_led_image_load_mem(const void *data, size_t size,
                                 ts_led_image_format_t format,
                                 ts_led_image_t *image)
{
    if (!data || !image) return ESP_ERR_INVALID_ARG;
    
    if (format == TS_LED_IMG_FMT_AUTO) {
        format = detect_format(data, size);
    }
    
    switch (format) {
        case TS_LED_IMG_FMT_BMP:
            return load_bmp(data, size, image);
#if TS_LED_IMAGE_PNG_SUPPORT
        case TS_LED_IMG_FMT_PNG:
            return load_png(data, size, image);
#endif
#if TS_LED_IMAGE_JPG_SUPPORT
        case TS_LED_IMG_FMT_JPG:
            return load_jpg(data, size, image);
#endif
#if TS_LED_IMAGE_GIF_SUPPORT
        case TS_LED_IMG_FMT_GIF:
            return load_gif(data, size, image);
#endif
        default:
            TS_LOGW(TAG, "Format %d not implemented", format);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t ts_led_image_create(const ts_led_rgb_t *data, uint16_t width,
                               uint16_t height, ts_led_image_t *image)
{
    if (!data || !image) return ESP_ERR_INVALID_ARG;
    
    struct ts_led_image *img = TS_CALLOC_PSRAM(1, sizeof(struct ts_led_image));
    if (!img) return ESP_ERR_NO_MEM;
    
    img->width = width;
    img->height = height;
    img->format = TS_LED_IMG_FMT_RAW;
    img->frame_count = 1;
    
    size_t px_size = width * height * sizeof(ts_led_rgb_t);
    img->pixels = TS_MALLOC_PSRAM(px_size);
    if (!img->pixels) {
        free(img);
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(img->pixels, data, px_size);
    *image = img;
    return ESP_OK;
}

esp_err_t ts_led_image_free(ts_led_image_t image)
{
    if (!image) return ESP_ERR_INVALID_ARG;
    
    /* For multi-frame images (GIF), pixels points to frames[0], so don't double-free */
    if (image->frames) {
        /* Free all frames */
        for (int i = 0; i < image->frame_count; i++) {
            free(image->frames[i]);
        }
        free(image->frames);
        /* pixels was pointing to frames[0], already freed above */
    } else {
        /* Single-frame image, pixels is separately allocated */
        free(image->pixels);
    }
    
    free(image->alpha);
    free(image->frame_delays);
    free(image);
    return ESP_OK;
}

esp_err_t ts_led_image_get_info(ts_led_image_t image, ts_led_image_info_t *info)
{
    if (!image || !info) return ESP_ERR_INVALID_ARG;
    
    info->width = image->width;
    info->height = image->height;
    info->format = image->format;
    info->frame_count = image->frame_count;
    info->frame_delays = image->frame_delays;
    return ESP_OK;
}

esp_err_t ts_led_image_get_pixel(ts_led_image_t image, uint16_t x, uint16_t y,
                                  ts_led_rgb_t *color)
{
    if (!image || !color) return ESP_ERR_INVALID_ARG;
    if (x >= image->width || y >= image->height) return ESP_ERR_INVALID_ARG;
    
    *color = image->pixels[y * image->width + x];
    return ESP_OK;
}

/**
 * @brief 计算图像中非透明像素的边界框
 * 
 * @param image 图像句柄
 * @param[out] x0, y0 左上角坐标
 * @param[out] x1, y1 右下角坐标（不含）
 * @return true 如果找到非透明像素，false 如果全透明
 */
static bool get_content_bounds(ts_led_image_t image, 
                                uint16_t *x0, uint16_t *y0,
                                uint16_t *x1, uint16_t *y1)
{
    if (!image->has_alpha || !image->alpha) {
        // 无透明通道，整个图像就是内容
        *x0 = 0;
        *y0 = 0;
        *x1 = image->width;
        *y1 = image->height;
        return true;
    }
    
    uint16_t min_x = image->width, min_y = image->height;
    uint16_t max_x = 0, max_y = 0;
    bool found = false;
    
    for (uint16_t y = 0; y < image->height; y++) {
        for (uint16_t x = 0; x < image->width; x++) {
            uint8_t alpha = image->alpha[y * image->width + x];
            if (alpha >= 128) {  // 非透明像素
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (x >= max_x) max_x = x + 1;
                if (y >= max_y) max_y = y + 1;
                found = true;
            }
        }
    }
    
    if (!found) {
        // 全透明
        *x0 = *y0 = 0;
        *x1 = image->width;
        *y1 = image->height;
        return false;
    }
    
    *x0 = min_x;
    *y0 = min_y;
    *x1 = max_x;
    *y1 = max_y;
    
    // 使用 VERBOSE 级别避免动画时刷屏
    TS_LOGV(TAG, "Content bounds: (%u,%u) - (%u,%u), size: %ux%u",
            min_x, min_y, max_x, max_y, max_x - min_x, max_y - min_y);
    
    return true;
}

/**
 * @brief 使用双线性插值获取缩放后的像素颜色
 */
static ts_led_rgb_t get_scaled_pixel_bilinear(const ts_led_rgb_t *pixels, 
                                               uint16_t img_w, uint16_t img_h,
                                               float src_x, float src_y)
{
    // 边界处理
    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x >= img_w - 1) src_x = img_w - 1.001f;
    if (src_y >= img_h - 1) src_y = img_h - 1.001f;
    
    int x0 = (int)src_x;
    int y0 = (int)src_y;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    
    // 确保边界安全
    if (x1 >= img_w) x1 = img_w - 1;
    if (y1 >= img_h) y1 = img_h - 1;
    
    float fx = src_x - x0;
    float fy = src_y - y0;
    
    // 获取四个相邻像素
    ts_led_rgb_t p00 = pixels[y0 * img_w + x0];
    ts_led_rgb_t p10 = pixels[y0 * img_w + x1];
    ts_led_rgb_t p01 = pixels[y1 * img_w + x0];
    ts_led_rgb_t p11 = pixels[y1 * img_w + x1];
    
    // 双线性插值
    ts_led_rgb_t result;
    result.r = (uint8_t)((1-fx)*(1-fy)*p00.r + fx*(1-fy)*p10.r + 
                         (1-fx)*fy*p01.r + fx*fy*p11.r);
    result.g = (uint8_t)((1-fx)*(1-fy)*p00.g + fx*(1-fy)*p10.g + 
                         (1-fx)*fy*p01.g + fx*fy*p11.g);
    result.b = (uint8_t)((1-fx)*(1-fy)*p00.b + fx*(1-fy)*p10.b + 
                         (1-fx)*fy*p01.b + fx*fy*p11.b);
    
    return result;
}

/**
 * @brief 使用区域平均获取缩放后的像素颜色（适合缩小图像）
 */
static ts_led_rgb_t get_scaled_pixel_area(const ts_led_rgb_t *pixels, 
                                           uint16_t img_w, uint16_t img_h,
                                           float src_x, float src_y,
                                           float scale_x, float scale_y)
{
    // 计算源图像中需要采样的区域
    float x0 = src_x;
    float y0 = src_y;
    float x1 = src_x + (1.0f / scale_x);
    float y1 = src_y + (1.0f / scale_y);
    
    // 边界限制
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img_w) x1 = img_w;
    if (y1 > img_h) y1 = img_h;
    
    // 采样并平均
    uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
    uint32_t count = 0;
    
    for (int sy = (int)y0; sy < (int)y1 && sy < img_h; sy++) {
        for (int sx = (int)x0; sx < (int)x1 && sx < img_w; sx++) {
            ts_led_rgb_t p = pixels[sy * img_w + sx];
            r_sum += p.r;
            g_sum += p.g;
            b_sum += p.b;
            count++;
        }
    }
    
    if (count == 0) {
        return (ts_led_rgb_t){0, 0, 0};
    }
    
    return (ts_led_rgb_t){
        .r = r_sum / count,
        .g = g_sum / count,
        .b = b_sum / count
    };
}

esp_err_t ts_led_image_display(ts_led_layer_t layer, ts_led_image_t image,
                                const ts_led_image_options_t *options)
{
    if (!layer || !image) return ESP_ERR_INVALID_ARG;
    
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    
    ts_led_image_options_t opts = options ? *options : 
        (ts_led_image_options_t)TS_LED_IMAGE_DEFAULT_OPTIONS();
    
    uint16_t dev_w = l->device->config.width;
    uint16_t dev_h = l->device->config.height;
    
    // 如果图像有透明通道，先清空 layer（透明区域显示为黑色）
    if (image->has_alpha) {
        ts_led_fill(layer, (ts_led_rgb_t){0, 0, 0});
    }
    
    // 确定用于缩放计算的源区域
    uint16_t src_x0 = 0, src_y0 = 0;
    uint16_t src_w = image->width, src_h = image->height;
    
    // 如果是内容居中模式，计算非透明内容的边界框
    if (opts.center == TS_LED_IMG_CENTER_CONTENT && image->has_alpha) {
        uint16_t cx0, cy0, cx1, cy1;
        if (get_content_bounds(image, &cx0, &cy0, &cx1, &cy1)) {
            src_x0 = cx0;
            src_y0 = cy0;
            src_w = cx1 - cx0;
            src_h = cy1 - cy0;
            // 使用 VERBOSE 级别避免动画时刷屏
            TS_LOGV(TAG, "Content region: origin=(%u,%u) size=%ux%u", 
                    src_x0, src_y0, src_w, src_h);
        }
    }
    
    // 根据缩放模式计算目标尺寸和缩放比例
    uint16_t dst_w, dst_h;
    float scale_x, scale_y;
    int16_t offset_x = opts.x, offset_y = opts.y;
    
    switch (opts.scale) {
        case TS_LED_IMG_SCALE_NONE:
            // 不缩放，直接显示原始尺寸
            dst_w = src_w;
            dst_h = src_h;
            scale_x = 1.0f;
            scale_y = 1.0f;
            // 居中
            offset_x = opts.x + (dev_w - dst_w) / 2;
            offset_y = opts.y + (dev_h - dst_h) / 2;
            break;
            
        case TS_LED_IMG_SCALE_FIT: {
            // 保持宽高比，适应设备尺寸
            float ratio_x = (float)dev_w / src_w;
            float ratio_y = (float)dev_h / src_h;
            float ratio = (ratio_x < ratio_y) ? ratio_x : ratio_y;
            dst_w = (uint16_t)(src_w * ratio);
            dst_h = (uint16_t)(src_h * ratio);
            scale_x = ratio;
            scale_y = ratio;
            // 居中显示
            offset_x = opts.x + (dev_w - dst_w) / 2;
            offset_y = opts.y + (dev_h - dst_h) / 2;
            break;
        }
        
        case TS_LED_IMG_SCALE_FILL: {
            // 保持宽高比，填充设备（可能裁剪）
            float ratio_x = (float)dev_w / src_w;
            float ratio_y = (float)dev_h / src_h;
            float ratio = (ratio_x > ratio_y) ? ratio_x : ratio_y;
            dst_w = dev_w;
            dst_h = dev_h;
            scale_x = ratio;
            scale_y = ratio;
            offset_x = opts.x;
            offset_y = opts.y;
            break;
        }
        
        case TS_LED_IMG_SCALE_STRETCH:
            // 拉伸到设备尺寸
            dst_w = dev_w;
            dst_h = dev_h;
            scale_x = (float)dev_w / src_w;
            scale_y = (float)dev_h / src_h;
            offset_x = opts.x;
            offset_y = opts.y;
            break;
            
        default:
            dst_w = src_w;
            dst_h = src_h;
            scale_x = 1.0f;
            scale_y = 1.0f;
            break;
    }
    
    // 使用 VERBOSE 级别避免动画时刷屏
    TS_LOGV(TAG, "Display: src=%ux%u -> dst=%ux%u (scale: %.2f x %.2f, offset: %d,%d)",
            src_w, src_h, dst_w, dst_h, scale_x, scale_y, offset_x, offset_y);
    
    // 缩小时使用区域平均，放大时使用双线性插值
    bool use_area_sampling = (scale_x < 1.0f || scale_y < 1.0f);
    
    for (int dy = 0; dy < dst_h && dy + offset_y < dev_h; dy++) {
        for (int dx = 0; dx < dst_w && dx + offset_x < dev_w; dx++) {
            int16_t px_x = dx + offset_x;
            int16_t px_y = dy + offset_y;
            
            if (px_x < 0 || px_y < 0) continue;
            
            // 计算源图像坐标（相对于内容区域起点）
            float src_x, src_y;
            
            if (opts.scale == TS_LED_IMG_SCALE_FILL) {
                // FILL 模式：从内容区域中心裁剪
                float src_offset_x = (src_w * scale_x - dev_w) / 2.0f;
                float src_offset_y = (src_h * scale_y - dev_h) / 2.0f;
                src_x = src_x0 + (dx + src_offset_x) / scale_x;
                src_y = src_y0 + (dy + src_offset_y) / scale_y;
            } else {
                src_x = src_x0 + dx / scale_x;
                src_y = src_y0 + dy / scale_y;
            }
            
            // 边界检查
            if (src_x < 0 || src_x >= image->width || 
                src_y < 0 || src_y >= image->height) {
                continue;
            }
            
            // 处理透明通道：采样区域内检查 alpha
            if (image->has_alpha && image->alpha) {
                uint8_t alpha;
                if (use_area_sampling) {
                    // 区域平均 alpha
                    float ax0 = src_x;
                    float ay0 = src_y;
                    float ax1 = src_x + (1.0f / scale_x);
                    float ay1 = src_y + (1.0f / scale_y);
                    if (ax0 < 0) ax0 = 0;
                    if (ay0 < 0) ay0 = 0;
                    if (ax1 > image->width) ax1 = image->width;
                    if (ay1 > image->height) ay1 = image->height;
                    
                    uint32_t a_sum = 0, count = 0;
                    for (int sy = (int)ay0; sy < (int)ay1 && sy < image->height; sy++) {
                        for (int sx = (int)ax0; sx < (int)ax1 && sx < image->width; sx++) {
                            a_sum += image->alpha[sy * image->width + sx];
                            count++;
                        }
                    }
                    alpha = count > 0 ? (a_sum / count) : 0;
                } else {
                    // 最近邻 alpha
                    int ix = (int)(src_x + 0.5f);
                    int iy = (int)(src_y + 0.5f);
                    if (ix >= image->width) ix = image->width - 1;
                    if (iy >= image->height) iy = image->height - 1;
                    alpha = image->alpha[iy * image->width + ix];
                }
                
                // 跳过透明像素（alpha < 128 视为透明）
                if (alpha < 128) {
                    continue;
                }
            }
            
            ts_led_rgb_t px;
            if (use_area_sampling) {
                px = get_scaled_pixel_area(image->pixels, image->width, image->height,
                                           src_x, src_y, scale_x, scale_y);
            } else {
                px = get_scaled_pixel_bilinear(image->pixels, image->width, image->height,
                                                src_x, src_y);
            }
            
            if (opts.brightness < 255) {
                px = ts_led_scale_color(px, opts.brightness);
            }
            ts_led_set_pixel_xy(layer, px_x, px_y, px);
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_led_image_display_frame(ts_led_layer_t layer, ts_led_image_t image,
                                      uint16_t frame,
                                      const ts_led_image_options_t *options)
{
    if (!layer || !image) return ESP_ERR_INVALID_ARG;
    
    /* Switch to the requested frame */
    if (image->frames && frame < image->frame_count) {
        image->pixels = image->frames[frame];
        image->current_frame = frame;
    }
    
    return ts_led_image_display(layer, image, options);
}

/**
 * @brief GIF animation effect function - called by the effect system
 */
static void gif_animation_effect(ts_led_layer_t layer, uint32_t time_ms, void *user_data)
{
    (void)time_ms;  /* Use our own timing */
    ts_led_anim_ctx_t *ctx = (ts_led_anim_ctx_t *)user_data;
    if (!ctx || !ctx->image) return;
    
    ts_led_image_t img = ctx->image;
    
    /* Get current time */
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    /* Check if it's time for next frame */
    uint32_t delay = img->frame_delays ? img->frame_delays[ctx->current_frame] : 100;
    if (delay < 50) delay = 100;  /* Minimum delay */
    
    if (now - ctx->last_frame_time >= delay) {
        /* Advance to next frame */
        ctx->current_frame++;
        if (ctx->current_frame >= img->frame_count) {
            ctx->current_frame = 0;  /* Loop */
        }
        ctx->last_frame_time = now;
        
        /* Display the frame */
        ts_led_image_display_frame(layer, img, ctx->current_frame, &ctx->options);
    }
}

esp_err_t ts_led_image_animate_start(ts_led_layer_t layer, ts_led_image_t image,
                                      const ts_led_image_options_t *options)
{
    if (!layer || !image) return ESP_ERR_INVALID_ARG;
    if (image->frame_count <= 1) {
        /* Single frame, just display it */
        return ts_led_image_display(layer, image, options);
    }
    
    /* Stop any existing animation */
    ts_led_image_animate_stop(layer);
    
    /* Create animation context */
    ts_led_anim_ctx_t *ctx = TS_CALLOC_PSRAM(1, sizeof(ts_led_anim_ctx_t));
    if (!ctx) return ESP_ERR_NO_MEM;
    
    ctx->image = image;
    ctx->options = options ? *options : (ts_led_image_options_t)TS_LED_IMAGE_DEFAULT_OPTIONS();
    ctx->options.scale = TS_LED_IMG_SCALE_FIT;
    /* GIF 使用 CENTER_IMAGE 模式，因为帧已经是合成的 */
    ctx->options.center = TS_LED_IMG_CENTER_IMAGE;
    ctx->current_frame = 0;
    ctx->last_frame_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ctx->layer = layer;
    
    
    /* Display first frame */
    ts_led_image_display_frame(layer, image, 0, &ctx->options);
    
    /* Create animation to drive GIF playback */
    ts_led_animation_def_t anim_def = {
        .name = "gif_anim",
        .func = gif_animation_effect,
        .user_data = ctx,
        .frame_interval_ms = 20,  /* Check every 20ms */
    };
    
    esp_err_t ret = ts_led_animation_start(layer, &anim_def);
    if (ret != ESP_OK) {
        free(ctx);
        return ret;
    }
    
    TS_LOGI(TAG, "GIF animation started: %d frames", image->frame_count);
    return ESP_OK;
}

void ts_led_image_release_animation(ts_led_layer_t layer)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    if (l->anim_fn == gif_animation_effect) {
        free(l->anim_data);
        l->anim_data = NULL;
    }
}

esp_err_t ts_led_image_animate_stop(ts_led_layer_t layer)
{
    return ts_led_animation_stop(layer);
}

bool ts_led_image_is_playing(ts_led_layer_t layer)
{
    if (!layer) return false;
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    xSemaphoreTake(l->device->mutex, portMAX_DELAY);
    bool playing = l->anim_fn == gif_animation_effect && l->anim_data != NULL;
    xSemaphoreGive(l->device->mutex);
    return playing;
}
