/*
 * Text Rendering System — Font management, multi-line layout, caching
 */

#include "text.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================
 * Font Manager
 * ======================================================================== */

FontManager* font_manager_create(void) {
    FontManager* fm = (FontManager*)calloc(1, sizeof(FontManager));
    return fm;
}

void font_manager_destroy(FontManager* fm) {
    if (!fm) return;
    for (int i = 0; i < fm->count; i++) {
        if (fm->entries[i].font) {
            skia_font_destroy(fm->entries[i].font);
        }
    }
    free(fm);
}

SkiaFont font_manager_get(FontManager* fm, const char* family, float size,
                            int bold, int italic) {
    if (!fm || !family) return NULL;

    /* Search cache */
    for (int i = 0; i < fm->count; i++) {
        FontCacheEntry* e = &fm->entries[i];
        if (strcmp(e->family, family) == 0 &&
            fabsf(e->size - size) < 0.01f &&
            e->bold == bold && e->italic == italic) {
            return e->font;
        }
    }

    /* Create new font */
    if (fm->count >= FONT_MAX_CACHED) {
        /* Evict oldest entry */
        skia_font_destroy(fm->entries[0].font);
        memmove(&fm->entries[0], &fm->entries[1],
                (FONT_MAX_CACHED - 1) * sizeof(FontCacheEntry));
        fm->count--;
    }

    SkiaFont font = skia_font_create(family, size);
    if (!font) return NULL;

    if (bold) skia_font_set_bold(font, 1);
    if (italic) skia_font_set_italic(font, 1);

    FontCacheEntry* entry = &fm->entries[fm->count];
    strncpy(entry->family, family, 63);
    entry->family[63] = '\0';
    entry->size = size;
    entry->bold = bold;
    entry->italic = italic;
    entry->font = font;
    fm->count++;

    return font;
}

SkiaFont font_manager_get_simple(FontManager* fm, const char* family, float size) {
    return font_manager_get(fm, family, size, 0, 0);
}

/* ========================================================================
 * Multi-line Text Layout
 * ======================================================================== */

/* Check if a character is a word break point */
static int is_breakable(char c) {
    return c == ' ' || c == '\t' || c == '-';
}

void text_layout_compute(TextLayout* layout, const char* text, SkiaFont font,
                          float max_width) {
    if (!layout || !text || !font) return;

    memset(layout, 0, sizeof(TextLayout));
    layout->text = text;
    layout->font = font;
    layout->line_height = skia_font_get_height(font);

    int text_len = (int)strlen(text);
    if (text_len == 0) {
        layout->total_height = layout->line_height;
        return;
    }

    int line_start = 0;
    int last_break = -1;
    float line_width = 0;

    /* Measure character widths */
    float* char_widths = (float*)malloc(text_len * sizeof(float));
    skia_font_get_char_widths(font, text, char_widths);

    for (int i = 0; i < text_len && layout->line_count < TEXT_MAX_LINES; i++) {
        /* Hard line break */
        if (text[i] == '\n') {
            layout->line_starts[layout->line_count] = line_start;
            layout->line_lengths[layout->line_count] = i - line_start;
            layout->line_widths[layout->line_count] = line_width;
            if (line_width > layout->max_width) layout->max_width = line_width;
            layout->line_count++;

            line_start = i + 1;
            last_break = -1;
            line_width = 0;
            continue;
        }

        if (is_breakable(text[i])) {
            last_break = i;
        }

        line_width += char_widths[i];

        /* Soft wrap */
        if (max_width > 0 && line_width > max_width && i > line_start) {
            int break_at;
            if (last_break > line_start) {
                break_at = last_break + 1; /* Break after space */
            } else {
                break_at = i; /* Force break mid-word */
            }

            /* Recalculate line width up to break point */
            float actual_width = 0;
            for (int j = line_start; j < break_at; j++) {
                actual_width += char_widths[j];
            }

            layout->line_starts[layout->line_count] = line_start;
            layout->line_lengths[layout->line_count] = break_at - line_start;
            layout->line_widths[layout->line_count] = actual_width;
            if (actual_width > layout->max_width) layout->max_width = actual_width;
            layout->line_count++;

            line_start = break_at;
            /* Skip leading spaces on next line */
            while (line_start < text_len && text[line_start] == ' ') {
                line_start++;
            }
            i = line_start - 1; /* Will be incremented by loop */
            last_break = -1;
            line_width = 0;
        }
    }

    /* Add final line */
    if (line_start < text_len && layout->line_count < TEXT_MAX_LINES) {
        /* Recalculate remaining width */
        float remaining_width = 0;
        for (int j = line_start; j < text_len; j++) {
            remaining_width += char_widths[j];
        }
        layout->line_starts[layout->line_count] = line_start;
        layout->line_lengths[layout->line_count] = text_len - line_start;
        layout->line_widths[layout->line_count] = remaining_width;
        if (remaining_width > layout->max_width) layout->max_width = remaining_width;
        layout->line_count++;
    }

    layout->total_height = layout->line_count * layout->line_height;

    free(char_widths);
}

