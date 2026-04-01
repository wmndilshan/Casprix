#include "gui_bridge.h"
#include "support/log.h"

void cpx_gui_training_progress(float loss, int step) {
    (void)loss;
    (void)step;
    /* Hook for LLM training HUD — no-op until a display backend registers. */
}
