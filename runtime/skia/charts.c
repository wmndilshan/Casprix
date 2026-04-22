#define _POSIX_C_SOURCE 200809L
/**
 * Chart Widgets Implementation
 */

#include "charts.h"
#include "widgets.h"
#include "shapes.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================
 * Line Chart Implementation
 * ======================================================================== */

static void line_chart_custom_draw(SkiaCanvas c, float w, float h, void* data) {
    LineChartState* state = (LineChartState*)data;
    if (!state || !state->values || state->count < 2) return;
    
    SkiaPaint paint = skia_paint_create();
    skia_paint_set_antialias(paint, 1);
    
    /* Draw grid */
    if (state->show_grid) {
        skia_paint_set_style(paint, 1);
        skia_paint_set_stroke_width(paint, 1.0f);
        skia_paint_set_color(paint, state->grid_color);
        skia_draw_grid(c, 0, 0, w, h, w/10, h/5, paint);
    }
    
    /* Calculate scale */
    float range = state->max - state->min;
    if (range < 0.001f) range = 1.0f;
    
    float x_step = w / (state->count - 1);
    
    /* Draw fill under line */
    if (state->fill_color != 0) {
        SkiaPath fill_path = skia_path_create();
        skia_path_move_to(fill_path, 0, h);
        
        for (int i = 0; i < state->count; i++) {
            float x = i * x_step;
            float y = h - ((state->values[i] - state->min) / range) * h;
            skia_path_line_to(fill_path, x, y);
        }
        
        skia_path_line_to(fill_path, (state->count - 1) * x_step, h);
        skia_path_close(fill_path);
        
        skia_paint_set_style(paint, 0);
        skia_paint_set_color(paint, state->fill_color);
        skia_canvas_draw_path(c, fill_path, paint);
        skia_path_destroy(fill_path);
    }
    
    /* Draw line */
    SkiaPath line_path = skia_path_create();
    for (int i = 0; i < state->count; i++) {
        float x = i * x_step;
        float y = h - ((state->values[i] - state->min) / range) * h;
        
        if (i == 0) {
            skia_path_move_to(line_path, x, y);
        } else {
            skia_path_line_to(line_path, x, y);
        }
    }
    
    skia_paint_set_style(paint, 1);
    skia_paint_set_stroke_width(paint, 2.0f);
    skia_paint_set_color(paint, state->line_color);
    skia_canvas_draw_path(c, line_path, paint);
    skia_path_destroy(line_path);
    
    /* Draw points */
    if (state->show_points) {
        skia_paint_set_style(paint, 0);
        skia_paint_set_color(paint, state->line_color);
        
        for (int i = 0; i < state->count; i++) {
            float x = i * x_step;
            float y = h - ((state->values[i] - state->min) / range) * h;
            skia_canvas_draw_circle(c, x, y, state->point_radius, paint);
        }
    }
    
    skia_paint_destroy(paint);
}

static void line_chart_destroy_state(void* data) {
    LineChartState* state = (LineChartState*)data;

    if (!state) return;
    free(state->values);
    free(state);
}

SGNode* widget_line_chart(float* data, int count, float min_val, float max_val) {
    LineChartState* state = (LineChartState*)calloc(1, sizeof(LineChartState));
    state->values = (float*)malloc(count * sizeof(float));
    memcpy(state->values, data, count * sizeof(float));
    state->count = count;
    state->min = min_val;
    state->max = max_val;
    state->line_color = 0xFF3498DB;
    state->fill_color = 0x403498DB;  /* Semi-transparent */
    state->grid_color = 0x20000000;
    state->show_points = 1;
    state->show_grid = 1;
    state->point_radius = 4.0f;
    
    SGNode* canvas = widget_canvas(300, 200, line_chart_custom_draw, state);
    canvas->style.background = 0xFFFFFFFF;
    canvas->style.border_color = 0xFFDDDDDD;
    canvas->style.border_width = 1.0f;
    canvas->style.padding[0] = 10;
    canvas->style.padding[1] = 10;
    canvas->style.padding[2] = 10;
    canvas->style.padding[3] = 10;
    widget_canvas_set_data_destructor(canvas, line_chart_destroy_state);
    
    return canvas;
}

void widget_line_chart_set_data(SGNode* chart, float* data, int count) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    LineChartState* state = (LineChartState*)canvas_state->draw_data;
    if (!state) return;
    
    free(state->values);
    state->values = (float*)malloc(count * sizeof(float));
    memcpy(state->values, data, count * sizeof(float));
    state->count = count;
    
    widget_canvas_invalidate(chart);
}