void text_layout_render(TextLayout* layout, SkiaCanvas canvas,
                         float x, float y, int alignment, SkiaPaint paint) {
    if (!layout || !canvas || !layout->font || layout->line_count == 0) return;

    float ascent = -skia_font_get_ascent(layout->font); /* Ascent is negative in Skia */
    float cy = y + ascent;

    /* Temporary buffer for line extraction */
    char line_buf[4096];

    for (int i = 0; i < layout->line_count; i++) {
        int start = layout->line_starts[i];
        int len = layout->line_lengths[i];
        if (len <= 0) {
            cy += layout->line_height;
            continue;
        }
        if (len >= 4095) len = 4095;

        memcpy(line_buf, layout->text + start, len);
        line_buf[len] = '\0';

        float lx = x;
        switch (alignment) {
            case SG_TEXT_ALIGN_CENTER:
                lx = x + (layout->max_width - layout->line_widths[i]) / 2.0f;
                break;
            case SG_TEXT_ALIGN_RIGHT:
                lx = x + layout->max_width - layout->line_widths[i];
                break;
            default:
                break;
        }

        skia_canvas_draw_text(canvas, line_buf, lx, cy, layout->font, paint);
        cy += layout->line_height;
    }
}

SGSize text_layout_get_size(TextLayout* layout) {
    SGSize size = { 0, 0 };
    if (!layout) return size;
    size.w = layout->max_width;
    size.h = layout->total_height;
    return size;
}

/* ========================================================================
 * Text Blob Cache
 * ======================================================================== */

static uint32_t text_hash(const char* text, SkiaFont font) {
    /* FNV-1a hash */
    uint32_t hash = 2166136261u;
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= 16777619u;
    }
    /* Mix in font pointer */
    uintptr_t fp = (uintptr_t)font;
    hash ^= (uint32_t)(fp & 0xFFFFFFFF);
    hash *= 16777619u;
    return hash;
}

TextCache* text_cache_create(void) {
    return (TextCache*)calloc(1, sizeof(TextCache));
}

void text_cache_destroy(TextCache* cache) {
    if (!cache) return;
    text_cache_clear(cache);
    free(cache);
}

SkiaTextBlob text_cache_get(TextCache* cache, const char* text, SkiaFont font) {
    if (!cache || !text || !font) return NULL;

    uint32_t hash = text_hash(text, font);

    /* Search existing entries */
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].hash == hash &&
            cache->entries[i].font == font &&
            strcmp(cache->entries[i].text, text) == 0) {
            return cache->entries[i].blob;
        }
    }

    /* Create new blob */
    SkiaTextBlob blob = skia_text_blob_make(text, font);
    if (!blob) return NULL;

    /* Insert into cache */
    if (cache->count >= TEXT_CACHE_SIZE) {
        /* Evict oldest */
        free(cache->entries[0].text);
        skia_text_blob_destroy(cache->entries[0].blob);
        memmove(&cache->entries[0], &cache->entries[1],
                (TEXT_CACHE_SIZE - 1) * sizeof(TextCacheEntry));
        cache->count--;
    }

    TextCacheEntry* entry = &cache->entries[cache->count];
    entry->hash = hash;
    entry->text = (char*)malloc(strlen(text) + 1);
    strcpy(entry->text, text);
    entry->font = font;
    entry->blob = blob;
    cache->count++;

    return blob;
}

void text_cache_clear(TextCache* cache) {
    if (!cache) return;
    for (int i = 0; i < cache->count; i++) {
        free(cache->entries[i].text);
        if (cache->entries[i].blob) {
            skia_text_blob_destroy(cache->entries[i].blob);
        }
    }
    cache->count = 0;
}

/* ========================================================================
 * Text Measurement Utilities
 * ======================================================================== */

float text_get_cursor_x(SkiaFont font, const char* text, int char_index) {
    if (!font || !text) return 0;

    int len = (int)strlen(text);
    if (char_index <= 0) return 0;
    if (char_index > len) char_index = len;

    float* widths = (float*)malloc(len * sizeof(float));
    skia_font_get_char_widths(font, text, widths);

    float x = 0;
    for (int i = 0; i < char_index; i++) {
        x += widths[i];
    }

    free(widths);
    return x;
}

int text_get_char_at_x(SkiaFont font, const char* text, float x) {
    if (!font || !text || x <= 0) return 0;

    int len = (int)strlen(text);
    if (len == 0) return 0;

    float* widths = (float*)malloc(len * sizeof(float));
    skia_font_get_char_widths(font, text, widths);

    float accum = 0;
    int result = len;
    for (int i = 0; i < len; i++) {
        if (accum + widths[i] / 2.0f > x) {
            result = i;
            break;
        }
        accum += widths[i];
    }

    free(widths);
    return result;
}

int text_count_fitting_chars(SkiaFont font, const char* text, float max_width) {
    if (!font || !text || max_width <= 0) return 0;

    int len = (int)strlen(text);
    if (len == 0) return 0;

    float* widths = (float*)malloc(len * sizeof(float));
    skia_font_get_char_widths(font, text, widths);

    float accum = 0;
    int count = 0;
    for (int i = 0; i < len; i++) {
        if (accum + widths[i] > max_width) break;
        accum += widths[i];
        count++;
    }

    free(widths);
    return count;
}
