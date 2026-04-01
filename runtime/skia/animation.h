/*
 * Animation System — Property animations with easing functions
 *
 * Animates SGNode properties (position, size, opacity, rotation, color)
 * over time with configurable easing curves and looping modes.
 */

#ifndef ANIMATION_H
#define ANIMATION_H

#include "scene_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Easing Functions
 * ======================================================================== */

typedef enum {
    SG_EASE_LINEAR,
    SG_EASE_IN_QUAD,
    SG_EASE_OUT_QUAD,
    SG_EASE_IN_OUT_QUAD,
    SG_EASE_IN_CUBIC,
    SG_EASE_OUT_CUBIC,
    SG_EASE_IN_OUT_CUBIC,
    SG_EASE_IN_EXPO,
    SG_EASE_OUT_EXPO,
    SG_EASE_IN_OUT_EXPO,
    SG_EASE_IN_BACK,
    SG_EASE_OUT_BACK,
    SG_EASE_IN_OUT_BACK,
    SG_EASE_OUT_BOUNCE,
    SG_EASE_IN_ELASTIC,
    SG_EASE_OUT_ELASTIC,
} SGEasing;

/* Evaluate easing function. t in [0,1], returns [0,1] */
float sg_ease(SGEasing easing, float t);

/* ========================================================================
 * Animatable Properties
 * ======================================================================== */

typedef enum {
    SG_ANIM_X,
    SG_ANIM_Y,
    SG_ANIM_W,
    SG_ANIM_H,
    SG_ANIM_OPACITY,
    SG_ANIM_SCALE_X,
    SG_ANIM_SCALE_Y,
    SG_ANIM_ROTATION,
    SG_ANIM_TRANSLATE_X,
    SG_ANIM_TRANSLATE_Y,
    SG_ANIM_BORDER_RADIUS,
    SG_ANIM_PADDING,         /* Uniform padding */
    SG_ANIM_MIN_WIDTH,
} SGAnimProperty;

/* ========================================================================
 * Loop Modes
 * ======================================================================== */

typedef enum {
    SG_LOOP_NONE,         /* Play once and stop */
    SG_LOOP_REPEAT,       /* Restart from beginning */
    SG_LOOP_PING_PONG,    /* Reverse direction on each cycle */
} SGLoopMode;

/* ========================================================================
 * Animation
 * ======================================================================== */

typedef void (*SGAnimCallback)(void* user_data);

typedef struct SGAnimation {
    SGNode* target;            /* Node being animated */
    SGAnimProperty property;   /* Which property to animate */
    float from;                /* Start value */
    float to;                  /* End value */
    float duration;            /* Duration in seconds */
    float elapsed;             /* Time elapsed */
    float delay;               /* Start delay in seconds */
    SGEasing easing;           /* Easing function */
    SGLoopMode loop;           /* Looping mode */
    int active;                /* Is this animation running? */
    int reverse;               /* Currently playing in reverse? (ping-pong) */

    /* Callbacks */
    SGAnimCallback on_complete;
    SGAnimCallback on_update;
    void* user_data;

    /* Linked list for animation pool */
    struct SGAnimation* next;
} SGAnimation;

/* ========================================================================
 * Animation Pool
 * ======================================================================== */

#define SG_MAX_ANIMATIONS 256

typedef struct {
    SGAnimation pool[SG_MAX_ANIMATIONS];
    SGAnimation* active_head;     /* Active animations linked list */
    SGAnimation* free_head;       /* Free slots linked list */
    int active_count;
} SGAnimationPool;

SGAnimationPool* sg_animation_pool_create(void);
void             sg_animation_pool_destroy(SGAnimationPool* pool);

/* ========================================================================
 * Animation API
 * ======================================================================== */

/* Start a new animation. Returns animation handle. */
SGAnimation* sg_animate(SGAnimationPool* pool, SGNode* target,
                         SGAnimProperty property, double from, double to,
                         double duration, SGEasing easing);

/* Set animation options (builder pattern) */
SGAnimation* sg_animate_delay(SGAnimation* anim, float delay);
SGAnimation* sg_animate_loop(SGAnimation* anim, SGLoopMode mode);
SGAnimation* sg_animate_on_complete(SGAnimation* anim, SGAnimCallback cb, void* data);

/* Cancel an animation */
void sg_animate_cancel(SGAnimationPool* pool, SGAnimation* anim);

/* Cancel all animations targeting a specific node */
void sg_animate_cancel_all(SGAnimationPool* pool, SGNode* target);

/* Tick all active animations by dt seconds.
 * Returns number of active animations remaining. */
int sg_animation_tick(SGAnimationPool* pool, float dt);

/* ========================================================================
 * Convenience Animations
 * ======================================================================== */

/* Fade in (opacity 0 → 1) */
SGAnimation* sg_fade_in(SGAnimationPool* pool, SGNode* target, double duration);

/* Fade out (opacity 1 → 0) */
SGAnimation* sg_fade_out(SGAnimationPool* pool, SGNode* target, double duration);

/* Slide from offset to current position */
SGAnimation* sg_slide_in(SGAnimationPool* pool, SGNode* target,
                          float from_x, float from_y, float duration);

/* Scale animation */
SGAnimation* sg_scale_to(SGAnimationPool* pool, SGNode* target,
                          double from_scale, double to_scale, double duration);

#ifdef __cplusplus
}
#endif

#endif /* ANIMATION_H */
