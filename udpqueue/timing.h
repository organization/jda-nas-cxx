#pragma once

#include <chrono>
#include <thread>

inline int64_t timing_get_nanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline void timing_sleep(int64_t nano_sec) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(nano_sec));
}