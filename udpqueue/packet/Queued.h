#pragma once

namespace packet {
    class Queued {
    public:
        lni::vector<uint8_t> data;
        size_t length{};
    };
}// namespace packet