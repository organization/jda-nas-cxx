#pragma once

namespace queue {
    struct Buffer {
        size_t index;
        size_t size;
        size_t capacity;
    };
}// namespace queue