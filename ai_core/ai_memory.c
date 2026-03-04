#include "ai_core.h"

size_t ai_predict_memory(uint32_t pid) {
    if (pid >= AI_MAX_PROCESSES) return 4096;

    ai_process_profile_t *p = &profiles[pid];

    size_t predicted = p->memory_usage;

    predicted += (p->syscall_count * 64);
    predicted += (p->io_operations * 128);

    if (predicted < 4096)
        predicted = 4096;

    return predicted;
}
