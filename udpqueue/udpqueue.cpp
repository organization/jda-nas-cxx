#include <jni.h>
#include <jvmti.h>

#include <cinttypes>
#include <cstring>

#include "lni/vector.hpp"
#include "timing.h"
#include "tsl/ordered_map.h"

extern "C" {
#include "mutex.h"
}

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#define SOCKET_INVALID INVALID_SOCKET
typedef SOCKET socket_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#define SOCKET_INVALID -1
typedef int socket_t;
#define closesocket close
#endif

namespace packet {
    class Queued {
    public:
        uint8_t *data;
        size_t length;
    };

    class Unsent {
    public:
        Queued packet;
        addrinfo* address;
        socket_t explicit_socket;
    };
}

namespace queue {
    struct Buffer {
        size_t index;
        size_t size;
        size_t capacity;
    };

    class Item {
    public:
        int64_t next_due_time{};
        lni::vector<packet::Queued> packet_buffer;
        Buffer buffer{};
        addrinfo *address{};
        socket_t explicit_socket{};
    };

    class Manager {
    public:
        tsl::ordered_map<int64_t, Item> queues;
        size_t queue_buffer_capacity{};
        int64_t packet_interval{};
        mutex_t lock{};
        mutex_t process_lock{};
        bool shutting_down{};
    };

    static void addrinfo_free(const queue::Item& entry) {
        if (entry.address != nullptr) {
            freeaddrinfo(entry.address);
        }
    }

    static void pop_packet(queue::Item &item, packet::Unsent &packet_out) {
        packet::Queued packet = item.packet_buffer[item.buffer.index];

        item.buffer.index = (item.buffer.index + 1) % item.buffer.capacity;
        item.buffer.size--;

        packet_out.packet = packet;
        packet_out.address = item.address;
        packet_out.explicit_socket = item.explicit_socket;

        packet.data = nullptr;
        packet.length = 0;
    }
}

namespace Manager {

    static void destroy(queue::Manager *manager) {
        mutex_lock(manager->lock);
        manager->shutting_down = true;
        mutex_unlock(manager->lock);

        mutex_lock(manager->process_lock);
        mutex_unlock(manager->process_lock);

        mutex_destroy(manager->lock);
        mutex_destroy(manager->process_lock);

        delete manager;
    }

    static queue::Manager *create(size_t queue_buffer_capacity, int64_t packet_interval) {
        auto manager = new queue::Manager;

        manager->lock = mutex_create();
        manager->process_lock = mutex_create();

        manager->queue_buffer_capacity = queue_buffer_capacity;
        manager->packet_interval = packet_interval;

        return manager;
    }

    static size_t get_remaining_capacity(queue::Manager *manager, uint64_t key) {
        mutex_lock(manager->lock);

        size_t remaining;

        if (manager->queues.count(key)) {
            auto item = manager->queues[key];
            remaining = item.buffer.capacity - item.buffer.size;
        } else {
            remaining = (jint) manager->queue_buffer_capacity;
        }

        mutex_unlock(manager->lock);

        return remaining;
    }

    static addrinfo *resolve_address(const char *address, int32_t port) {
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

        addrinfo *result = nullptr;
        getaddrinfo(address, port_text, &hints, &result);

        return result;
    }

    static bool queue_packet_locked(queue::Manager *manager, uint64_t key, const char *address, int32_t port,
                                    uint8_t *data, size_t data_length, socket_t explicit_socket) {

        if (!manager->queues.count(key)) {
            queue::Item item;

            queue::Buffer buffer{};
            buffer.capacity = manager->queue_buffer_capacity;

            item.next_due_time = 0;
            item.address = resolve_address(address, port);
            item.buffer = buffer;
            item.explicit_socket = explicit_socket;

            if (item.address == nullptr) {
                queue::addrinfo_free(item);
                manager->queues.erase(key);
                return false;
            }

            manager->queues.insert({key, item});
        }

        queue::Item& item = manager->queues[key];

        if (item.buffer.size >= item.buffer.capacity) {
            return false;
        }

        size_t next_index = (item.buffer.index + item.buffer.size) % item.buffer.capacity;

        packet::Queued queued_packet{
                data,
                data_length};

        item.packet_buffer.emplace(item.packet_buffer.begin() + next_index, queued_packet);
        item.buffer.size++;
        return true;
    }

