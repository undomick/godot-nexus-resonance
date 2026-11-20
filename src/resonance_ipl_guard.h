#ifndef RESONANCE_IPL_GUARD_H
#define RESONANCE_IPL_GUARD_H

#include <phonon.h>

namespace godot {

/// Releases an IPL handle on scope exit. release_fn takes the address of the handle,
/// e.g. IPLScopedRelease<IPLSerializedObject> guard(obj, iplSerializedObjectRelease).
template <typename T>
struct IPLScopedRelease {
    T ptr;
    void (*release_fn)(T*);

    IPLScopedRelease(T p, void (*fn)(T*)) : ptr(p), release_fn(fn) {}
    ~IPLScopedRelease() {
        if (ptr && release_fn)
            release_fn(&ptr);
    }
    /// Hands ownership to the caller; the guard no longer releases the handle.
    void detach() { ptr = nullptr; }
    IPLScopedRelease(const IPLScopedRelease&) = delete;
    IPLScopedRelease& operator=(const IPLScopedRelease&) = delete;
};

} // namespace godot

#endif // RESONANCE_IPL_GUARD_H
