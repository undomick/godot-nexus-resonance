#ifndef RESONANCE_SHUTDOWN_PLAYBACK_POLICY_H
#define RESONANCE_SHUTDOWN_PLAYBACK_POLICY_H

#include <cstdint>

namespace resonance {

/// While true, the audio thread outputs silence on the mix and reverb paths.
inline bool ipl_audio_teardown_blocks_mix(bool is_shutting_down, bool ipl_teardown_active) {
    return is_shutting_down || ipl_teardown_active;
}

/// Phases run strictly in order: the worker must be joined before IPL objects are released.
enum class ShutdownPhase : uint8_t {
    Idle = 0,
    TeardownFlagsSet,
    WorkerJoined,
    CachesInvalidated,
    ClientsDrained,
    SimulationTeardownDone,
};

inline bool shutdown_phase_transition_allowed(ShutdownPhase from, ShutdownPhase to) {
    if (from == to)
        return true;
    switch (from) {
    case ShutdownPhase::Idle:
        return to == ShutdownPhase::TeardownFlagsSet;
    case ShutdownPhase::TeardownFlagsSet:
        return to == ShutdownPhase::WorkerJoined;
    case ShutdownPhase::WorkerJoined:
        return to == ShutdownPhase::CachesInvalidated;
    case ShutdownPhase::CachesInvalidated:
        return to == ShutdownPhase::ClientsDrained;
    case ShutdownPhase::ClientsDrained:
        return to == ShutdownPhase::SimulationTeardownDone;
    case ShutdownPhase::SimulationTeardownDone:
        return false;
    }
    return false;
}

} // namespace resonance

#endif // RESONANCE_SHUTDOWN_PLAYBACK_POLICY_H
