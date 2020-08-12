#pragma once

namespace queue {
    class Item {
    public:
        int64_t next_due_time{};
        std::unordered_map<size_t, packet::Queued> packet_buffer;
        Buffer buffer{};
        addrinfo* address{};
        socket_t explicit_socket{};
    };
}// namespace queue