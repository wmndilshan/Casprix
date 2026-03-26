/**
 * Learning Rate Schedulers Implementation
 */

#include "scheduler.h"
#include <stdlib.h>
#include <math.h>

LRScheduler* scheduler_step_create(float initial_lr, int step_size, float gamma) {
    LRScheduler* scheduler = (LRScheduler*)calloc(1, sizeof(LRScheduler));
    
    scheduler->type = LR_STEP;
    scheduler->initial_lr = initial_lr;
    scheduler->current_epoch = 0;
    scheduler->params.step.step_size = step_size;
    scheduler->params.step.gamma = gamma;
    
    return scheduler;
}

LRScheduler* scheduler_exponential_create(float initial_lr, float gamma) {
    LRScheduler* scheduler = (LRScheduler*)calloc(1, sizeof(LRScheduler));
    
    scheduler->type = LR_EXPONENTIAL;
    scheduler->initial_lr = initial_lr;
    scheduler->current_epoch = 0;
    scheduler->params.exponential.gamma = gamma;
    
    return scheduler;
}

LRScheduler* scheduler_cosine_create(float initial_lr, int T_max, float eta_min) {
    LRScheduler* scheduler = (LRScheduler*)calloc(1, sizeof(LRScheduler));
    
    scheduler->type = LR_COSINE_ANNEALING;
    scheduler->initial_lr = initial_lr;
    scheduler->current_epoch = 0;
    scheduler->params.cosine.T_max = T_max;
    scheduler->params.cosine.eta_min = eta_min;
    
    return scheduler;
}

LRScheduler* scheduler_plateau_create(float initial_lr, float factor,
                                     int patience, float threshold) {
    LRScheduler* scheduler = (LRScheduler*)calloc(1, sizeof(LRScheduler));
    
    scheduler->type = LR_REDUCE_ON_PLATEAU;
    scheduler->initial_lr = initial_lr;
    scheduler->current_epoch = 0;
    scheduler->params.plateau.factor = factor;
    scheduler->params.plateau.patience = patience;
    scheduler->params.plateau.threshold = threshold;
    scheduler->params.plateau.cooldown = 0;
    scheduler->params.plateau.wait = 0;
    scheduler->params.plateau.best_loss = 1e9f;
    
    return scheduler;
}

float scheduler_get_lr(LRScheduler* scheduler) {
    switch (scheduler->type) {
        case LR_STEP: {
            int num_decays = scheduler->current_epoch / scheduler->params.step.step_size;
            return scheduler->initial_lr * powf(scheduler->params.step.gamma, num_decays);
        }
        
        case LR_EXPONENTIAL: {
            return scheduler->initial_lr * powf(scheduler->params.exponential.gamma, 
                                               scheduler->current_epoch);
        }
        
        case LR_COSINE_ANNEALING: {
            float T_max = (float)scheduler->params.cosine.T_max;
            float eta_min = scheduler->params.cosine.eta_min;
            float cosine = cosf(3.14159265f * scheduler->current_epoch / T_max);
            return eta_min + (scheduler->initial_lr - eta_min) * (1.0f + cosine) / 2.0f;
        }
        
        case LR_REDUCE_ON_PLATEAU: {
            return scheduler->initial_lr;  /* Updated in step_with_metric */
        }
    }
    
    return scheduler->initial_lr;
}

void scheduler_step(LRScheduler* scheduler) {
    scheduler->current_epoch++;
}

void scheduler_step_with_metric(LRScheduler* scheduler, float metric) {
    if (scheduler->type != LR_REDUCE_ON_PLATEAU) {
        scheduler_step(scheduler);
        return;
    }
    
    auto* p = &scheduler->params.plateau;
    
    /* Check if improved */
    if (metric < p->best_loss - p->threshold) {
        p->best_loss = metric;
        p->wait = 0;
    } else {
        p->wait++;
        if (p->wait >= p->patience && p->cooldown == 0) {
            scheduler->initial_lr *= p->factor;
            p->wait = 0;
            p->cooldown = p->patience;
        }
    }
    
    if (p->cooldown > 0) {
        p->cooldown--;
    }
    
    scheduler->current_epoch++;
}

void scheduler_free(LRScheduler* scheduler) {
    free(scheduler);
}