    static bool queue_packet(queue::Manager *manager, uint64_t key, const char *address, int32_t port, void *data,
                             size_t data_length, socket_t explicit_socket) {

        uint8_t* bytes;

        try {
            bytes = new uint8_t[data_length];
        } catch (const std::bad_alloc& e) {
            return false;
        }

        std::memcpy(bytes, data, data_length);

        mutex_lock(manager->lock);
        bool result = queue_packet_locked(manager, key, address, port, bytes, data_length, explicit_socket);
        mutex_unlock(manager->lock);

        if (!result) {
            delete[] bytes;
        }

        return result;
    }

    static bool queue_delete(queue::Manager *manager, uint64_t key) {
        bool found = false;
        mutex_lock(manager->lock);

        if (manager->queues.count(key)) {
            found = true;
            queue::Item &item = manager->queues[key];

            if (item.buffer.size > 0) {
                while (item.buffer.size > 0) {
                    packet::Unsent unsent_packet{};
                    queue::pop_packet(item, unsent_packet);

                    delete[] unsent_packet.packet.data;
                    unsent_packet.packet.data = nullptr;
                }
            }
        }

        mutex_unlock(manager->lock);
        return found;
    }

    static int64_t get_target_time(queue::Manager *manager, int64_t current_time) {
        return manager->queues.empty() ? current_time + manager->packet_interval
                                       : manager->queues.front().second.next_due_time;
    }

    static int64_t process_next_locked(queue::Manager *manager, packet::Unsent &packet_out, int64_t current_time) {
        if (manager->queues.empty()) {
            return current_time + manager->packet_interval;
        }

        const auto &item_pair = manager->queues.front();
        auto item = item_pair.second;
        auto key = item_pair.first;

        packet_out.packet.data = nullptr;
        packet_out.address = nullptr;
        packet_out.explicit_socket = SOCKET_INVALID;

        if (item.buffer.size == 0) {
            manager->queues.erase(key);
            queue::addrinfo_free(item);
            return get_target_time(manager, current_time);
        }

        if (item.next_due_time == 0) {
            item.next_due_time = current_time;
        } else if (item.next_due_time - current_time >= 1500000LL) {
            return item.next_due_time;
        }

        queue::pop_packet(item, packet_out);

        //manager->queues.erase(key);
        //manager->queues.insert({key, item});

        current_time = timing_get_nanos();

        if (current_time - item.next_due_time >= 2 * manager->packet_interval) {
            item.next_due_time = current_time + manager->packet_interval;
        } else {
            item.next_due_time += manager->packet_interval;
        }

        return get_target_time(manager, current_time);
    }

    static void dispatch_packet(socket_t socket_vx, packet::Unsent &unsent_packet) {
        sendto(socket_vx, (const char *) unsent_packet.packet.data, (int) unsent_packet.packet.length, 0,
               unsent_packet.address->ai_addr, sizeof(*unsent_packet.address->ai_addr));

        delete[] unsent_packet.packet.data;
        unsent_packet.packet.data = nullptr;
        unsent_packet.packet.length = 0;
    }

    static void process_with_socket(queue::Manager *manager, socket_t socket_v4, socket_t socket_v6) {
        mutex_lock(manager->process_lock);

        while (true) {
            mutex_lock(manager->lock);

            if (manager->shutting_down) {
                mutex_unlock(manager->lock);
                break;
            }

            int64_t current_time = timing_get_nanos();
            packet::Unsent packet_to_send = {nullptr};

            int64_t target_time = process_next_locked(manager, packet_to_send, current_time);
            mutex_unlock(manager->lock);

            if (packet_to_send.packet.data != nullptr) {
                if (packet_to_send.explicit_socket == SOCKET_INVALID) {
                    socket_t socket_vx = packet_to_send.address->ai_family == AF_INET ? socket_v4 : socket_v6;
                    dispatch_packet(socket_vx, packet_to_send);
                } else {
                    dispatch_packet(packet_to_send.explicit_socket, packet_to_send);
                }

                current_time = timing_get_nanos();
            }

            int64_t wait_time = target_time - current_time;

            if (wait_time >= 1500000LL) {
                timing_sleep(wait_time);
            }
        }

        mutex_unlock(manager->process_lock);
    }

