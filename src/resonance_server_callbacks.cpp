#include "resonance_server.h"
#include "resonance_utils.h"
#include <mutex>

using namespace godot;

void IPLCALL ResonanceServer::_pathing_vis_callback(IPLVector3 from, IPLVector3 to, IPLbool occluded, void* userData) {
    if (ResonanceServer::is_shutting_down_flag.load(std::memory_order_acquire) || ResonanceServer::ipl_audio_teardown_active())
        return;
    ResonanceServer* server = static_cast<ResonanceServer*>(userData);
    if (!server)
        return;
    PathVisSegment seg;
    seg.from = ResonanceUtils::to_godot_vector3(from);
    seg.to = ResonanceUtils::to_godot_vector3(to);
    seg.occluded = (occluded == IPL_TRUE);
    std::lock_guard<std::mutex> lock(server->pathing_vis_mutex);
    if ((int)server->pathing_vis_segments.size() >= resonance::kPathingVisMaxSegmentsPerTick)
        return;
    server->pathing_vis_segments.push_back(seg);
}
