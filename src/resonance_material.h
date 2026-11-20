#ifndef RESONANCE_MATERIAL_H
#define RESONANCE_MATERIAL_H

#include <godot_cpp/classes/resource.hpp>
#include <phonon.h>

namespace godot {

class ResonanceMaterial : public Resource {
    GDCLASS(ResonanceMaterial, Resource)

  private:
    // Bands: low up to ~800 Hz, mid ~800 Hz - 8 kHz, high above 8 kHz. All coefficients are 0..1.
    float low_freq_absorption = 0.10f;
    float mid_freq_absorption = 0.20f;
    float high_freq_absorption = 0.30f;

    // 0 is mirror-like, 1 fully diffuse.
    float scattering = 0.05f;

    float transmission_low = 0.01f;
    float transmission_mid = 0.01f;
    float transmission_high = 0.01f;

  protected:
    static void _bind_methods();

  public:
    ResonanceMaterial() = default;

    void set_low_freq_absorption(float p_val);
    float get_low_freq_absorption() const;

    void set_mid_freq_absorption(float p_val);
    float get_mid_freq_absorption() const;

    void set_high_freq_absorption(float p_val);
    float get_high_freq_absorption() const;

    void set_scattering(float p_val);
    float get_scattering() const;

    void set_transmission_low(float p_val);
    float get_transmission_low() const;
    void set_transmission_mid(float p_val);
    float get_transmission_mid() const;
    void set_transmission_high(float p_val);
    float get_transmission_high() const;

    // Deprecated: sets all three transmission bands.
    void set_transmission(float p_val);
    float get_transmission() const;

    IPLMaterial get_ipl_material() const;

    /// Material used for geometry without a ResonanceMaterial.
    static IPLMaterial default_ipl_material();
    static IPLMaterial ipl_material_or_default(const Ref<ResonanceMaterial>& p_material);
};

} // namespace godot

#endif // RESONANCE_MATERIAL_H