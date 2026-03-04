#include "ai_core.h"

void ai_syscall_hook(uint32_t pid, uint32_t syscall_id) {
    ai_record_syscall(pid, syscall_id);
}
