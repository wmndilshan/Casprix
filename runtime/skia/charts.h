/**
 * Chart Widgets - Data Visualization for Casperix UI
 * 
 * Production-ready chart widgets:
 *  - Line charts (time series, trends)
 *  - Bar charts (horizontal/vertical)
 *  - Pie charts (proportions, segments)
 */

#ifndef CASPERIX_CHARTS_H
#define CASPERIX_CHARTS_H

#include "scene_graph.h"
#include "skia_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Line Chart
 * ======================================================================== */

typedef struct {
    float* values;                /* Data points */
    int count;                    /* Number of points */
    float min, max;               /* Value range */
    uint32_t line_color;          /* Line color */
    uint32_t fill_color;          /* Fill under line (0 = no fill) */
    uint32_t grid_color;          /* Grid lines */
    int show_points;              /* Show dots at each point */
    int show_grid;                /* Show grid lines */
    float point_radius;           /* Dot size */
} LineChartState;

/**
 * Create a line chart
 * @param data Array of Y values
 * @param count Number of data points
 * @param min_val Minimum Y value for scale
 * @param max_val Maximum Y value for scale
 */
SGNode* widget_line_chart(float* data, int count, float min_val, float max_val);

/** Update chart data */
void widget_line_chart_set_data(SGNode* chart, float* data, int count);

/** Set colors */
void widget_line_chart_set_colors(SGNode* chart, uint32_t line, uint32_t fill, uint32_t grid);

/** Show/hide points */
void widget_line_chart_show_points(SGNode* chart, int show, float radius);

/* ========================================================================
 * Bar Chart
 * ======================================================================== */

typedef struct {
    float* values;                /* Bar values */
    char** labels;                /* Bar labels */
    int count;                    /* Number of bars */
    float max;                    /* Maximum value */
    uint32_t bar_color;           /* Bar fill color */
    uint32_t text_color;          /* Label text color */
    int vertical;                 /* 0=horizontal, 1=vertical */
    float bar_spacing;            /* Space between bars */
} BarChartState;

/**
 * Create a bar chart
 * @param values Array of bar values
 * @param labels Array of bar labels (can be NULL)
 * @param count Number of bars
 */
SGNode* widget_bar_chart(float* values, const char** labels, int count);

/** Set orientation: 0=horizontal, 1=vertical */
void widget_bar_chart_set_orientation(SGNode* chart, int vertical);

/** Update data */
void widget_bar_chart_set_data(SGNode* chart, float* values, int count);

/** Set colors */
void widget_bar_chart_set_colors(SGNode* chart, uint32_t bar, uint32_t text);

/* ========================================================================
 * Pie Chart
 * ======================================================================== */

typedef struct {
    float* values;                /* Segment values */
    char** labels;                /* Segment labels */
    uint32_t* colors;             /* Segment colors */
    int count;                    /* Number of segments */
    float total;                  /* Sum of all values */
    float exploded[16];           /* Explode distance per segment */
    int show_labels;              /* Show labels */
    int show_percentages;         /* Show percentages */
} PieChartState;

/**
 * Create a pie chart
 * @param values Array of segment values
 * @param labels Array of segment labels (can be NULL)
 * @param count Number of segments
 */
SGNode* widget_pie_chart(float* values, const char** labels, int count);

/** Explode a segment outward */
void widget_pie_chart_set_exploded(SGNode* chart, int segment, float distance);

/** Set segment colors */
void widget_pie_chart_set_colors(SGNode* chart, const uint32_t* colors, int count);

/** Show/hide labels */
void widget_pie_chart_show_labels(SGNode* chart, int show, int show_percent);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_CHARTS_H */