void widget_line_chart_set_colors(SGNode* chart, uint32_t line, uint32_t fill, uint32_t grid) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    LineChartState* state = (LineChartState*)canvas_state->draw_data;
    if (!state) return;
    
    state->line_color = line;
    state->fill_color = fill;
    state->grid_color = grid;
    
    widget_canvas_invalidate(chart);
}

void widget_line_chart_show_points(SGNode* chart, int show, float radius) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    LineChartState* state = (LineChartState*)canvas_state->draw_data;
    if (!state) return;
    
    state->show_points = show;
    state->point_radius = radius;
    
    widget_canvas_invalidate(chart);
}

/* ========================================================================
 * Bar Chart Implementation
 * ======================================================================== */

static void bar_chart_custom_draw(SkiaCanvas c, float w, float h, void* data) {
    BarChartState* state = (BarChartState*)data;
    if (!state || !state->values || state->count < 1) return;
    
    SkiaPaint paint = skia_paint_create();
    skia_paint_set_antialias(paint, 1);
    skia_paint_set_style(paint, 0);  /* Fill */
    
    float bar_width = (w - state->bar_spacing * (state->count + 1)) / state->count;
    
    for (int i = 0; i < state->count; i++) {
        float value = state->values[i];
        float bar_height = (value / state->max) * h;
        
        float x = state->bar_spacing + i * (bar_width + state->bar_spacing);
        float y = h - bar_height;
        
        /* Draw bar */
        skia_paint_set_color(paint, state->bar_color);
        skia_canvas_draw_rrect(c, x, y, bar_width, bar_height, 4.0f, 4.0f, paint);
        
        /* Draw label */
        if (state->labels && state->labels[i]) {
            SkiaFont font = skia_font_create("Arial", 10.0f);
            skia_paint_set_color(paint, state->text_color);
            skia_canvas_draw_text(c, state->labels[i], x + bar_width/2, h + 15, font, paint);
            skia_font_destroy(font);
        }
    }
    
    skia_paint_destroy(paint);
}

static void bar_chart_destroy_state(void* data) {
    BarChartState* state = (BarChartState*)data;

    if (!state) return;
    free(state->values);
    if (state->labels) {
        for (int i = 0; i < state->count; i++) {
            free(state->labels[i]);
        }
        free(state->labels);
    }
    free(state);
}

SGNode* widget_bar_chart(float* values, const char** labels, int count) {
    BarChartState* state = (BarChartState*)calloc(1, sizeof(BarChartState));
    state->values = (float*)malloc(count * sizeof(float));
    memcpy(state->values, values, count * sizeof(float));
    state->count = count;
    
    /* Find max value */
    state->max = 0;
    for (int i = 0; i < count; i++) {
        if (values[i] > state->max) state->max = values[i];
    }
    if (state->max < 1.0f) state->max = 100.0f;
    
    /* Copy labels */
    if (labels) {
        state->labels = (char**)calloc(count, sizeof(char*));
        for (int i = 0; i < count; i++) {
            state->labels[i] = strdup(labels[i]);
        }
    }
    
    state->bar_color = 0xFF2ECC71;
    state->text_color = 0xFF333333;
    state->vertical = 1;
    state->bar_spacing = 10.0f;
    
    SGNode* canvas = widget_canvas(300, 200, bar_chart_custom_draw, state);
    canvas->style.background = 0xFFFFFFFF;
    canvas->style.border_color = 0xFFDDDDDD;
    canvas->style.border_width = 1.0f;
    canvas->style.padding[0] = 10;
    canvas->style.padding[1] = 10;
    canvas->style.padding[2] = 30;
    canvas->style.padding[3] = 10;
    widget_canvas_set_data_destructor(canvas, bar_chart_destroy_state);
    
    return canvas;
}

void widget_bar_chart_set_orientation(SGNode* chart, int vertical) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    BarChartState* state = (BarChartState*)canvas_state->draw_data;
    if (!state) return;
    
    state->vertical = vertical;
    widget_canvas_invalidate(chart);
}

void widget_bar_chart_set_data(SGNode* chart, float* values, int count) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    BarChartState* state = (BarChartState*)canvas_state->draw_data;
    if (!state) return;
    
    free(state->values);
    state->values = (float*)malloc(count * sizeof(float));
    memcpy(state->values, values, count * sizeof(float));
    state->count = count;
    
    widget_canvas_invalidate(chart);
}

void widget_bar_chart_set_colors(SGNode* chart, uint32_t bar, uint32_t text) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    BarChartState* state = (BarChartState*)canvas_state->draw_data;
    if (!state) return;
    
    state->bar_color = bar;
    state->text_color = text;
    
    widget_canvas_invalidate(chart);
}

