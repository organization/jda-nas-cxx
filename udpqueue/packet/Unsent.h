#pragma once

namespace packet {
    class Unsent {
    public:
        Queued packet;
        addrinfo* address{};
        socket_t explicit_socket{};
    };
}// namespace packet