/*
 * Frame Loop — Application container with 60 FPS render loop
 *
 * SGApp ties together: window, scene graph, layout, events, animations,
 * font management, and text caching into a single run loop.
 *
 * Frame lifecycle:
 *   1. Process OS events (PeekMessage)
 *   2. Tick animations
 *   3. Re-layout dirty subtrees
 *   4. Re-render to Skia canvas
 *   5. Present (blit to window)
 *   6. Sleep to target FPS
 */

#ifndef FRAME_LOOP_H
#define FRAME_LOOP_H

#include "scene_graph.h"
#include "events.h"
#include "animation.h"
#include "text.h"

/* Forward declare — defined in skia_window_win32.c */
typedef struct SkiaWindow SkiaWindow;

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Application State
 * ======================================================================== */

typedef struct {
    /* Core components */
    SkiaWindow* window;
    SGNode* root;
    SGEventManager* events;
    SGAnimationPool* animations;
    FontManager* fonts;
    TextCache* text_cache;

    /* Timing */
    double time;              /* Current time in seconds */
    double dt;                /* Delta time since last frame */
    double frame_start;       /* Start of current frame */
    int target_fps;           /* Target frame rate (default 60) */
    int frame_count;          /* Total frames rendered */

    /* State */
    int running;              /* Frame loop is running */
    int needs_layout;         /* Force layout next frame */
    int needs_repaint;        /* Force repaint next frame */

    /* Window dimensions */
    int width, height;

    /* User callbacks */
    void (*on_setup)(void* app);         /* Called once before first frame */
    void (*on_frame)(void* app, float dt); /* Called each frame */
    void (*on_cleanup)(void* app);       /* Called after loop exits */
    void* user_data;
} SGApp;

/* ========================================================================
 * Application Lifecycle
 * ======================================================================== */

/* Create application with window */
SGApp* sg_app_create(const char* title, int width, int height);

/* Destroy application and all owned resources */
void sg_app_destroy(SGApp* app);

/* ========================================================================
 * Configuration
 * ======================================================================== */

/* Set the root of the scene graph */
void sg_app_set_root(SGApp* app, SGNode* root);

/* Set target FPS (default 60) */
void sg_app_set_fps(SGApp* app, int fps);

/* Set frame callback */
void sg_app_on_frame(SGApp* app, void (*callback)(void* app, float dt));

/* Set setup callback (called once before first frame) */
void sg_app_on_setup(SGApp* app, void (*callback)(void* app));

/* Set cleanup callback (called after loop exits) */
void sg_app_on_cleanup(SGApp* app, void (*callback)(void* app));

/* Set user data pointer */
void sg_app_set_user_data(SGApp* app, void* data);

/* ========================================================================
 * Running
 * ======================================================================== */

/* Run the frame loop (blocking). Returns when window is closed. */
void sg_app_run(SGApp* app);

/* Request the frame loop to stop */
void sg_app_quit(SGApp* app);

/* Request a repaint on the next frame */
void sg_app_invalidate(SGApp* app);

/* Request a full layout recalculation */
void sg_app_relayout(SGApp* app);

/* ========================================================================
 * Accessors
 * ======================================================================== */

/* Get the Skia canvas for the current frame */
SkiaCanvas sg_app_get_canvas(SGApp* app);

/* Get components */
FontManager*     sg_app_get_fonts(SGApp* app);
TextCache*       sg_app_get_text_cache(SGApp* app);
SGAnimationPool* sg_app_get_animations(SGApp* app);
SGEventManager*  sg_app_get_events(SGApp* app);

/* Get timing info */
double sg_app_get_time(SGApp* app);
float  sg_app_get_dt(SGApp* app);
int    sg_app_get_fps_actual(SGApp* app); /* Measured FPS */

#ifdef __cplusplus
}
#endif

#endif /* FRAME_LOOP_H */
