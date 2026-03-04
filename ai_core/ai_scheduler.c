#include "ai_core.h"

int ai_predict_priority(uint32_t pid) {
    if (pid >= AI_MAX_PROCESSES) return 0;

    ai_process_profile_t *p = &profiles[pid];

    uint64_t score = 0;

    score += p->syscall_count * 2;
    score += p->io_operations * 3;

    if (p->memory_usage > 50 * 1024 * 1024)
        score += 50;

    if (p->cpu_cycles > 1000000)
        score -= 20;

    if (score > 100) return 5;      // High priority
    if (score > 50) return 3;
    return 1;
}
