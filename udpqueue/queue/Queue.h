#pragma once

#include "Buffer.h"
#include "Item.h"
#include "Manager.h"

namespace queue {

    static void addrinfo_free(const std::shared_ptr<queue::Item>& entry) {
        if (entry->address != nullptr) {
            freeaddrinfo(entry->address);
        }
    }

    static void pop(const std::shared_ptr<queue::Item>& item, packet::Unsent& packet_out) {
        packet::Queued& packet = item->packet_buffer[item->buffer.index];

        item->buffer.index = (item->buffer.index + 1) % item->buffer.capacity;
        item->buffer.size--;

        packet_out.packet = std::move(packet);
        packet_out.address = item->address;
        packet_out.explicit_socket = item->explicit_socket;

        item->packet_buffer.erase(item->buffer.index);
    }

    static addrinfo* resolve_address(const char* address, int32_t port) {
        char port_text[32];

        addrinfo hints{};

        std::memset(&hints, 0, sizeof(hints));

#ifndef AI_NUMERICSERV
        hints.ai_flags = AI_NUMERICHOST;
#else
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
#endif
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        std::snprintf(port_text, sizeof(port_text), "%" PRId32, port);

        addrinfo* result = nullptr;
        getaddrinfo(address, port_text, &hints, &result);

        return result;
    }

    static bool push_locked(queue::Manager* manager, uint64_t key, const char* address, int32_t port,
                            lni::vector<uint8_t>&& data, socket_t explicit_socket) {

        if (manager == nullptr) {
            return false;
        } else if (!manager->queues.count(key)) {
            auto item = std::make_unique<queue::Item>();

            item->next_due_time = 0;
            item->address = resolve_address(address, port);
            item->buffer = {
                    0,
                    0,
                    manager->queue_buffer_capacity};
            item->explicit_socket = explicit_socket;
            item->packet_buffer.reserve(item->buffer.capacity);

            manager->queues.insert({key, std::move(item)});
        }

        auto& item = manager->queues[key];

        if (item->buffer.size >= item->buffer.capacity) {
            return false;
        }

        size_t next_index = (item->buffer.index + item->buffer.size) % item->buffer.capacity;

        auto data_length = data.size();

        item->packet_buffer[next_index] = {
                std::move(data),
                data_length};
        item->buffer.size++;

        return true;
    }

    static bool push(queue::Manager* manager, uint64_t key, const char* address, int32_t port, void* data,
                     size_t data_length, socket_t explicit_socket) {

        lni::vector<uint8_t> bytes(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + data_length);

        mutex_lock(manager->lock);
        bool result = queue::push_locked(manager, key, address, port, std::move(bytes), explicit_socket);
        mutex_unlock(manager->lock);

        /*if (!result) {
            bytes.reset();
        }*/

        return result;
    }

    static bool remove(queue::Manager* manager, uint64_t key) {
        bool found = false;
        mutex_lock(manager->lock);

        if (manager->queues.count(key)) {
            found = true;
            auto& item = manager->queues[key];

            if (item->buffer.size > 0) {
                while (item->buffer.size > 0) {
                    packet::Unsent unsent_packet;
                    queue::pop(item, unsent_packet);
                }
            }
        }

        mutex_unlock(manager->lock);
        return found;
    }
}// namespace queue