    static void process(queue::Manager *manager) {
        socket_t socket_v4 = socket(AF_INET, SOCK_DGRAM, 0);
        socket_t socket_v6 = socket(AF_INET6, SOCK_DGRAM, 0);

        process_with_socket(manager, socket_v4, socket_v6);

        closesocket(socket_v4);
        closesocket(socket_v6);
    }
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_create(JNIEnv* jni, jobject me,
                                                                                     jint queue_buffer_capacity,
                                                                                     jlong packet_interval) {
    return reinterpret_cast<jlong>(Manager::create(queue_buffer_capacity, packet_interval));
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_destroy(JNIEnv* jni, jobject me,
                                                                                      jlong instance) {
    Manager::destroy(reinterpret_cast<queue::Manager*>(instance));
}

JNIEXPORT jint JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_getRemainingCapacity(JNIEnv* jni,
                                                                                                   jobject me,
                                                                                                   jlong instance,
                                                                                                   jlong key) {
    return Manager::get_remaining_capacity(reinterpret_cast<queue::Manager*>(instance), key);
}

JNIEXPORT jboolean JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_queuePacket(JNIEnv* jni, jobject me,
                                                                                          jlong instance, jlong key,
                                                                                          jstring address_string,
                                                                                          jint port,
                                                                                          jobject data_buffer,
                                                                                          jint data_length) {

    const char* address = jni->GetStringUTFChars(address_string, nullptr);
    void* bytes = jni->GetDirectBufferAddress(data_buffer);

    bool result = Manager::queue_packet(reinterpret_cast<queue::Manager*>(instance), key, address, port, bytes,
                                        data_length, SOCKET_INVALID);

    jni->ReleaseStringUTFChars(address_string, address);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_queuePacketWithSocket(
        JNIEnv* jni, jobject me, jlong instance, jlong key, jstring address_string, jint port, jobject data_buffer,
        jint data_length, jlong socket_handle) {

    const char* address = jni->GetStringUTFChars(address_string, nullptr);
    void* bytes = jni->GetDirectBufferAddress(data_buffer);

    bool result = Manager::queue_packet(reinterpret_cast<queue::Manager*>(instance), key, address, port, bytes,
                                        data_length, socket_handle);

    jni->ReleaseStringUTFChars(address_string, address);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_deleteQueue(
        JNIEnv* jni, jobject me, jlong instance, jlong key) {

    bool result = Manager::queue_delete(reinterpret_cast<queue::Manager*>(instance), key);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_process(JNIEnv* jni, jobject me,
                                                                                      jlong instance) {
    Manager::process((queue::Manager*) instance);
}

JNIEXPORT void JNICALL Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_processWithSocket(
        JNIEnv* jni, jobject me, jlong instance, jlong socket_v4, jlong socket_v6) {

    Manager::process_with_socket((queue::Manager*) instance, (socket_t) socket_v4, (socket_t) socket_v6);
}

jint JNICALL waiting_iterate_callback(jlong class_tag, jlong size, jlong* tag_ptr, jint length, void* user_data) {
    int wait_duration = *(reinterpret_cast<int*>(user_data));
    timing_sleep(wait_duration * 1000000LL);
    return JVMTI_VISIT_ABORT;
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_pauseDemo(JNIEnv* jni, jclass me,
                                                                                        jint length) {
    jvmtiEnv* jvmti;
    JavaVM* vm;

    if (jni->GetJavaVM(&vm) != JNI_OK) {
        return;
    }

    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        return;
    }

    jvmtiCapabilities capabilities;
    std::memset(&capabilities, 0, sizeof(capabilities));
    capabilities.can_tag_objects = 1;
    jvmti->AddCapabilities(&capabilities);

    jvmtiHeapCallbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.heap_iteration_callback = waiting_iterate_callback;

    jvmti->IterateThroughHeap(0, nullptr, &callbacks, &length);
    jvmti->DisposeEnvironment();
}
}
