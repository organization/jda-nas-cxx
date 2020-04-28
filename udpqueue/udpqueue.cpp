#include <jni.h>
#include <jvmti.h>

#include <cstdlib>
#include <cinttypes>
#include <cstring>

#include "tsl/ordered_map.h"
#include "timing.h"

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#include <vector>
#include <mutex>

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
        struct addrinfo *address;
        socket_t explicit_socket;
    };
}

namespace queue {
    class Buffer {
    public:
        size_t index;
        size_t size;
        size_t capacity;
    };

    class Item {
    public:
        int64_t next_due_time{};
        std::vector<packet::Queued> packet_buffer;
        Buffer buffer{};
        addrinfo *address{};
        socket_t explicit_socket{};
    };

    class Manager {
    public:
        tsl::ordered_map<int64_t, Item> queues;
        size_t queue_buffer_capacity{};
        int64_t packet_interval{};
        std::mutex lock{};
        std::mutex process_lock{};
        bool shutting_down{};
    };

    static void entry_free(const queue::Item &entry) {
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
        manager->lock.lock();
        manager->shutting_down = true;
        manager->lock.unlock();

        //manager->process_lock.lock();
        //manager->process_lock.unlock();
        delete manager;
    }

    static queue::Manager *create(size_t queue_buffer_capacity, int64_t packet_interval) {
        auto manager = new queue::Manager();

        manager->queue_buffer_capacity = queue_buffer_capacity;
        manager->packet_interval = packet_interval;

        return manager;
    }

    static size_t get_remaining_capacity(queue::Manager *manager, uint64_t key) {
        manager->lock.lock();

        size_t remaining;

        if (manager->queues.count(key)) {
            auto item = manager->queues[key];
            remaining = item.buffer.capacity - item.buffer.size;
        } else {
            remaining = (jint) manager->queue_buffer_capacity;
        }

        manager->lock.lock();

        return remaining;
    }

    static addrinfo *resolve_address(const char *address, int32_t port) {
        char port_text[32];

        addrinfo hints{};

        memset(&hints, 0, sizeof(hints));

#ifndef AI_NUMERICSERV
        hints.ai_flags = AI_NUMERICHOST;
#else
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
#endif
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        snprintf(port_text, sizeof(port_text), "%" PRId32, port);

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
                queue::entry_free(item);
                return false;
            }

            manager->queues[key] = item;
        }

        queue::Item &item = manager->queues[key];

        if (item.buffer.size >= item.buffer.capacity) {
            return false;
        }

        size_t next_index = (item.buffer.index + item.buffer.size) % item.buffer.capacity;
        item.packet_buffer[next_index].data = data;
        item.packet_buffer[next_index].length = data_length;
        item.buffer.size++;
        return true;
    }

    static bool queue_packet(queue::Manager *manager, uint64_t key, const char *address, int32_t port, void *data,
                             size_t data_length, socket_t explicit_socket) {

        auto *bytes = static_cast<uint8_t *>(malloc(data_length));
        if (bytes == nullptr) {
            return false;
        }

        memcpy(bytes, data, data_length);

        manager->lock.lock();
        bool result = queue_packet_locked(manager, key, address, port, bytes, data_length, explicit_socket);
        manager->lock.unlock();

        if (!result) {
            free(bytes);
        }

        return result;
    }

    static bool queue_delete(queue::Manager *manager, uint64_t key) {
        bool found = false;
        manager->lock.lock();

        if (manager->queues.count(key)) {
            found = true;
            queue::Item &item = manager->queues[key];

            if (item.buffer.size > 0) {

                while (item.buffer.size > 0) {
                    packet::Unsent unsent_packet{};
                    queue::pop_packet(item, unsent_packet);

                    free(unsent_packet.packet.data);
                    unsent_packet.packet.data = nullptr;
                }
            }
        }

        manager->lock.unlock();
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
            manager->queues.erase(item_pair.first);
            queue::entry_free(item);
            return get_target_time(manager, current_time);
        }

        if (item.next_due_time == 0) {
            item.next_due_time = current_time;
        } else if (item.next_due_time - current_time >= 1500000LL) {
            return item.next_due_time;
        }

        queue::pop_packet(item, packet_out);

        manager->queues.erase(key);
        manager->queues.insert(std::make_pair(key, item));

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

        free(unsent_packet.packet.data);
        unsent_packet.packet.data = nullptr;
        unsent_packet.packet.length = 0;
    }

    static void process_with_socket(queue::Manager *manager, socket_t socket_v4, socket_t socket_v6) {
        manager->process_lock.lock();

        while (true) {
            manager->lock.lock();

            if (manager->shutting_down) {
                manager->lock.unlock();
                break;
            }

            int64_t current_time = timing_get_nanos();
            packet::Unsent packet_to_send = {nullptr};

            int64_t target_time = process_next_locked(manager, packet_to_send, current_time);
            manager->lock.lock();

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

        manager->process_lock.unlock();
    }

    static void process(queue::Manager *manager) {
        socket_t socket_v4 = socket(AF_INET, SOCK_DGRAM, 0);
        socket_t socket_v6 = socket(AF_INET6, SOCK_DGRAM, 0);

        process_with_socket(manager, socket_v4, socket_v6);

        closesocket(socket_v4);
        closesocket(socket_v6);
    }
}

