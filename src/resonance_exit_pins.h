#ifndef RESONANCE_EXIT_PINS_H
#define RESONANCE_EXIT_PINS_H

#include "resonance_exit_pin_registry.h"

#include <godot_cpp/classes/ref_counted.hpp>

namespace resonance {

ExitPinRegistry<godot::RefCounted>& exit_pins();

/// Member for classes the AudioServer can still hold after the extension unloads (it is
/// destroyed after scene extensions at exit). Tracks its owner for the owner's lifetime.
class ExitPinned {
  public:
    explicit ExitPinned(godot::RefCounted* owner) : owner_(owner) { exit_pins().track(owner_); }
    ~ExitPinned() { exit_pins().untrack(owner_); }
    ExitPinned(const ExitPinned&) = delete;
    ExitPinned& operator=(const ExitPinned&) = delete;
    ExitPinned(ExitPinned&&) = delete;
    ExitPinned& operator=(ExitPinned&&) = delete;

  private:
    godot::RefCounted* owner_;
};

/// Call from the module terminator, before godot-cpp unregisters the classes. A late final
/// unreference would read the freed class table; an extra reference keeps it from ever happening.
void pin_exit_survivors();

} // namespace resonance

#endif
