#pragma once

namespace queue {
    class Manager {
    public:
        tsl::ordered_map<int64_t, std::shared_ptr<Item>> queues;
        size_t queue_buffer_capacity{};
        int64_t packet_interval{};
        mutex_t lock{};
        mutex_t process_lock{};
        bool shutting_down{};
    };
}// namespace queue