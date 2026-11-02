/**
 * @file ts_led_font.c
 * @brief TianShanOS LED Font Management Implementation
 * 
 * 字体缓存和结构优先分配到 PSRAM
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_led_font.h"
#include "ts_led_text.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM, TS_CALLOC_PSRAM */
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ts_font";

/*===========================================================================*/
/*                          Internal Functions                                */
/*===========================================================================*/

/**
 * @brief Binary search for glyph index entry
 */
static esp_err_t find_glyph_index(ts_font_t *font, uint16_t codepoint, 
                                   ts_font_index_entry_t *entry)
{
    // Check ASCII cache first
    if (codepoint >= 0x20 && codepoint <= 0x7E && font->ascii_index) {
        int idx = codepoint - 0x20;
        if (font->ascii_index[idx].codepoint == codepoint) {
            *entry = font->ascii_index[idx];
            return ESP_OK;
        }
    }
    
    // Binary search in file
    uint32_t left = 0;
    uint32_t right = font->header.glyph_count;
    
    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        
        // Seek to index entry
        uint32_t entry_offset = font->header.index_offset + mid * sizeof(ts_font_index_entry_t);
        if (fseek(font->fp, entry_offset, SEEK_SET) != 0) {
            return ESP_ERR_INVALID_STATE;
        }
        
        ts_font_index_entry_t temp;
        if (fread(&temp, sizeof(temp), 1, font->fp) != 1) {
            return ESP_ERR_INVALID_STATE;
        }
        
        if (temp.codepoint == codepoint) {
            *entry = temp;
            return ESP_OK;
        } else if (temp.codepoint < codepoint) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Read glyph bitmap from file
 */
static esp_err_t read_glyph_bitmap(ts_font_t *font, uint32_t offset, uint8_t *bitmap)
{
    uint8_t bytes_per_glyph = (font->header.width * font->header.height + 7) / 8;
    
    if (fseek(font->fp, offset, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (fread(bitmap, 1, bytes_per_glyph, font->fp) != bytes_per_glyph) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return ESP_OK;
}

/**
 * @brief Find LRU cache slot
 */
static int find_cache_slot(ts_font_t *font, uint16_t codepoint)
{
    if (!font->cache || font->cache_size == 0) {
        return -1;
    }
    
    // Look for existing entry
    for (int i = 0; i < font->cache_count; i++) {
        if (font->cache[i].codepoint == codepoint) {
            return i;
        }
    }
    
    return -1;
}

/**
 * @brief Get or allocate cache slot (LRU eviction)
 */
static int alloc_cache_slot(ts_font_t *font)
{
    if (!font->cache || font->cache_size == 0) {
        return -1;
    }
    
    // If cache not full, use next slot
    if (font->cache_count < font->cache_size) {
        return font->cache_count++;
    }
    
    // Find least recently used
    int lru_idx = 0;
    uint32_t lru_time = font->cache[0].last_used;
    
    for (int i = 1; i < font->cache_size; i++) {
        if (font->cache[i].last_used < lru_time) {
            lru_time = font->cache[i].last_used;
            lru_idx = i;
        }
    }
    
    return lru_idx;
}

/**
 * @brief Pre-cache ASCII glyphs
 */
static void precache_ascii(ts_font_t *font)
{
    if (!font->ascii_index) {
        font->ascii_index = TS_MALLOC_PSRAM(95 * sizeof(ts_font_index_entry_t));
        if (!font->ascii_index) {
            ESP_LOGW(TAG, "Failed to allocate ASCII index cache");
            return;
        }
    }
    
    // Read all ASCII index entries
    int loaded = 0;
    for (uint16_t cp = 0x20; cp <= 0x7E; cp++) {
        ts_font_index_entry_t entry;
        if (find_glyph_index(font, cp, &entry) == ESP_OK) {
            font->ascii_index[cp - 0x20] = entry;
            loaded++;
        } else {
            // Mark as invalid
            font->ascii_index[cp - 0x20].codepoint = 0;
            font->ascii_index[cp - 0x20].offset = 0;
        }
    }
    
    ESP_LOGI(TAG, "Pre-cached %d ASCII glyphs", loaded);
}

/*===========================================================================*/
/*                              Public API                                    */
/*===========================================================================*/

ts_font_t *ts_font_load(const char *path, const ts_font_config_t *config)
{
    if (!path) {
        ESP_LOGE(TAG, "Invalid path");
        return NULL;
    }
    
    // Use default config if not provided
    ts_font_config_t cfg = TS_FONT_DEFAULT_CONFIG();
    if (config) {
        cfg = *config;
    }
    
    // Allocate font structure
    ts_font_t *font = TS_CALLOC_PSRAM(1, sizeof(ts_font_t));
    if (!font) {
        ESP_LOGE(TAG, "Failed to allocate font structure");
        return NULL;
    }
    
    // Copy path
    strncpy(font->path, path, TS_FONT_PATH_MAX - 1);
    
    // Open file
    font->fp = fopen(path, "rb");
    if (!font->fp) {
        ESP_LOGE(TAG, "Failed to open font file: %s", path);
        free(font);
        return NULL;
    }
    
    // Read header
    if (fread(&font->header, sizeof(ts_font_header_t), 1, font->fp) != 1) {
        ESP_LOGE(TAG, "Failed to read font header");
        fclose(font->fp);
        free(font);
        return NULL;
    }
    
    // Validate magic
    if (memcmp(font->header.magic, TS_FONT_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "Invalid font magic (not a .fnt file)");
        fclose(font->fp);
        free(font);
        return NULL;
    }
    
    // Validate dimensions
    if (font->header.width > TS_FONT_MAX_SIZE || 
        font->header.height > TS_FONT_MAX_SIZE) {
        ESP_LOGE(TAG, "Font too large: %dx%d (max %d)", 
                 font->header.width, font->header.height, TS_FONT_MAX_SIZE);
        fclose(font->fp);
        free(font);
        return NULL;
    }
    
    // Allocate glyph cache
    if (cfg.cache_size > 0) {
        font->cache = TS_CALLOC_PSRAM(cfg.cache_size, sizeof(ts_font_glyph_cache_t));
        if (font->cache) {
            font->cache_size = cfg.cache_size;
            ESP_LOGD(TAG, "Allocated cache for %d glyphs");
        } else {
            ESP_LOGW(TAG, "Failed to allocate glyph cache");
        }
    }
    
    // Pre-cache ASCII if requested
    if (cfg.cache_ascii) {
        precache_ascii(font);
    }
    
    ESP_LOGI(TAG, "Loaded font: %s (%dx%d, %lu glyphs)", 
             path, font->header.width, font->header.height, 
             (unsigned long)font->header.glyph_count);
    
    return font;
}

void ts_font_unload(ts_font_t *font)
{
    if (!font) return;
    ts_led_text_overlay_release_font(font);
    
    if (font->fp) {
        fclose(font->fp);
    }
    
    if (font->ascii_index) {
        free(font->ascii_index);
    }
    
    if (font->cache) {
        free(font->cache);
    }
    
    ESP_LOGI(TAG, "Unloaded font: %s (hits=%lu, misses=%lu)", 
             font->path, (unsigned long)font->cache_hits, 
             (unsigned long)font->cache_misses);
    
    free(font);
}

esp_err_t ts_font_get_glyph(ts_font_t *font, uint16_t codepoint, 
                             const uint8_t **bitmap)
{
    if (!font || !bitmap) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check cache first
    int cache_idx = find_cache_slot(font, codepoint);
    if (cache_idx >= 0) {
        font->cache[cache_idx].last_used = (uint32_t)(esp_timer_get_time() / 1000);
        *bitmap = font->cache[cache_idx].bitmap;
        font->cache_hits++;
        return ESP_OK;
    }
    
    // Find glyph in index
    ts_font_index_entry_t entry;
    esp_err_t err = find_glyph_index(font, codepoint, &entry);
    if (err != ESP_OK) {
        font->cache_misses++;
        return err;
    }
    
    // Allocate cache slot
    cache_idx = alloc_cache_slot(font);
    if (cache_idx < 0) {
        // No cache, use static buffer (not thread-safe)
        static uint8_t static_bitmap[32];
        err = read_glyph_bitmap(font, entry.offset, static_bitmap);
        if (err == ESP_OK) {
            *bitmap = static_bitmap;
        }
        font->cache_misses++;
        return err;
    }
    
    // Read bitmap into cache
    err = read_glyph_bitmap(font, entry.offset, font->cache[cache_idx].bitmap);
    if (err != ESP_OK) {
        font->cache_misses++;
        return err;
    }
    
    font->cache[cache_idx].codepoint = codepoint;
    font->cache[cache_idx].last_used = (uint32_t)(esp_timer_get_time() / 1000);
    *bitmap = font->cache[cache_idx].bitmap;
    font->cache_misses++;  // First load counts as miss
    
    return ESP_OK;
}

bool ts_font_has_glyph(ts_font_t *font, uint16_t codepoint)
{
    if (!font) return false;
    
    ts_font_index_entry_t entry;
    return find_glyph_index(font, codepoint, &entry) == ESP_OK;
}

esp_err_t ts_font_get_size(ts_font_t *font, uint8_t *width, uint8_t *height)
{
    if (!font) return ESP_ERR_INVALID_ARG;
    
    if (width) *width = font->header.width;
    if (height) *height = font->header.height;
    
    return ESP_OK;
}

uint32_t ts_font_get_glyph_count(ts_font_t *font)
{
    return font ? font->header.glyph_count : 0;
}

void ts_font_clear_cache(ts_font_t *font)
{
    if (!font || !font->cache) return;
    
    font->cache_count = 0;
    memset(font->cache, 0, font->cache_size * sizeof(ts_font_glyph_cache_t));
}

void ts_font_get_stats(ts_font_t *font, uint32_t *hits, uint32_t *misses)
{
    if (!font) return;
    
    if (hits) *hits = font->cache_hits;
    if (misses) *misses = font->cache_misses;
}

size_t ts_font_get_memory_usage(void)
{
    // TODO: Track globally if needed
    return 0;
}
