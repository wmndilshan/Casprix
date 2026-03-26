/**
 * Advanced Shape Drawing Implementation
 */

#include "shapes.h"
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG_TO_RAD(deg) ((deg) * M_PI / 180.0)

/* ========================================================================
 * Polygon Drawing
 * ======================================================================== */

void skia_draw_polygon(SkiaCanvas c, const float* points, int count,
                       int closed, SkiaPaint p) {
    if (!c || !points || count < 2) return;
    
    SkiaPath path = skia_path_create();
    
    skia_path_move_to(path, points[0], points[1]);
    for (int i = 1; i < count; i++) {
        skia_path_line_to(path, points[i*2], points[i*2+1]);
    }
    
    if (closed) {
        skia_path_close(path);
    }
    
    skia_canvas_draw_path(c, path, p);
    skia_path_destroy(path);
}

void skia_draw_regular_polygon(SkiaCanvas c, float cx, float cy, float radius,
                                int sides, float rotation, SkiaPaint p) {
    if (!c || sides < 3) return;
    
    float* points = (float*)malloc(sides * 2 * sizeof(float));
    float angle_step = 360.0f / sides;
    float start_angle = rotation - 90.0f;  // Start at top by default
    
    for (int i = 0; i < sides; i++) {
        float angle = DEG_TO_RAD(start_angle + i * angle_step);
        points[i*2] = cx + radius * cosf(angle);
        points[i*2+1] = cy + radius * sinf(angle);
    }
    
    skia_draw_polygon(c, points, sides, 1, p);
    free(points);
}

/* ========================================================================
 * Star Shapes
 * ======================================================================== */

void skia_draw_star(SkiaCanvas c, float cx, float cy,
                    float outer_radius, float inner_radius,
                    int points, float rotation, SkiaPaint p) {
    if (!c || points < 3) return;
    
    SkiaPath path = skia_path_create();
    
    float angle_step = 360.0f / (points * 2);
    float start_angle = rotation - 90.0f;
    
    for (int i = 0; i < points * 2; i++) {
        float angle = DEG_TO_RAD(start_angle + i * angle_step);
        float r = (i % 2 == 0) ? outer_radius : inner_radius;
        float x = cx + r * cosf(angle);
        float y = cy + r * sinf(angle);
        
        if (i == 0) {
            skia_path_move_to(path, x, y);
        } else {
            skia_path_line_to(path, x, y);
        }
    }
    
    skia_path_close(path);
    skia_canvas_draw_path(c, path, p);
    skia_path_destroy(path);
}

/* ========================================================================
 * Arc Segments
 * ======================================================================== */

void skia_draw_arc(SkiaCanvas c, float cx, float cy, float radius,
                   float start_angle, float sweep_angle, SkiaPaint p) {
    if (!c) return;
    
    SkiaPath path = skia_path_create();
    
    // Convert angles to radians
    float start_rad = DEG_TO_RAD(start_angle);
    float end_rad = DEG_TO_RAD(start_angle + sweep_angle);
    
    // Move to start point on arc
    float start_x = cx + radius * cosf(start_rad);
    float start_y = cy + radius * sinf(start_rad);
    skia_path_move_to(path, start_x, start_y);
    
    // Approximate arc with line segments (8 segments per 90 degrees)
    int segments = (int)(fabsf(sweep_angle) / 90.0f * 8) + 1;
    if (segments < 4) segments = 4;
    
    float angle_inc = (end_rad - start_rad) / segments;
    for (int i = 1; i <= segments; i++) {
        float angle = start_rad + i * angle_inc;
        float x = cx + radius * cosf(angle);
        float y = cy + radius * sinf(angle);
        skia_path_line_to(path, x, y);
    }
    
    skia_canvas_draw_path(c, path, p);
    skia_path_destroy(path);
}

void skia_draw_pie(SkiaCanvas c, float cx, float cy, float radius,
                   float start_angle, float sweep_angle, SkiaPaint p) {
    if (!c) return;
    
    SkiaPath path = skia_path_create();
    
    // Start at center
    skia_path_move_to(path, cx, cy);
    
    // Line to start of arc
    float start_rad = DEG_TO_RAD(start_angle);
    float start_x = cx + radius * cosf(start_rad);
    float start_y = cy + radius * sinf(start_rad);
    skia_path_line_to(path, start_x, start_y);
    
    // Arc segment
    float end_rad = DEG_TO_RAD(start_angle + sweep_angle);
    int segments = (int)(fabsf(sweep_angle) / 90.0f * 8) + 1;
    if (segments < 4) segments = 4;
    
    float angle_inc = (end_rad - start_rad) / segments;
    for (int i = 1; i <= segments; i++) {
        float angle = start_rad + i * angle_inc;
        float x = cx + radius * cosf(angle);
        float y = cy + radius * sinf(angle);
        skia_path_line_to(path, x, y);
    }
    
    // Close back to center
    skia_path_close(path);
    
    skia_canvas_draw_path(c, path, p);
    skia_path_destroy(path);
}

