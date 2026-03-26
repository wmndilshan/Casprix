/*
 * Animation System — Easing functions and property animation tick
 */

#include "animation.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========================================================================
 * Easing Functions
 * ======================================================================== */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float sg_ease(SGEasing easing, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;

    switch (easing) {
        case SG_EASE_LINEAR:
            return t;

        case SG_EASE_IN_QUAD:
            return t * t;

        case SG_EASE_OUT_QUAD:
            return t * (2.0f - t);

        case SG_EASE_IN_OUT_QUAD:
            return (t < 0.5f) ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;

        case SG_EASE_IN_CUBIC:
            return t * t * t;

        case SG_EASE_OUT_CUBIC: {
            float u = t - 1.0f;
            return u * u * u + 1.0f;
        }

        case SG_EASE_IN_OUT_CUBIC:
            return (t < 0.5f)
                ? 4.0f * t * t * t
                : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;

        case SG_EASE_IN_EXPO:
            return powf(2.0f, 10.0f * (t - 1.0f));

        case SG_EASE_OUT_EXPO:
            return 1.0f - powf(2.0f, -10.0f * t);

        case SG_EASE_IN_OUT_EXPO:
            if (t < 0.5f)
                return powf(2.0f, 20.0f * t - 10.0f) / 2.0f;
            return (2.0f - powf(2.0f, -20.0f * t + 10.0f)) / 2.0f;

        case SG_EASE_IN_BACK: {
            float s = 1.70158f;
            return t * t * ((s + 1.0f) * t - s);
        }

        case SG_EASE_OUT_BACK: {
            float s = 1.70158f;
            float u = t - 1.0f;
            return u * u * ((s + 1.0f) * u + s) + 1.0f;
        }

        case SG_EASE_IN_OUT_BACK: {
            float s = 1.70158f * 1.525f;
            if (t < 0.5f) {
                float u = 2.0f * t;
                return (u * u * ((s + 1.0f) * u - s)) / 2.0f;
            } else {
                float u = 2.0f * t - 2.0f;
                return (u * u * ((s + 1.0f) * u + s) + 2.0f) / 2.0f;
            }
        }

        case SG_EASE_OUT_BOUNCE: {
            if (t < 1.0f / 2.75f) {
                return 7.5625f * t * t;
            } else if (t < 2.0f / 2.75f) {
                float u = t - 1.5f / 2.75f;
                return 7.5625f * u * u + 0.75f;
            } else if (t < 2.5f / 2.75f) {
                float u = t - 2.25f / 2.75f;
                return 7.5625f * u * u + 0.9375f;
            } else {
                float u = t - 2.625f / 2.75f;
                return 7.5625f * u * u + 0.984375f;
            }
        }

        case SG_EASE_IN_ELASTIC:
            return -powf(2.0f, 10.0f * t - 10.0f) *
                    sinf((t * 10.0f - 10.75f) * (2.0f * (float)M_PI) / 3.0f);

        case SG_EASE_OUT_ELASTIC:
            return powf(2.0f, -10.0f * t) *
                    sinf((t * 10.0f - 0.75f) * (2.0f * (float)M_PI) / 3.0f) + 1.0f;

        default:
            return t;
    }
}

/* ========================================================================
 * Property Application
 * ======================================================================== */

static void apply_animated_value(SGNode* target, SGAnimProperty prop, float value) {
    if (!target) return;

    switch (prop) {
        case SG_ANIM_X:           target->transform.tx = value; break;
        case SG_ANIM_Y:           target->transform.ty = value; break;
        case SG_ANIM_W:           target->bounds.w = value; break;
        case SG_ANIM_H:           target->bounds.h = value; break;
        case SG_ANIM_OPACITY:     target->style.opacity = value; break;
        case SG_ANIM_SCALE_X:     target->transform.sx = value; break;
        case SG_ANIM_SCALE_Y:     target->transform.sy = value; break;
        case SG_ANIM_ROTATION:    target->transform.rotation = value; break;
        case SG_ANIM_TRANSLATE_X: target->transform.tx = value; break;
        case SG_ANIM_TRANSLATE_Y: target->transform.ty = value; break;
        case SG_ANIM_BORDER_RADIUS: target->style.border_radius = value; break;
        case SG_ANIM_PADDING:
            target->style.padding[0] = value;
            target->style.padding[1] = value;
            target->style.padding[2] = value;
            target->style.padding[3] = value;
            break;
    }

    sg_node_mark_dirty(target, SG_DIRTY_PAINT);
}

/* ========================================================================
 * Animation Pool
 * ======================================================================== */

SGAnimationPool* sg_animation_pool_create(void) {
    SGAnimationPool* pool = (SGAnimationPool*)calloc(1, sizeof(SGAnimationPool));
    if (!pool) return NULL;

    /* Build free list */
    pool->free_head = &pool->pool[0];
    for (int i = 0; i < SG_MAX_ANIMATIONS - 1; i++) {
        pool->pool[i].next = &pool->pool[i + 1];
    }
    pool->pool[SG_MAX_ANIMATIONS - 1].next = NULL;
    pool->active_head = NULL;
    pool->active_count = 0;

    return pool;
}

void sg_animation_pool_destroy(SGAnimationPool* pool) {
    free(pool);
}

/* ========================================================================
 * Animation API
 * ======================================================================== */

