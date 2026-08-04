#ifndef RESONANCE_LOG_H
#define RESONANCE_LOG_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

/// Forward to ResonanceLogger (GDScript) when available. Category for thematic filtering.
/// Always deferred to the main thread (safe from audio / worker threads).
void resonance_logger_log(const char* category, const char* message, Dictionary data);

/// Call once from the Godot main thread (e.g. first ResonanceServer::tick).
void resonance_log_bind_main_thread();

/// Drain lock-free log posts from audio/worker threads; call from main thread only.
void resonance_log_drain_pending();

class ResonanceLog {
  public:
    enum LogLevel {
        LEVEL_NONE = 0,
        LEVEL_ERROR = 1,
        LEVEL_WARN = 2,
        LEVEL_INFO = 3,
        LEVEL_TRACE = 4 // Very verbose, prints every step
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

    // Trace is special: It flushes immediately to std::cout to survive crashes
    static void trace(const String& p_msg);

    // Helper to check pointer validity
    static void check_ptr(const char* name, void* ptr);
};

} // namespace godot

#endif