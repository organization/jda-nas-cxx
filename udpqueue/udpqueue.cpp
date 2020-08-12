#include <cinttypes>
#include <cstring>

// Vector
#include "lni/vector.hpp"

// Map
#include "tsl/ordered_map.h"
#include <unordered_map>

#include "timing.h"
extern "C" {
#include "mutex.h"
}

// JNI
#include <jni.h>
#include <jvmti.h>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define SOCKET_INVALID INVALID_SOCKET
typedef SOCKET socket_t;

#else

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SOCKET_INVALID -1
typedef int socket_t;
#define closesocket close

#endif

#include "packet/Queued.h"
#include "packet/Unsent.h"

#include "queue/Queue.h"

namespace manager {

    static void destroy(queue::Manager* manager) {
        if (manager->lock != nullptr && manager->process_lock != nullptr) {
            mutex_lock(manager->lock);
            manager->shutting_down = true;
            mutex_unlock(manager->lock);

            mutex_lock(manager->process_lock);
            mutex_unlock(manager->process_lock);
        }

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

        manager->queues.reserve(100);

        return manager;
    }

    static size_t get_remaining_capacity(queue::Manager *manager, uint64_t key) {
        mutex_lock(manager->lock);

        size_t remaining;

        if (manager->queues.count(key)) {
            auto& item = manager->queues[key];
            remaining = item->buffer.capacity - item->buffer.size;
        } else {
            remaining = manager->queue_buffer_capacity;
        }

        mutex_unlock(manager->lock);

        return remaining;
    }

    static int64_t get_target_time(queue::Manager *manager, int64_t current_time) {
        return manager->queues.empty() ? current_time + manager->packet_interval
                                       : manager->queues.front().second->next_due_time;
    }

    static int64_t process_next_locked(queue::Manager *manager, packet::Unsent &packet_out, int64_t current_time) {
        packet_out.packet.data.clear();
        packet_out.address = nullptr;
        packet_out.explicit_socket = SOCKET_INVALID;

        if (manager->queues.empty()) {
            return current_time + manager->packet_interval;
        }

        auto& item_pair = manager->queues.front();
        auto item = item_pair.second;
        auto key = item_pair.first;

        if (item->buffer.size == 0) {
            manager->queues.erase(key);
            queue::addrinfo_free(item);
            return get_target_time(manager, current_time);
        } else if (item->next_due_time == 0) {
            item->next_due_time = current_time;
        } else if (item->next_due_time - current_time >= 1500000LL) {
            return item->next_due_time;
        }

        queue::pop(item, packet_out);

        manager->queues.erase(key);
        manager->queues.insert({key, item});

        current_time = timing_get_nanos();

        if (current_time - item->next_due_time >= 2 * manager->packet_interval) {
            item->next_due_time = current_time + manager->packet_interval;
        } else {
            item->next_due_time += manager->packet_interval;
        }

        return get_target_time(manager, current_time);
    }

    static void dispatch_packet(socket_t socket_vx, packet::Unsent &unsent_packet) {
        sendto(socket_vx, reinterpret_cast<const char*>(unsent_packet.packet.data.data()), unsent_packet.packet.length, 0,
               unsent_packet.address->ai_addr, sizeof(*unsent_packet.address->ai_addr));

        unsent_packet.packet.data.clear();
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
            packet::Unsent packet_to_send;

            int64_t target_time = process_next_locked(manager, packet_to_send, current_time);
            mutex_unlock(manager->lock);

            if (!packet_to_send.packet.data.empty()) {
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
    return reinterpret_cast<jlong>(manager::create(queue_buffer_capacity, packet_interval));
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_destroy(JNIEnv* jni, jobject me,
                                                                                      jlong instance) {
    manager::destroy(reinterpret_cast<queue::Manager*>(instance));
}

JNIEXPORT jint JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_getRemainingCapacity(JNIEnv* jni,
                                                                                                   jobject me,
                                                                                                   jlong instance,
                                                                                                   jlong key) {
    return manager::get_remaining_capacity(reinterpret_cast<queue::Manager*>(instance), key);
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

    bool result = queue::push(reinterpret_cast<queue::Manager*>(instance), key, address, port, bytes,
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

    bool result = queue::push(reinterpret_cast<queue::Manager*>(instance), key, address, port, bytes,
                              data_length, socket_handle);

    jni->ReleaseStringUTFChars(address_string, address);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_deleteQueue(
        JNIEnv* jni, jobject me, jlong instance, jlong key) {

    bool result = queue::remove(reinterpret_cast<queue::Manager*>(instance), key);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_process(JNIEnv* jni, jobject me,
                                                                                      jlong instance) {
    manager::process((queue::Manager*) instance);
}

JNIEXPORT void JNICALL Java_com_sedmelluq_discord_lavaplayer_udpqueue_natives_UdpQueueManagerLibrary_processWithSocket(
        JNIEnv* jni, jobject me, jlong instance, jlong socket_v4, jlong socket_v6) {

    manager::process_with_socket((queue::Manager*) instance, (socket_t) socket_v4, (socket_t) socket_v6);
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
