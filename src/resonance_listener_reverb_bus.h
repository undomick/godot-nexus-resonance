#ifndef RESONANCE_LISTENER_REVERB_BUS_H
#define RESONANCE_LISTENER_REVERB_BUS_H

#include "resonance_mixer_processor.h"
#include "resonance_processor_reflection.h"
#include <godot_cpp/classes/audio_frame.hpp>
#include <phonon.h>

namespace godot {

class ResonanceServer;

// Reverb of the listener reverb source applied once to everything sent to the reverb bus.
// Convolution and TAN feed the shared reflection mixer; Parametric and Hybrid add their wet to the output.
class ResonanceListenerReverbBus {
  public:
    ResonanceListenerReverbBus() = default;
    ~ResonanceListenerReverbBus() { cleanup(); }
    ResonanceListenerReverbBus(const ResonanceListenerReverbBus&) = delete;
    ResonanceListenerReverbBus& operator=(const ResonanceListenerReverbBus&) = delete;

    /// Main thread.
    bool initialize(ResonanceServer& srv);
    void cleanup();
    bool is_ready() const { return ready_; }

    /// Audio thread. Call before dst is overwritten: the bus hands src and dst as the same buffer.
    void capture_input(const AudioFrame* src, int frame_count);
    /// Audio thread. Adds the parametric or hybrid wet to dst; convolution and TAN wet reach dst through the
    /// mixer return.
    void apply(ResonanceServer& srv, AudioFrame* dst, int frame_count, IPLReflectionMixer mixer,
               const IPLCoordinateSpace3& listener);

  private:
    void _add_direct_wet(ResonanceServer& srv, AudioFrame* dst, int frame_count, const IPLCoordinateSpace3& listener);

    ResonanceReflectionProcessor reflection_;
    ResonanceMixerProcessor decoder_;
    IPLContext context_ = nullptr;
    IPLAudioBuffer input_{};
    int frame_size_ = 0;
    bool ready_ = false;
};

} // namespace godot

#endif // RESONANCE_LISTENER_REVERB_BUS_H