/* ========================================================================
 * Rounded Shapes
 * ======================================================================== */

void skia_draw_rounded_polygon(SkiaCanvas c, const float* points, int count,
                                float corner_radius, SkiaPaint p) {
    if (!c || !points || count < 3 || corner_radius <= 0) {
        // Fallback to regular polygon
        skia_draw_polygon(c, points, count, 1, p);
        return;
    }
    
    SkiaPath path = skia_path_create();
    
    for (int i = 0; i < count; i++) {
        int prev = (i == 0) ? count - 1 : i - 1;
        int next = (i + 1) % count;
        
        float x0 = points[prev*2], y0 = points[prev*2+1];
        float x1 = points[i*2],    y1 = points[i*2+1];
        float x2 = points[next*2], y2 = points[next*2+1];
        
        // Calculate distances
        float dx1 = x1 - x0, dy1 = y1 - y0;
        float dx2 = x2 - x1, dy2 = y2 - y1;
        float len1 = sqrtf(dx1*dx1 + dy1*dy1);
        float len2 = sqrtf(dx2*dx2 + dy2*dy2);
        
        if (len1 < 0.01f || len2 < 0.01f) continue;
        
        // Normalize
        dx1 /= len1; dy1 /= len1;
        dx2 /= len2; dy2 /= len2;
        
        // Start and end points of rounded corner
        float r = (corner_radius < len1/2 && corner_radius < len2/2) ? corner_radius : 
                  (len1 < len2 ? len1/2 : len2/2);
        
        float sx = x1 - dx1 * r;
        float sy = y1 - dy1 * r;
        float ex = x1 + dx2 * r;
        float ey = y1 + dy2 * r;
        
        if (i == 0) {
            skia_path_move_to(path, sx, sy);
        } else {
            skia_path_line_to(path, sx, sy);
        }
        
        // Quadratic bezier for rounded corner
        skia_path_quad_to(path, x1, y1, ex, ey);
    }
    
    skia_path_close(path);
    skia_canvas_draw_path(c, path, p);
    skia_path_destroy(path);
}

/* ========================================================================
 * Arrows & Indicators
 * ======================================================================== */

void skia_draw_arrow(SkiaCanvas c, float x1, float y1, float x2, float y2,
                     float head_length, float head_width, SkiaPaint p) {
    if (!c) return;
    
    // Line body
    skia_canvas_draw_line(c, x1, y1, x2, y2, p);
    
    // Calculate arrow direction
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx*dx + dy*dy);
    
    if (len < 0.01f) return;
    
    // Normalize
    dx /= len;
    dy /= len;
    
    // Perpendicular vector
    float perp_x = -dy;
    float perp_y = dx;
    
    // Arrowhead points
    float base_x = x2 - dx * head_length;
    float base_y = y2 - dy * head_length;
    
    float p1_x = base_x + perp_x * head_width / 2;
    float p1_y = base_y + perp_y * head_width / 2;
    float p2_x = base_x - perp_x * head_width / 2;
    float p2_y = base_y - perp_y * head_width / 2;
    
    SkiaPath head = skia_path_create();
    skia_path_move_to(head, x2, y2);
    skia_path_line_to(head, p1_x, p1_y);
    skia_path_line_to(head, p2_x, p2_y);
    skia_path_close(head);
    
    skia_canvas_draw_path(c, head, p);
    skia_path_destroy(head);
}

/* ========================================================================
 * Grid & Patterns
 * ======================================================================== */

void skia_draw_grid(SkiaCanvas c, float x, float y, float width, float height,
                    float cell_w, float cell_h, SkiaPaint p) {
    if (!c || cell_w <= 0 || cell_h <= 0) return;
    
    // Vertical lines
    for (float cx = x; cx <= x + width; cx += cell_w) {
        skia_canvas_draw_line(c, cx, y, cx, y + height, p);
    }
    
    // Horizontal lines
    for (float cy = y; cy <= y + height; cy += cell_h) {
        skia_canvas_draw_line(c, x, cy, x + width, cy, p);
    }
}
