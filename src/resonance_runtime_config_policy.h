#ifndef RESONANCE_RUNTIME_CONFIG_POLICY_H
#define RESONANCE_RUNTIME_CONFIG_POLICY_H

namespace resonance {

/// Takes a ResonanceRuntimeConfig property name.
bool runtime_config_property_requires_engine_reinit(const char* property);

bool runtime_config_property_is_live_patchable(const char* property);

bool runtime_config_property_requires_routing_refresh(const char* property);

/// Takes a get_config() dictionary key, not a property name.
bool runtime_config_dict_key_is_live_patchable(const char* key);

} // namespace resonance

#endif
