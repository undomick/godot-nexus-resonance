#pragma once
// Thread-safe; callable from the Steam Audio worker thread. Set the path on the main thread before the
// simulation starts.

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

namespace resonance {
inline std::string g_debug_log_path = "nexus_resonance_debug.log";
inline std::mutex g_debug_log_mutex;

inline std::string _debug_log_escape_json(const char* s) {
    if (!s)
        return "";
    std::string out;
    for (; *s; ++s) {
        switch (*s) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += *s;
            break;
        }
    }
    return out;
}

inline void set_debug_log_path(const char* path) {
    std::lock_guard<std::mutex> lock(g_debug_log_mutex);
    g_debug_log_path = (path && path[0]) ? path : "nexus_resonance_debug.log";
}

inline void debug_log_raw(const char* loc, const char* msg, int v1, int v2) {
    if (!loc)
        loc = "";
    if (!msg)
        msg = "";
    std::string loc_esc = _debug_log_escape_json(loc);
    std::string msg_esc = _debug_log_escape_json(msg);
    std::lock_guard<std::mutex> lock(g_debug_log_mutex);
    std::ofstream f(g_debug_log_path, std::ios::app);
    if (!f)
        return;
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    f << "{\"sessionId\":\"e92d3b\",\"location\":\"" << loc_esc << "\",\"message\":\"" << msg_esc << "\"";
    if (v1 >= 0)
        f << ",\"v1\":" << v1;
    if (v2 >= 0)
        f << ",\"v2\":" << v2;
    f << ",\"timestamp\":" << ts << "}\n";
    f.flush();
}
} // namespace resonance
