#include "resonance_exit_pins.h"

namespace resonance {

ExitPinRegistry<godot::RefCounted>& exit_pins() {
    static ExitPinRegistry<godot::RefCounted> registry;
    return registry;
}

void pin_exit_survivors() {
    exit_pins().pin_survivors([](godot::RefCounted* obj) { obj->reference(); });
}

} // namespace resonance
