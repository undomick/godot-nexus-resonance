#pragma once

#include <cstdint>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace resonance {

constexpr int64_t kUnknownOsThreadId = -1;

// Kernel thread id as listed under /proc/self/task, or kUnknownOsThreadId off Linux.
inline int64_t current_os_thread_id() {
#if defined(__linux__)
    return static_cast<int64_t>(syscall(SYS_gettid));
#else
    return kUnknownOsThreadId;
#endif
}

} // namespace resonance
