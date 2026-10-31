/**
 * Learning Rate Schedulers
 */

#ifndef NN_SCHEDULER_H
#define NN_SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LR_STEP,                /* Step decay */
    LR_EXPONENTIAL,         /* Exponential decay */
    LR_COSINE_ANNEALING,   /* Cosine annealing */
    LR_REDUCE_ON_PLATEAU   /* Reduce on plateau */
} LRSchedulerType;

typedef struct {
    LRSchedulerType type;
    float initial_lr;
    int current_epoch;
    
    /* Type-specific parameters */
    union {
        struct {
            int step_size;
            float gamma;
        } step;
        
        struct {
            float gamma;
        } exponential;
        
        struct {
            int T_max;
            float eta_min;
        } cosine;
        
        struct {
            float factor;
            int patience;
            float threshold;
            int cooldown;
            int wait;
            float best_loss;
        } plateau;
    } params;
} LRScheduler;

/**
 * Create step decay scheduler
 * Multiply LR by gamma every step_size epochs
 */
LRScheduler* scheduler_step_create(float initial_lr, int step_size, float gamma);

/**
 * Create exponential decay scheduler
 * LR = initial_lr * gamma^epoch
 */
LRScheduler* scheduler_exponential_create(float initial_lr, float gamma);

/**
 * Create cosine annealing scheduler
 * LR = eta_min + (initial_lr - eta_min) * (1 + cos(π * epoch / T_max)) / 2
 */
LRScheduler* scheduler_cosine_create(float initial_lr, int T_max, float eta_min);

/**
 * Create reduce on plateau scheduler
 * Reduce LR when metric has stopped improving
 */
LRScheduler* scheduler_plateau_create(float initial_lr, float factor, 
                                     int patience, float threshold);

/**
 * Get current learning rate
 */
float scheduler_get_lr(LRScheduler* scheduler);

/**
 * Step scheduler (call at end of epoch)
 */
void scheduler_step(LRScheduler* scheduler);

/**
 * Step with metric (for plateau scheduler)
 */
void scheduler_step_with_metric(LRScheduler* scheduler, float metric);

/**
 * Free scheduler
 */
void scheduler_free(LRScheduler* scheduler);

#ifdef __cplusplus
}
#endif

#endif /* NN_SCHEDULER_H */