/* ========================================================================
 * Pie Chart Implementation
 * ======================================================================== */

static uint32_t default_pie_colors[] = {
    0xFFE74C3C, 0xFF3498DB, 0xFF2ECC71, 0xFFF39C12, 0xFF9B59B6,
    0xFF1ABC9C, 0xFFE67E22, 0xFF34495E, 0xFFFF6B6B, 0xFF4ECDC4
};

static void pie_chart_custom_draw(SkiaCanvas c, float w, float h, void* data) {
    PieChartState* state = (PieChartState*)data;
    if (!state || !state->values || state->count < 1) return;
    
    SkiaPaint paint = skia_paint_create();
    skia_paint_set_antialias(paint, 1);
    skia_paint_set_style(paint, 0);  /* Fill */
    
    float cx = w / 2;
    float cy = h / 2;
    float radius = (w < h ? w : h) / 2 - 20;
    
    float current_angle = -90;  /* Start at top */
    
    for (int i = 0; i < state->count && i < 16; i++) {
        float sweep = (state->values[i] / state->total) * 360.0f;
        
        /* Calculate center with explosion */
        float explode = state->exploded[i];
        float angle_rad = (current_angle + sweep/2) * M_PI / 180.0f;
        float ecx = cx + explode * cosf(angle_rad);
        float ecy = cy + explode * sinf(angle_rad);
        
        /* Draw pie slice */
        uint32_t color = state->colors ? state->colors[i] : default_pie_colors[i % 10];
        skia_paint_set_color(paint, color);
        skia_draw_pie(c, ecx, ecy, radius, current_angle, sweep, paint);
        
        current_angle += sweep;
    }
    
    skia_paint_destroy(paint);
}

static void pie_chart_destroy_state(void* data) {
    PieChartState* state = (PieChartState*)data;

    if (!state) return;
    free(state->values);
    if (state->labels) {
        for (int i = 0; i < state->count; i++) {
            free(state->labels[i]);
        }
        free(state->labels);
    }
    free(state->colors);
    free(state);
}

SGNode* widget_pie_chart(float* values, const char** labels, int count) {
    PieChartState* state = (PieChartState*)calloc(1, sizeof(PieChartState));
    state->values = (float*)malloc(count * sizeof(float));
    memcpy(state->values, values, count * sizeof(float));
    state->count = count;
    
    /* Calculate total */
    state->total = 0;
    for (int i = 0; i < count; i++) {
        state->total += values[i];
    }
    
    /* Copy labels */
    if (labels) {
        state->labels = (char**)calloc(count, sizeof(char*));
        for (int i = 0; i < count; i++) {
            state->labels[i] = strdup(labels[i]);
        }
    }
    
    /* Initialize explosion distances */
    for (int i = 0; i < 16; i++) {
        state->exploded[i] = 0;
    }
    
    state->show_labels = 1;
    state->show_percentages = 1;
    
    SGNode* canvas = widget_canvas(250, 250, pie_chart_custom_draw, state);
    canvas->style.background = 0xFFFFFFFF;
    canvas->style.border_color = 0xFFDDDDDD;
    canvas->style.border_width = 1.0f;
    canvas->style.padding[0] = 10;
    canvas->style.padding[1] = 10;
    canvas->style.padding[2] = 10;
    canvas->style.padding[3] = 10;
    widget_canvas_set_data_destructor(canvas, pie_chart_destroy_state);
    
    return canvas;
}

void widget_pie_chart_set_exploded(SGNode* chart, int segment, float distance) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    PieChartState* state = (PieChartState*)canvas_state->draw_data;
    if (!state || segment < 0 || segment >= 16) return;
    
    state->exploded[segment] = distance;
    widget_canvas_invalidate(chart);
}

void widget_pie_chart_set_colors(SGNode* chart, const uint32_t* colors, int count) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    PieChartState* state = (PieChartState*)canvas_state->draw_data;
    if (!state) return;
    
    if (state->colors) free(state->colors);
    state->colors = (uint32_t*)malloc(count * sizeof(uint32_t));
    memcpy(state->colors, colors, count * sizeof(uint32_t));
    
    widget_canvas_invalidate(chart);
}

void widget_pie_chart_show_labels(SGNode* chart, int show, int show_percent) {
    CustomCanvasState* canvas_state = (CustomCanvasState*)chart->user_data;
    if (!canvas_state) return;
    
    PieChartState* state = (PieChartState*)canvas_state->draw_data;
    if (!state) return;
    
    state->show_labels = show;
    state->show_percentages = show_percent;
    
    widget_canvas_invalidate(chart);
}
