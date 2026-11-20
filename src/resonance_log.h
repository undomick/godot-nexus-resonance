#ifndef RESONANCE_LOG_H
#define RESONANCE_LOG_H

#include <cstdint>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

/// Forwards to the ResonanceLogger singleton when present, via call_deferred.
void resonance_logger_log(const char* category, const char* message, Dictionary data);

/// Call once from the Godot main thread (e.g. first ResonanceServer::tick).
void resonance_log_bind_main_thread();

/// Drain lock-free log posts from audio/worker threads; call from main thread only.
void resonance_log_drain_pending();

/// Undrained posts lost when the log ring wrapped.
uint64_t resonance_log_get_drop_count();

class ResonanceLog {
  public:
    enum LogLevel {
        LEVEL_NONE = 0,
        LEVEL_ERROR = 1,
        LEVEL_WARN = 2,
        LEVEL_INFO = 3,
        LEVEL_TRACE = 4
    };

  private:
    static LogLevel current_level;

  public:
    static void set_level(LogLevel p_level);

    static void info(const String& p_msg);
    static void warn(const String& p_msg);
    static void error(const String& p_msg);
    /// UTF-8 C-string path for audio/worker threads (no Godot String alloc on the caller).
    static void warn_cstr(const char* p_msg);
    static void error_cstr(const char* p_msg);

    // Writes and flushes stdout directly so the line survives a crash.
    static void trace(const String& p_msg);

    static void check_ptr(const char* name, void* ptr);
};

} // namespace godot

#endif