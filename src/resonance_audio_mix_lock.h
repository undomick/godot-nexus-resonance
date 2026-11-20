#ifndef RESONANCE_AUDIO_MIX_LOCK_H
#define RESONANCE_AUDIO_MIX_LOCK_H

#include <godot_cpp/classes/audio_server.hpp>

namespace godot {

/// Godot runs each whole mix block under the AudioServer lock: while this is held, no _mix is mid-block.
/// Main thread only.
class AudioMixBlockLock {
  public:
    AudioMixBlockLock() : audio_(AudioServer::get_singleton()) {
        if (audio_)
            audio_->lock();
    }
    ~AudioMixBlockLock() {
        if (audio_)
            audio_->unlock();
    }

    AudioMixBlockLock(const AudioMixBlockLock&) = delete;
    AudioMixBlockLock& operator=(const AudioMixBlockLock&) = delete;
    AudioMixBlockLock(AudioMixBlockLock&&) = delete;
    AudioMixBlockLock& operator=(AudioMixBlockLock&&) = delete;

  private:
    AudioServer* audio_ = nullptr;
};

} // namespace godot

#endif // RESONANCE_AUDIO_MIX_LOCK_H
