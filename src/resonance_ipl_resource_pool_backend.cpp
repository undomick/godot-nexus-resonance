#include "resonance_ipl_resource_pool_backend.h"
#include "resonance_log.h"

#if defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace godot {

namespace {
constexpr int kPoolThreadNice = 10;
}

IPLSource IplResourcePoolBackend::create_source() const {
    IPLSourceSettings settings = source;
    IPLSource created = nullptr;
    if (iplSourceCreate(simulator, &settings, &created) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error_cstr("ResonancePlayerResourcePool: iplSourceCreate failed; the pool stops refilling.");
        return nullptr;
    }
    return created;
}

IPLReflectionEffect IplResourcePoolBackend::create_effect() const {
    IPLAudioSettings audio_settings = reflection.audio;
    IPLReflectionEffectSettings settings = reflection.settings;
    IPLReflectionEffect created = nullptr;
    if (iplReflectionEffectCreate(context, &audio_settings, &settings, &created) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error_cstr("ResonancePlayerResourcePool: iplReflectionEffectCreate failed; the pool stops refilling.");
        return nullptr;
    }
    return created;
}

void IplResourcePoolBackend::reset(IPLReflectionEffect effect) const { iplReflectionEffectReset(effect); }

void IplResourcePoolBackend::release(IPLSource released) const {
    if (released)
        iplSourceRelease(&released);
}

void IplResourcePoolBackend::release(IPLReflectionEffect effect) const {
    if (effect)
        iplReflectionEffectRelease(&effect);
}

void IplResourcePoolBackend::on_pool_thread_start() const {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "NexusPool");
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), kPoolThreadNice);
#endif
}

} // namespace godot
