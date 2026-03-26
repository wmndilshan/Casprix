/*
 * Text Rendering System — Font management, multi-line layout, caching
 *
 * Features:
 *   - Font cache by family+size+style
 *   - Multi-line text layout with word wrapping
 *   - Text blob caching (avoid re-shaping every frame)
 *   - UTF-8 text support via Skia
 */

#ifndef TEXT_H
#define TEXT_H

#include "skia_c.h"
#include "scene_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Font Manager — Cache fonts by family/size/style
 * ======================================================================== */

#define FONT_MAX_CACHED 64

typedef struct {
    char family[64];
    float size;
    int bold;
    int italic;
    SkiaFont font;
} FontCacheEntry;

typedef struct {
    FontCacheEntry entries[FONT_MAX_CACHED];
    int count;
} FontManager;

FontManager* font_manager_create(void);
void         font_manager_destroy(FontManager* fm);

/* Get or create a font. Returns cached if already loaded. */
SkiaFont font_manager_get(FontManager* fm, const char* family, float size,
                            int bold, int italic);

/* Get font with just family and size (normal weight) */
SkiaFont font_manager_get_simple(FontManager* fm, const char* family, float size);

/* ========================================================================
 * Multi-line Text Layout
 * ======================================================================== */

#define TEXT_MAX_LINES 256

typedef struct {
    /* Input */
    const char* text;             /* Original text (not owned) */
    SkiaFont font;

    /* Computed lines */
    int line_starts[TEXT_MAX_LINES];   /* Byte offset of each line start */
    int line_lengths[TEXT_MAX_LINES];  /* Byte length of each line */
    float line_widths[TEXT_MAX_LINES]; /* Pixel width of each line */
    int line_count;

    /* Metrics */
    float max_width;              /* Widest line */
    float total_height;           /* Total height of all lines */
    float line_height;            /* Per-line height (ascent + descent + leading) */
} TextLayout;

/* Create a text layout that fits within max_width.
 * Performs word-wrapping. If max_width <= 0, no wrapping. */
void text_layout_compute(TextLayout* layout, const char* text, SkiaFont font,
                          float max_width);

/* Render the text layout at position (x, y).
 * alignment: 0=left, 1=center, 2=right. */
void text_layout_render(TextLayout* layout, SkiaCanvas canvas,
                         float x, float y, int alignment, SkiaPaint paint);

/* Get the bounding size of the text layout */
SGSize text_layout_get_size(TextLayout* layout);

/* ========================================================================
 * Text Blob Cache — Avoid re-shaping text every frame
 * ======================================================================== */

#define TEXT_CACHE_SIZE 128

typedef struct {
    uint32_t hash;
    char* text;
    SkiaFont font;
    SkiaTextBlob blob;
} TextCacheEntry;

typedef struct {
    TextCacheEntry entries[TEXT_CACHE_SIZE];
    int count;
} TextCache;

TextCache*   text_cache_create(void);
void         text_cache_destroy(TextCache* cache);

/* Get or create a text blob for the given text+font combo.
 * Returns cached blob if available. */
SkiaTextBlob text_cache_get(TextCache* cache, const char* text, SkiaFont font);

/* Invalidate all cached blobs */
void text_cache_clear(TextCache* cache);

/* ========================================================================
 * Text Measurement Utilities
 * ======================================================================== */

/* Get cursor X position for character index in text */
float text_get_cursor_x(SkiaFont font, const char* text, int char_index);

/* Get character index from X position in text */
int text_get_char_at_x(SkiaFont font, const char* text, float x);

/* Count visible characters that fit within max_width */
int text_count_fitting_chars(SkiaFont font, const char* text, float max_width);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_H */
