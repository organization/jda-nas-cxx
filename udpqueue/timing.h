#pragma once

#include <chrono>
#include <thread>

int64_t timing_get_nanos() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

    return nanoseconds.count();
}

void timing_sleep(int64_t nano_sec) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(nano_sec));
}