JNIEXPORT jlong JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_create(JNIEnv *jni, jobject me,
                                                                                     jint queue_buffer_capacity,
                                                                                     jlong packet_interval) {
    return (jlong) Manager::create((size_t) queue_buffer_capacity, packet_interval);
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_destroy(JNIEnv *jni, jobject me,
                                                                                      jlong instance) {
    Manager::destroy((queue::Manager *) instance);
}

JNIEXPORT jint JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_getRemainingCapacity(JNIEnv *jni,
                                                                                                   jobject me,
                                                                                                   jlong instance,
                                                                                                   jlong key) {
    return (jint) Manager::get_remaining_capacity((queue::Manager *) instance, (uint64_t) key);
}

JNIEXPORT jboolean JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_queuePacket(JNIEnv *jni, jobject me,
                                                                                          jlong instance, jlong key,
                                                                                          jstring address_string,
                                                                                          jint port,
                                                                                          jobject data_buffer,
                                                                                          jint data_length) {

    const char *address = jni->GetStringUTFChars(address_string, nullptr);
    void *bytes = jni->GetDirectBufferAddress(data_buffer);

    bool result = Manager::queue_packet((queue::Manager *) instance, (uint64_t) key, address, port, bytes,
                                        (size_t) data_length, SOCKET_INVALID);

    jni->ReleaseStringUTFChars(address_string, address);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_queuePacketWithSocket(
        JNIEnv *jni, jobject me, jlong instance, jlong key, jstring address_string, jint port, jobject data_buffer,
        jint data_length, jlong socket_handle) {

    const char *address = jni->GetStringUTFChars(address_string, nullptr);
    void *bytes = jni->GetDirectBufferAddress(data_buffer);

    bool result = Manager::queue_packet((queue::Manager *) instance, (uint64_t) key, address, port, bytes,
                                        (size_t) data_length, (socket_t) socket_handle);

    jni->ReleaseStringUTFChars(address_string, address);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_deleteQueue(
        JNIEnv *jni, jobject me, jlong instance, jlong key) {

    bool result = Manager::queue_delete((queue::Manager *) instance, (uint64_t) key);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_process(JNIEnv *jni, jobject me,
                                                                                      jlong instance) {
    Manager::process((queue::Manager *) instance);
}

JNIEXPORT void JNICALL Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_processWithSocket(
        JNIEnv *jni, jobject me, jlong instance, jlong socket_v4, jlong socket_v6) {

    Manager::process_with_socket((queue::Manager *) instance, (socket_t) socket_v4, (socket_t) socket_v6);
}

jint JNICALL waiting_iterate_callback(jlong class_tag, jlong size, jlong *tag_ptr, jint length, void *user_data) {
    int wait_duration = *((int *) user_data);
    timing_sleep(wait_duration * 1000000LL);
    return JVMTI_VISIT_ABORT;
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_pauseDemo(JNIEnv *jni, jclass me,
                                                                                        jint length) {
    jvmtiEnv *jvmti;
    JavaVM *vm;

    if (jni->GetJavaVM(&vm) != JNI_OK) {
        return;
    }

    if (vm->GetEnv((void **) &jvmti, JVMTI_VERSION_1_2) != JNI_OK) {
        return;
    }

    jvmtiCapabilities capabilities;
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.can_tag_objects = 1;
    jvmti->AddCapabilities(&capabilities);

    jvmtiHeapCallbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.heap_iteration_callback = waiting_iterate_callback;

    jvmti->IterateThroughHeap(0, nullptr, &callbacks, &length);
    jvmti->DisposeEnvironment();
}