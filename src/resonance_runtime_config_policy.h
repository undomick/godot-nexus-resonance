#ifndef RESONANCE_RUNTIME_CONFIG_POLICY_H
#define RESONANCE_RUNTIME_CONFIG_POLICY_H

namespace resonance {

/// SSOT: ResonanceRuntimeConfig property names (UTF-8) that require ipl reinit.
bool runtime_config_property_requires_engine_reinit(const char* property);

bool runtime_config_property_is_live_patchable(const char* property);

bool runtime_config_property_requires_routing_refresh(const char* property);

/// SSOT: get_config() keys safe to patch on a running engine without reinit.
bool runtime_config_dict_key_is_live_patchable(const char* key);

} // namespace resonance

#endif
