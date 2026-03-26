/**
 * Advanced Shape Drawing for Casperix UI
 * 
 * Higher-level shape primitives beyond basic rect/circle:
 *  - Regular/irregular polygons
 *  - Star shapes
 *  - Arc segments
 *  - Bezier helpers
 */

#ifndef CASPERIX_SHAPES_H
#define CASPERIX_SHAPES_H

#include "skia_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Polygon Drawing
 * ======================================================================== */

/**
 * Draw polygon from array of points
 * @param c Canvas
 * @param points Flat array [x0, y0, x1, y1, ...]
 * @param count Number of points (array length / 2)
 * @param closed Whether to close the path
 * @param p Paint
 */
void skia_draw_polygon(SkiaCanvas c, const float* points, int count, 
                       int closed, SkiaPaint p);

/**
 * Draw regular polygon (pentagon, hexagon, octagon, etc.)
 * @param c Canvas
 * @param cx Center X
 * @param cy Center Y
 * @param radius Circumradius (distance from center to vertex)
 * @param sides Number of sides (3=triangle, 4=square, 5=pentagon, etc.)
 * @param rotation Rotation in degrees (0=first vertex at top)
 * @param p Paint
 */
void skia_draw_regular_polygon(SkiaCanvas c, float cx, float cy, float radius,
                                int sides, float rotation, SkiaPaint p);

/* ========================================================================
 * Star Shapes
 * ======================================================================== */

/**
 * Draw star shape
 * @param c Canvas
 * @param cx Center X
 * @param cy Center Y
 * @param outer_radius Radius to outer points
 * @param inner_radius Radius to inner points
 * @param points Number of star points (5=classic 5-point star)
 * @param rotation Rotation in degrees
 * @param p Paint
 */
void skia_draw_star(SkiaCanvas c, float cx, float cy, 
                    float outer_radius, float inner_radius,
                    int points, float rotation, SkiaPaint p);

/* ========================================================================
 * Arc Segments
 * ======================================================================== */

/**
 * Draw arc (portion of circle's outline)
 * @param c Canvas
 * @param cx Center X
 * @param cy Center Y
 * @param radius Arc radius
 * @param start_angle Start angle in degrees (0=right, 90=bottom)
 * @param sweep_angle How many degrees to sweep (positive=clockwise)
 * @param p Paint (use stroke style)
 */
void skia_draw_arc(SkiaCanvas c, float cx, float cy, float radius,
                   float start_angle, float sweep_angle, SkiaPaint p);

/**
 * Draw pie slice (filled arc segment)
 * @param c Canvas
 * @param cx Center X
 * @param cy Center Y
 * @param radius Pie radius
 * @param start_angle Start angle in degrees
 * @param sweep_angle Sweep angle in degrees
 * @param p Paint (use fill style)
 */
void skia_draw_pie(SkiaCanvas c, float cx, float cy, float radius,
                   float start_angle, float sweep_angle, SkiaPaint p);

/* ========================================================================
 * Rounded Shapes
 * ======================================================================== */

/**
 * Draw rounded polygon (polygon with rounded corners)
 * @param c Canvas
 * @param points Flat array of points
 * @param count Number of points
 * @param corner_radius Radius of corner rounding
 * @param p Paint
 */
void skia_draw_rounded_polygon(SkiaCanvas c, const float* points, int count,
                                float corner_radius, SkiaPaint p);

/* ========================================================================
 * Arrows & Indicators
 * ======================================================================== */

/**
 * Draw arrow from (x1,y1) to (x2,y2)
 * @param c Canvas
 * @param x1 Start X
 * @param y1 Start Y
 * @param x2 End X
 * @param y2 End Y
 * @param head_length Length of arrowhead
 * @param head_width Width of arrowhead
 * @param p Paint
 */
void skia_draw_arrow(SkiaCanvas c, float x1, float y1, float x2, float y2,
                     float head_length, float head_width, SkiaPaint p);

/* ========================================================================
 * Grid & Patterns
 * ======================================================================== */

/**
 * Draw grid lines
 * @param c Canvas
 * @param x Grid origin X
 * @param y Grid origin Y
 * @param width Grid width
 * @param height Grid height
 * @param cell_w Cell width
 * @param cell_h Cell height
 * @param p Paint (use stroke style)
 */
void skia_draw_grid(SkiaCanvas c, float x, float y, float width, float height,
                    float cell_w, float cell_h, SkiaPaint p);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_SHAPES_H */
