#include "resonance_log.h"
#include <array>
#include <atomic>
#include <cstring>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <iostream>
#include <thread>

namespace godot {

namespace {

constexpr int kLogRingSlots = 64;
constexpr size_t kLogCategoryCap = 24;
constexpr size_t kLogTextCap = 480;

enum class PostedLevel : uint8_t { Info = 0,
                                   Warn = 1,
                                   Error = 2 };

struct LogSlot {
    std::atomic<uint32_t> ticket{0};
    PostedLevel level = PostedLevel::Info;
    char category[kLogCategoryCap]{};
    char text[kLogTextCap]{};
};

std::array<LogSlot, kLogRingSlots> g_log_ring{};
std::atomic<uint32_t> g_log_next_ticket{0};
std::atomic<uint32_t> g_log_drained_ticket{0};
std::atomic<bool> g_main_thread_bound{false};
std::thread::id g_main_thread_id;

void copy_trunc(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void emit_on_main(PostedLevel level, const char* category, const char* text) {
    const char* cat = (category && category[0]) ? category : "log";
    const char* body = (text && text[0]) ? text : "";
    String full_msg = String("Nexus Resonance: ") + String(body);

    switch (level) {
    case PostedLevel::Info:
        UtilityFunctions::print("Nexus Resonance: ", String(body));
        resonance_logger_log(cat, full_msg.utf8().get_data(), Dictionary());
        break;
    case PostedLevel::Warn:
        UtilityFunctions::push_warning(full_msg);
        resonance_logger_log(cat, full_msg.utf8().get_data(), Dictionary());
        break;
    case PostedLevel::Error:
        UtilityFunctions::push_error(full_msg);
        resonance_logger_log(cat, full_msg.utf8().get_data(), Dictionary());
        break;
    }
}

void post_to_ring(PostedLevel level, const char* category, const char* utf8_text) {
    const uint32_t ticket = g_log_next_ticket.fetch_add(1, std::memory_order_relaxed) + 1u;
    LogSlot& slot = g_log_ring[static_cast<size_t>((ticket - 1u) % static_cast<uint32_t>(kLogRingSlots))];
    slot.ticket.store(0, std::memory_order_relaxed);
    slot.level = level;
    copy_trunc(slot.category, kLogCategoryCap, category);
    copy_trunc(slot.text, kLogTextCap, utf8_text);
    slot.ticket.store(ticket, std::memory_order_release);
}

bool on_main_thread() {
    return g_main_thread_bound.load(std::memory_order_acquire) &&
           std::this_thread::get_id() == g_main_thread_id;
}

void log_utf8(PostedLevel level, const char* category, const char* utf8_text) {
    if (!utf8_text)
        utf8_text = "";
    if (on_main_thread()) {
        emit_on_main(level, category, utf8_text);
        return;
    }
    post_to_ring(level, category, utf8_text);
}

} // namespace

void resonance_log_bind_main_thread() {
    g_main_thread_id = std::this_thread::get_id();
    g_main_thread_bound.store(true, std::memory_order_release);
}

void resonance_log_drain_pending() {
    if (!on_main_thread())
        return;

    const uint32_t target = g_log_next_ticket.load(std::memory_order_acquire);
    uint32_t drained = g_log_drained_ticket.load(std::memory_order_relaxed);
    while (drained < target) {
        const uint32_t ticket = drained + 1u;
        LogSlot& slot = g_log_ring[static_cast<size_t>((ticket - 1u) % static_cast<uint32_t>(kLogRingSlots))];
        const uint32_t published = slot.ticket.load(std::memory_order_acquire);
        if (published != ticket) {
            drained++;
            continue;
        }
        emit_on_main(slot.level, slot.category, slot.text);
        drained++;
    }
    g_log_drained_ticket.store(drained, std::memory_order_release);
}

ResonanceLog::LogLevel ResonanceLog::current_level = ResonanceLog::LEVEL_WARN;

void ResonanceLog::set_level(LogLevel p_level) {
    current_level = p_level;
}

void ResonanceLog::info(const String& p_msg) {
    if (current_level >= LEVEL_INFO) {
        String full_msg = "Nexus Resonance: " + p_msg;
        if (!on_main_thread())
            std::cout << full_msg.utf8().get_data() << std::endl;
        log_utf8(PostedLevel::Info, "init", full_msg.utf8().get_data());
    }
}

void ResonanceLog::warn(const String& p_msg) {
    if (current_level >= LEVEL_WARN) {
        String full_msg = "Nexus Resonance: " + p_msg;
        if (!on_main_thread())
            std::cout << full_msg.utf8().get_data() << std::endl;
        log_utf8(PostedLevel::Warn, "warn", full_msg.utf8().get_data());
    }
}

void ResonanceLog::error(const String& p_msg) {
    if (current_level >= LEVEL_ERROR) {
        String full_msg = "Nexus Resonance: " + p_msg;
        if (!on_main_thread())
            std::cerr << full_msg.utf8().get_data() << std::endl;
        log_utf8(PostedLevel::Error, "error", full_msg.utf8().get_data());
    }
}

void ResonanceLog::trace(const String& p_msg) {
    if (current_level >= LEVEL_TRACE) {
        std::cout << "Nexus Resonance: " << p_msg.utf8().get_data() << std::endl;
        std::flush(std::cout);
    }
}

void ResonanceLog::check_ptr(const char* name, void* ptr) {
    if (current_level >= LEVEL_TRACE) {
        String status = (ptr == nullptr) ? "NULL" : "VALID";
        String msg = String("PTR CHECK: ") + String(name) + " is " + status;
        std::cout << "Nexus Resonance: " << msg.utf8().get_data() << std::endl;
        if (ptr == nullptr)
            error(String(name) + " is NULL!");
    }
}

void resonance_logger_log(const char* category, const char* message, Dictionary data) {
    if (!category || !message)
        return;
    Engine* eng = Engine::get_singleton();
    if (!eng || !eng->has_singleton("ResonanceLogger"))
        return;
    Variant logger_var = eng->get_singleton("ResonanceLogger");
    if (logger_var.get_type() != Variant::OBJECT)
        return;
    Object* logger_obj = logger_var.operator Object*();
    if (!logger_obj || !logger_obj->has_method("log"))
        return;
    logger_obj->call_deferred(StringName("log"), StringName(category), String(message), data);
}

} // namespace godot
