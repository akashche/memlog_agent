/*
 * Copyright 2017, akashche at redhat.com
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE.md file that
 * accompanied this code).
 */

/* 
 * File:   agentmain.cpp
 * Author: alex
 *
 * Created on April 12, 2017, 4:29 PM
 */

#include <cctype>
#include <cstring>
#include <atomic>
#include <iostream>
#include <string>

#include "staticlib/config.hpp"
#include "staticlib/cron.hpp"
#include "staticlib/io.hpp"
#include "staticlib/jni.hpp"
#include "staticlib/json.hpp"
#include "staticlib/jvmti.hpp"
#include "staticlib/support.hpp"
#include "staticlib/tinydir.hpp"
#include "staticlib/utils.hpp"

#include "config.hpp"
#include "collect_mem_os.hpp"
#include "memlog_exception.hpp"

namespace memlog {

namespace { // anonymous

std::atomic<uint64_t>& static_gcs_count() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

} // namespace

class agent : public sl::jvmti::agent_base<agent> {
    config cf;
    sl::cron::expression cron;
    sl::json::array_writer<sl::io::buffered_sink<sl::tinydir::file_sink>> log_writer;
    
public:
    agent(JavaVM* jvm, char* options) :
    sl::jvmti::agent_base<agent>(jvm, options),
    cf(read_config()),
    cron(cf.cron_expr),
    log_writer(sl::io::make_buffered_sink(sl::tinydir::file_sink(cf.output_path_json))) {
        write_to_stdout("agent created");
    }
    
    void operator()() STATICLIB_NOEXCEPT {
        write_to_stdout("agent initialized");
        try {
            while(sl::jni::static_java_vm().running()) {
                collect_and_write_measurement();
                auto millis = cron.next<std::chrono::milliseconds>();
                sl::jni::static_java_vm().thread_sleep_before_shutdown(millis/cf.timeout_divider);
            }
            // all spawned threads must be joined at this point
            write_to_stdout("shutting down");
        } catch(const std::exception& e) {
            std::cerr << TRACEMSG(e.what() + "\nWorker error") << std::endl;
        } catch(...) {
            std::cerr << TRACEMSG("Unexpected worker error") << std::endl;
        }
    }
    
    std::unique_ptr<jvmtiCapabilities> capabilities() {
        auto caps = sl::support::make_unique<jvmtiCapabilities>();
        std::memset(caps.get(), 0, sizeof (*caps));
        caps->can_generate_garbage_collection_events = 1;
        return caps;
    }
    
    std::unique_ptr<jvmtiEventCallbacks> callbacks() {
        auto cbs = sl::support::make_unique<jvmtiEventCallbacks>();
        std::memset(cbs.get(), 0, sizeof (*cbs));
        // gc finish events
        cbs->GarbageCollectionFinish = agent::gc_finish_event;
        sl::jvmti::error_checker ec;
        ec = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, nullptr);
        
        return cbs;
    }
    
    bool can_write_stdout() {
        return cf.stdout_messages;
    }
    
private:
    void collect_and_write_measurement() {
        log_writer.write({
            { "currentTimeMillis", current_time_millis() },
            { "gcEventsCount", static_gcs_count().load(std::memory_order_relaxed) },
            { "os", collect_mem_from_os() },
            { "jvm", collect_mem_from_jvm() }
        });
    }
    
    sl::json::value collect_mem_from_jvm() {
        auto mucls = sl::jni::jclass_ptr("java/lang/management/MemoryUsage");
        // heap
        auto muheap = jmm.call_object_method(mucls, &JmmInterface::GetMemoryUsage, true);
        auto heap_committed = muheap.call_method<jlong>("getCommitted", "()J", &JNIEnv::CallLongMethod);
        auto heap_init = muheap.call_method<jlong>("getInit", "()J", &JNIEnv::CallLongMethod);
        auto heap_max = muheap.call_method<jlong>("getMax", "()J", &JNIEnv::CallLongMethod);
        auto heap_used = muheap.call_method<jlong>("getUsed", "()J", &JNIEnv::CallLongMethod);
        // non-heap
        auto munh = jmm.call_object_method(mucls, &JmmInterface::GetMemoryUsage, false);
        auto nh_committed = munh.call_method<jlong>("getCommitted", "()J", &JNIEnv::CallLongMethod);
        auto nh_init = munh.call_method<jlong>("getInit", "()J", &JNIEnv::CallLongMethod);
        auto nh_max = munh.call_method<jlong>("getMax", "()J", &JNIEnv::CallLongMethod);
        auto nh_used = munh.call_method<jlong>("getUsed", "()J", &JNIEnv::CallLongMethod);
        
        auto overall = heap_committed + nh_committed;
        return {
            {"overall", overall},
            {"heap", {
                {"committed", heap_committed},
                {"init", heap_init},
                {"max", heap_max},
                {"used", heap_used}
            }},
            {"nonHeap", {
                {"committed", nh_committed},
                {"init", nh_init},
                {"max", nh_max},
                {"used", nh_used}
            }}
        };
    }
    
    config read_config() {
        std::string path = [this] {
            if (!options.empty()) {
                return options;
            }
            // points to jvm/bin
            // auto exepath = sl::utils::current_executable_path();
            // auto exedir = sl::utils::strip_filename(exepath);
            // return exedir + "config.json";
            return std::string("config.json");
        }();
        auto src = sl::tinydir::file_source(path);
        auto json = sl::json::load(src);
        return config(json);
    }
    
    void write_to_stdout(const std::string& message) {
        if (cf.stdout_messages) {
            std::cout<< "memlog_agent: " << message << std::endl;
        }
    }
    
    uint64_t current_time_millis() {
        auto val = std::chrono::system_clock::now().time_since_epoch();
        std::chrono::milliseconds millis = std::chrono::duration_cast<std::chrono::milliseconds>(val);
        return static_cast<uint64_t>(millis.count());
    }
    
    static void gc_finish_event(jvmtiEnv*) {
        // http://en.cppreference.com/w/cpp/atomic/memory_order#Relaxed_ordering
        static_gcs_count().fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

memlog::agent* global_agent = nullptr;

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* /* reserved */) {
    try {
        global_agent = new memlog::agent(jvm, options);
        return JNI_OK;
    } catch (const std::exception& e) {
        std::cerr << TRACEMSG(e.what() + "\nInitialization error") << std::endl;
        return JNI_ERR;
    }
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* /* vm */) {
    bool can_write = global_agent->can_write_stdout();
    delete global_agent;
    if (can_write) {
        std::cout << "memlog_agent: shutdown complete" << std::endl;
    }
}