SGAnimation* sg_animate(SGAnimationPool* pool, SGNode* target,
                         SGAnimProperty property, double from, double to,
                         double duration, SGEasing easing) {
    if (!pool || !target || !pool->free_head) return NULL;

    /* Take from free list */
    SGAnimation* anim = pool->free_head;
    pool->free_head = anim->next;

    /* Initialize */
    memset(anim, 0, sizeof(SGAnimation));
    anim->target = target;
    anim->property = property;
    anim->from = from;
    anim->to = to;
    anim->duration = duration;
    anim->easing = easing;
    anim->active = 1;
    anim->loop = SG_LOOP_NONE;

    /* Add to active list */
    anim->next = pool->active_head;
    pool->active_head = anim;
    pool->active_count++;

    /* Apply initial value */
    apply_animated_value(target, property, from);

    return anim;
}

SGAnimation* sg_animate_delay(SGAnimation* anim, float delay) {
    if (anim) anim->delay = delay;
    return anim;
}

SGAnimation* sg_animate_loop(SGAnimation* anim, SGLoopMode mode) {
    if (anim) anim->loop = mode;
    return anim;
}

SGAnimation* sg_animate_on_complete(SGAnimation* anim, SGAnimCallback cb, void* data) {
    if (anim) {
        anim->on_complete = cb;
        anim->user_data = data;
    }
    return anim;
}

void sg_animate_cancel(SGAnimationPool* pool, SGAnimation* anim) {
    if (!pool || !anim) return;
    anim->active = 0;
}

void sg_animate_cancel_all(SGAnimationPool* pool, SGNode* target) {
    if (!pool || !target) return;
    SGAnimation* a = pool->active_head;
    while (a) {
        if (a->target == target) a->active = 0;
        a = a->next;
    }
}

/* ========================================================================
 * Animation Tick
 * ======================================================================== */

int sg_animation_tick(SGAnimationPool* pool, float dt) {
    if (!pool) return 0;

    SGAnimation* prev = NULL;
    SGAnimation* anim = pool->active_head;
    int active = 0;

    while (anim) {
        SGAnimation* next_anim = anim->next;

        if (!anim->active) {
            /* Remove from active list, return to free list */
            if (prev) {
                prev->next = next_anim;
            } else {
                pool->active_head = next_anim;
            }
            anim->next = pool->free_head;
            pool->free_head = anim;
            pool->active_count--;
            anim = next_anim;
            continue;
        }

        /* Handle delay */
        if (anim->delay > 0) {
            anim->delay -= dt;
            if (anim->delay > 0) {
                prev = anim;
                anim = next_anim;
                active++;
                continue;
            }
            /* Delay just expired — absorb overshoot */
            dt = -anim->delay;
            anim->delay = 0;
        }

        anim->elapsed += dt;

        float t = anim->elapsed / anim->duration;
        if (t > 1.0f) t = 1.0f;

        /* Apply easing */
        float eased;
        if (anim->reverse) {
            eased = sg_ease(anim->easing, 1.0f - t);
        } else {
            eased = sg_ease(anim->easing, t);
        }

        /* Interpolate value */
        float value = anim->from + (anim->to - anim->from) * eased;
        apply_animated_value(anim->target, anim->property, value);

        /* Notify update */
        if (anim->on_update) {
            anim->on_update(anim->user_data);
        }

        /* Check completion */
        if (anim->elapsed >= anim->duration) {
            switch (anim->loop) {
                case SG_LOOP_NONE:
                    /* Apply final value exactly */
                    apply_animated_value(anim->target, anim->property,
                                          anim->reverse ? anim->from : anim->to);
                    if (anim->on_complete) {
                        anim->on_complete(anim->user_data);
                    }
                    anim->active = 0;
                    break;

                case SG_LOOP_REPEAT:
                    anim->elapsed -= anim->duration;
                    break;

                case SG_LOOP_PING_PONG:
                    anim->elapsed -= anim->duration;
                    anim->reverse = !anim->reverse;
                    break;
            }
        }

        active++;
        prev = anim;
        anim = next_anim;
    }

    return active;
}

/* ========================================================================
 * Convenience Animations
 * ======================================================================== */

SGAnimation* sg_fade_in(SGAnimationPool* pool, SGNode* target, double duration) {
    return sg_animate(pool, target, SG_ANIM_OPACITY, 0.0f, 1.0f,
                       duration, SG_EASE_OUT_QUAD);
}

SGAnimation* sg_fade_out(SGAnimationPool* pool, SGNode* target, double duration) {
    return sg_animate(pool, target, SG_ANIM_OPACITY, 1.0f, 0.0f,
                       duration, SG_EASE_IN_QUAD);
}

SGAnimation* sg_slide_in(SGAnimationPool* pool, SGNode* target,
                          float from_x, float from_y, float duration) {
    float to_x = target->transform.tx;
    float to_y = target->transform.ty;
    sg_animate(pool, target, SG_ANIM_TRANSLATE_X, from_x, to_x,
                duration, SG_EASE_OUT_CUBIC);
    return sg_animate(pool, target, SG_ANIM_TRANSLATE_Y, from_y, to_y,
                       duration, SG_EASE_OUT_CUBIC);
}

SGAnimation* sg_scale_to(SGAnimationPool* pool, SGNode* target,
                          double from_scale, double to_scale, double duration) {
    sg_animate(pool, target, SG_ANIM_SCALE_X, from_scale, to_scale,
                duration, SG_EASE_OUT_CUBIC);
    return sg_animate(pool, target, SG_ANIM_SCALE_Y, from_scale, to_scale,
                       duration, SG_EASE_OUT_CUBIC);
}
