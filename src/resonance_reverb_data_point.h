#ifndef RESONANCE_REVERB_DATA_POINT_H
#define RESONANCE_REVERB_DATA_POINT_H

#include "resonance_constants.h"
#include "resonance_probe_data.h"
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

/// Editor/analysis marker: sample baked reflections at a world point from ResonanceProbeData.
/// Inverse-distance probe interpolation; does not affect runtime audio by itself.
class ResonanceReverbDataPoint : public Node3D {
    GDCLASS(ResonanceReverbDataPoint, Node3D)

  private:
    Ref<ResonanceProbeData> probe_data;
    int baked_variation = 0;
    Vector3 static_endpoint;
    float static_influence_radius = 0.0f;
    float neighbor_radius = resonance::kStaticSourceProbeNeighborRadiusM;
    bool reconstruct_impulse_response = false;
    Dictionary last_query;
    bool did_play_query_ = false;

    void _try_inherit_probe_data_from_ancestor();
    void _try_play_query_once();

  protected:
    static void _bind_methods();
    void _validate_property(PropertyInfo& p_property) const;

  public:
    void _enter_tree() override;
    void _process(double delta) override;

    void set_probe_data(const Ref<ResonanceProbeData>& p_data);
    Ref<ResonanceProbeData> get_probe_data() const;

    void set_baked_variation(int p_variation);
    int get_baked_variation() const;

    void set_static_endpoint(const Vector3& p_endpoint);
    Vector3 get_static_endpoint() const;

    void set_static_influence_radius(float p_radius);
    float get_static_influence_radius() const;

    void set_neighbor_radius(float p_radius);
    float get_neighbor_radius() const;

    void set_reconstruct_impulse_response(bool p_enabled);
    bool get_reconstruct_impulse_response() const;

    Dictionary get_last_query() const;

    /// Sample baked layer at this node's global position. Requires live ResonanceServer context (editor or runtime).
    Dictionary query_baked_reverb();
    Dictionary query_baked_reverb_at(const Vector3& world_position);
};

} // namespace godot

#endif
