// Times raw iplReflectionEffectApply (convolution) for several distinct and shared IRs.
// Talks to the Steam Audio C API only; no Godot and no Nexus runtime classes.

#include <phonon.h>

#include "resonance_math.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

constexpr int kDefaultSampleRate = 48000;
constexpr int kFrameSize = 512;
constexpr int kAmbisonicsOrder = 1;
constexpr int kChannels = (kAmbisonicsOrder + 1) * (kAmbisonicsOrder + 1);
constexpr int kMaxSources = 32;
constexpr int kNumRays = 4096;
constexpr int kNumBounces = 16;
constexpr int kNumDiffuseSamples = 32;
constexpr float kIrradianceMinDistance = 1.0f;
constexpr uint32_t kNoiseSeed = 12345u;
constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;
// Realtime settings of the Nexus perf check's convolution scenarios.
constexpr int kDefaultSimIntervalMs = 100;
constexpr int kDefaultSimRays = 1024;
constexpr int kDefaultSimBounces = 4;

// What a second thread simulates while the effects are applied.
enum class SimLoad {
    None,
    Other, // a separate simulator: CPU and cache load only
    Same,  // the sources whose IRs are applied, so the IRs change under the effects
};

struct Options {
    int blocks = 300;
    int sample_rate = kDefaultSampleRate;
    int warmup_blocks = 50;
    std::string only;
    bool json = false;
    SimLoad sim_load = SimLoad::None;
    int sim_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    int sim_interval_ms = kDefaultSimIntervalMs;
    int sim_rays = kDefaultSimRays;
    int sim_bounces = kDefaultSimBounces;
    std::optional<int> audio_cpu;
    std::optional<int> sim_cpu;
    int sim_nice = 0;
    bool paced = false;
};

struct Scenario {
    std::string name;
    int num_effects = 0;
    float ir_duration_sec = 0.0f;
    bool shared_ir = false;
};

struct Result {
    Scenario scenario;
    int ir_size_samples = 0;
    int distinct_irs = 0;
    double apply_mean_us = 0.0;
    double apply_p95_us = 0.0;
    double apply_p99_us = 0.0;
    double apply_max_us = 0.0;
    double block_mean_us = 0.0;
    double block_max_us = 0.0;
    double mixer_mean_us = 0.0;
    int sim_runs = 0;
    double sim_run_mean_us = 0.0;
};

const std::vector<Scenario>& all_scenarios() {
    static const std::vector<Scenario> scenarios = {
        {"distinct_1", 1, 2.0f, false},
        {"distinct_8", 8, 2.0f, false},
        {"distinct_32", 32, 2.0f, false},
        {"shared_8", 8, 2.0f, true},
        {"distinct_8_1s", 8, 1.0f, false},
    };
    return scenarios;
}

std::optional<SimLoad> parse_sim_load(const std::string& value) {
    if (value == "none")
        return SimLoad::None;
    if (value == "other")
        return SimLoad::Other;
    if (value == "same")
        return SimLoad::Same;
    return std::nullopt;
}

const char* sim_load_name(SimLoad load) {
    switch (load) {
    case SimLoad::Other:
        return "other";
    case SimLoad::Same:
        return "same";
    case SimLoad::None:
        break;
    }
    return "none";
}

int int_after(const std::string& arg, const char* prefix) {
    return std::atoi(arg.c_str() + std::strlen(prefix));
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            options.json = true;
        } else if (arg == "--paced") {
            options.paced = true;
        } else if (arg.rfind("--sim-load=", 0) == 0) {
            const std::optional<SimLoad> load = parse_sim_load(arg.substr(std::strlen("--sim-load=")));
            if (!load) {
                std::fprintf(stderr, "--sim-load must be none, other or same\n");
                return std::nullopt;
            }
            options.sim_load = *load;
        } else if (arg.rfind("--sim-threads=", 0) == 0) {
            options.sim_threads = int_after(arg, "--sim-threads=");
        } else if (arg.rfind("--sim-interval-ms=", 0) == 0) {
            options.sim_interval_ms = int_after(arg, "--sim-interval-ms=");
        } else if (arg.rfind("--sim-rays=", 0) == 0) {
            options.sim_rays = int_after(arg, "--sim-rays=");
        } else if (arg.rfind("--sim-bounces=", 0) == 0) {
            options.sim_bounces = int_after(arg, "--sim-bounces=");
        } else if (arg.rfind("--sim-nice=", 0) == 0) {
            options.sim_nice = int_after(arg, "--sim-nice=");
        } else if (arg.rfind("--audio-cpu=", 0) == 0) {
            options.audio_cpu = int_after(arg, "--audio-cpu=");
        } else if (arg.rfind("--sim-cpu=", 0) == 0) {
            options.sim_cpu = int_after(arg, "--sim-cpu=");
        } else if (arg.rfind("--blocks=", 0) == 0) {
            options.blocks = std::atoi(arg.c_str() + std::strlen("--blocks="));
        } else if (arg.rfind("--sample-rate=", 0) == 0) {
            options.sample_rate = std::atoi(arg.c_str() + std::strlen("--sample-rate="));
        } else if (arg.rfind("--only=", 0) == 0) {
            options.only = arg.substr(std::strlen("--only="));
        } else {
            std::fprintf(stderr, "Unknown argument: %s\nUsage: convolution_apply_bench [--blocks=N] [--sample-rate=HZ] [--only=<scenario>] [--json]\n"
                                 "  [--sim-load=none|other|same] [--sim-threads=N] [--sim-interval-ms=MS] [--sim-rays=N] [--sim-bounces=N]\n"
                                 "  [--sim-nice=N] [--paced] [--audio-cpu=N] [--sim-cpu=N]\n",
                         arg.c_str());
            return std::nullopt;
        }
    }
    if (options.blocks < 1) {
        std::fprintf(stderr, "--blocks must be at least 1\n");
        return std::nullopt;
    }
    if (options.sample_rate < 1) {
        std::fprintf(stderr, "--sample-rate must be positive\n");
        return std::nullopt;
    }
    if (options.sim_threads < 1 || options.sim_rays < 1 || options.sim_bounces < 1 || options.sim_interval_ms < 0) {
        std::fprintf(stderr, "--sim-threads, --sim-rays and --sim-bounces must be at least 1, --sim-interval-ms not negative\n");
        return std::nullopt;
    }
    return options;
}

std::vector<Scenario> selected_scenarios(const Options& options) {
    std::vector<Scenario> selected;
    for (const Scenario& s : all_scenarios()) {
        if (options.only.empty() || options.only == s.name)
            selected.push_back(s);
    }
    return selected;
}

struct FixtureKey {
    float duration_sec = 0.0f;
    int sources = 0;
    bool load_only = false;

    bool operator<(const FixtureKey& other) const {
        return std::tie(duration_sec, sources, load_only) < std::tie(other.duration_sec, other.sources, other.load_only);
    }
};

// Owns every Steam Audio object the benchmark creates. Release order matters: sources before the
// simulator, the simulator before the scene, everything before the context.
class Phonon {
  public:
    Phonon() = default;
    Phonon(const Phonon&) = delete;
    Phonon& operator=(const Phonon&) = delete;
    Phonon(Phonon&&) = delete;
    Phonon& operator=(Phonon&&) = delete;

    ~Phonon() {
        for (auto& [key, fixture] : ir_fixtures_) {
            for (IPLSource& source : fixture.sources)
                iplSourceRelease(&source);
            iplSimulatorRelease(&fixture.simulator);
        }
        if (static_mesh_)
            iplStaticMeshRelease(&static_mesh_);
        if (scene_)
            iplSceneRelease(&scene_);
        if (context_)
            iplContextRelease(&context_);
    }

    bool init(int sample_rate, int sim_threads) {
        sample_rate_ = sample_rate;
        sim_threads_ = sim_threads;
        IPLContextSettings settings{};
        settings.version = STEAMAUDIO_VERSION;
        settings.simdLevel = IPL_SIMDLEVEL_AVX512;
        if (iplContextCreate(&settings, &context_) != IPL_STATUS_SUCCESS)
            return false;
        return create_shoebox_scene();
    }

    IPLContext context() const { return context_; }

    // Returns one convolution parameter set per source, simulated once per key.
    // IR handles stay owned by the sources.
    std::optional<std::vector<IPLReflectionEffectParams>> simulate_irs(const FixtureKey& key) {
        if (key.sources < 1 || key.sources > kMaxSources)
            return std::nullopt;
        IrFixture& fixture = ir_fixtures_[key];
        if (!fixture.simulator && !create_simulator(key, fixture))
            return std::nullopt;
        return fixture.params;
    }

    // Runs one more reflections pass on a fixture simulate_irs created. Safe on another thread
    // while effects apply the fixture's IRs, as long as no new fixture is created meanwhile.
    std::vector<IPLReflectionEffectParams> run_reflections(const FixtureKey& key, int rays, int bounces) {
        const auto it = ir_fixtures_.find(key);
        if (it == ir_fixtures_.end())
            return {};
        set_shared_inputs(it->second.simulator, key.duration_sec, std::min(rays, kNumRays), bounces);
        iplSimulatorRunReflections(it->second.simulator);
        return read_params(key, it->second);
    }

  private:
    struct IrFixture {
        IPLSimulator simulator = nullptr;
        std::vector<IPLSource> sources;
        std::vector<IPLReflectionEffectParams> params;
    };

    bool create_shoebox_scene() {
        IPLSceneSettings scene_settings{};
        scene_settings.type = IPL_SCENETYPE_DEFAULT;
        if (iplSceneCreate(context_, &scene_settings, &scene_) != IPL_STATUS_SUCCESS)
            return false;

        constexpr float w = 10.0f;
        constexpr float h = 4.0f;
        constexpr float d = 8.0f;
        IPLVector3 vertices[8] = {
            {0, 0, 0},
            {w, 0, 0},
            {w, 0, d},
            {0, 0, d},
            {0, h, 0},
            {w, h, 0},
            {w, h, d},
            {0, h, d},
        };
        IPLTriangle triangles[12] = {
            {{0, 1, 2}},
            {{0, 2, 3}},
            {{4, 6, 5}},
            {{4, 7, 6}},
            {{0, 4, 5}},
            {{0, 5, 1}},
            {{3, 2, 6}},
            {{3, 6, 7}},
            {{0, 3, 7}},
            {{0, 7, 4}},
            {{1, 5, 6}},
            {{1, 6, 2}},
        };
        IPLint32 material_indices[12] = {};
        IPLMaterial material = {{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}};

        IPLStaticMeshSettings mesh_settings{};
        mesh_settings.numVertices = 8;
        mesh_settings.numTriangles = 12;
        mesh_settings.numMaterials = 1;
        mesh_settings.vertices = vertices;
        mesh_settings.triangles = triangles;
        mesh_settings.materialIndices = material_indices;
        mesh_settings.materials = &material;
        if (iplStaticMeshCreate(scene_, &mesh_settings, &static_mesh_) != IPL_STATUS_SUCCESS)
            return false;
        iplStaticMeshAdd(static_mesh_, scene_);
        iplSceneCommit(scene_);
        return true;
    }

    static IPLCoordinateSpace3 pose_at(IPLVector3 origin) {
        IPLCoordinateSpace3 pose{};
        pose.right = {1, 0, 0};
        pose.up = {0, 1, 0};
        pose.ahead = {0, 0, -1};
        pose.origin = origin;
        return pose;
    }

    // Spreads sources on a grid inside the room, away from the walls and the listener.
    static IPLVector3 source_position(int index) {
        constexpr int kColumns = 8;
        const int column = index % kColumns;
        const int row = index / kColumns;
        return {1.0f + 1.1f * static_cast<float>(column), 1.2f + 0.4f * static_cast<float>(row % 4), 1.0f + 0.8f * static_cast<float>(row)};
    }

    bool create_simulator(const FixtureKey& key, IrFixture& fixture) {
        const float duration_sec = key.duration_sec;
        IPLSimulationSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        settings.sceneType = IPL_SCENETYPE_DEFAULT;
        settings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        settings.maxNumRays = kNumRays;
        settings.numDiffuseSamples = kNumDiffuseSamples;
        settings.maxDuration = duration_sec;
        settings.maxOrder = kAmbisonicsOrder;
        settings.maxNumSources = key.sources;
        settings.numThreads = sim_threads_;
        settings.rayBatchSize = 1;
        settings.samplingRate = sample_rate_;
        settings.frameSize = kFrameSize;
        if (iplSimulatorCreate(context_, &settings, &fixture.simulator) != IPL_STATUS_SUCCESS)
            return false;
        iplSimulatorSetScene(fixture.simulator, scene_);

        for (int i = 0; i < key.sources; i++) {
            IPLSourceSettings source_settings{};
            source_settings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            IPLSource source = nullptr;
            if (iplSourceCreate(fixture.simulator, &source_settings, &source) != IPL_STATUS_SUCCESS)
                return false;
            fixture.sources.push_back(source);
            iplSourceAdd(source, fixture.simulator);

            IPLSimulationInputs inputs{};
            inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            inputs.source = pose_at(source_position(i));
            inputs.reverbScale[0] = 1.0f;
            inputs.reverbScale[1] = 1.0f;
            inputs.reverbScale[2] = 1.0f;
            iplSourceSetInputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
        }

        set_shared_inputs(fixture.simulator, duration_sec, kNumRays, kNumBounces);
        iplSimulatorCommit(fixture.simulator);
        iplSimulatorRunReflections(fixture.simulator);
        fixture.params = read_params(key, fixture);
        return true;
    }

    static void set_shared_inputs(IPLSimulator simulator, float duration_sec, int rays, int bounces) {
        IPLSimulationSharedInputs shared{};
        shared.listener = pose_at({5.0f, 1.7f, 4.0f});
        shared.numRays = rays;
        shared.numBounces = bounces;
        shared.duration = duration_sec;
        shared.order = kAmbisonicsOrder;
        shared.irradianceMinDistance = kIrradianceMinDistance;
        iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);
    }

    std::vector<IPLReflectionEffectParams> read_params(const FixtureKey& key, const IrFixture& fixture) const {
        const int ir_size = resonance::reverb_ir_size_samples(sample_rate_, key.duration_sec);
        std::vector<IPLReflectionEffectParams> all;
        for (IPLSource source : fixture.sources) {
            IPLSimulationOutputs outputs{};
            iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
            IPLReflectionEffectParams params = outputs.reflections;
            params.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
            params.numChannels = kChannels;
            params.irSize = ir_size;
            all.push_back(params);
        }
        return all;
    }

    int sample_rate_ = kDefaultSampleRate;
    int sim_threads_ = 1;
    IPLContext context_ = nullptr;
    IPLScene scene_ = nullptr;
    IPLStaticMesh static_mesh_ = nullptr;
    std::map<FixtureKey, IrFixture> ir_fixtures_;
};

// Owns the per-scenario audio objects: one effect per voice, one shared mixer, the buffers.
class ScenarioRig {
  public:
    explicit ScenarioRig(IPLContext context) : context_(context) {}
    ScenarioRig(const ScenarioRig&) = delete;
    ScenarioRig& operator=(const ScenarioRig&) = delete;
    ScenarioRig(ScenarioRig&&) = delete;
    ScenarioRig& operator=(ScenarioRig&&) = delete;

    ~ScenarioRig() {
        for (IPLReflectionEffect& effect : effects_)
            iplReflectionEffectRelease(&effect);
        if (mixer_)
            iplReflectionMixerRelease(&mixer_);
        for (IPLAudioBuffer& buffer : inputs_)
            iplAudioBufferFree(context_, &buffer);
        if (output_.data)
            iplAudioBufferFree(context_, &output_);
        if (mixer_output_.data)
            iplAudioBufferFree(context_, &mixer_output_);
    }

    bool init(int num_effects, int ir_size, int sample_rate) {
        IPLAudioSettings audio{sample_rate, kFrameSize};
        IPLReflectionEffectSettings effect_settings{IPL_REFLECTIONEFFECTTYPE_CONVOLUTION, ir_size, kChannels};
        if (iplReflectionMixerCreate(context_, &audio, &effect_settings, &mixer_) != IPL_STATUS_SUCCESS)
            return false;
        if (iplAudioBufferAllocate(context_, kChannels, kFrameSize, &output_) != IPL_STATUS_SUCCESS)
            return false;
        if (iplAudioBufferAllocate(context_, kChannels, kFrameSize, &mixer_output_) != IPL_STATUS_SUCCESS)
            return false;

        std::mt19937 rng(kNoiseSeed);
        std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
        for (int i = 0; i < num_effects; i++) {
            IPLReflectionEffect effect = nullptr;
            if (iplReflectionEffectCreate(context_, &audio, &effect_settings, &effect) != IPL_STATUS_SUCCESS)
                return false;
            effects_.push_back(effect);
            IPLAudioBuffer input{};
            if (iplAudioBufferAllocate(context_, 1, kFrameSize, &input) != IPL_STATUS_SUCCESS)
                return false;
            inputs_.push_back(input);
            for (int f = 0; f < kFrameSize; f++)
                input.data[0][f] = noise(rng);
        }
        return true;
    }

    IPLReflectionMixer mixer() const { return mixer_; }
    IPLReflectionEffect effect(int i) const { return effects_[static_cast<size_t>(i)]; }
    IPLAudioBuffer* input(int i) { return &inputs_[static_cast<size_t>(i)]; }
    IPLAudioBuffer* output() { return &output_; }
    IPLAudioBuffer* mixer_output() { return &mixer_output_; }

  private:
    IPLContext context_ = nullptr;
    IPLReflectionMixer mixer_ = nullptr;
    std::vector<IPLReflectionEffect> effects_;
    std::vector<IPLAudioBuffer> inputs_;
    IPLAudioBuffer output_{};
    IPLAudioBuffer mixer_output_{};
};

double elapsed_us(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - start).count();
}

double mean(const std::vector<double>& values) {
    if (values.empty())
        return 0.0;
    double sum = 0.0;
    for (double v : values)
        sum += v;
    return sum / static_cast<double>(values.size());
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<double>(values.size())));
    return values[index];
}

double max_of(const std::vector<double>& values) {
    if (values.empty())
        return 0.0;
    return *std::max_element(values.begin(), values.end());
}

int count_distinct_irs(const std::vector<IPLReflectionEffectParams>& params) {
    std::set<IPLReflectionEffectIR> handles;
    for (const IPLReflectionEffectParams& p : params)
        handles.insert(p.ir);
    return static_cast<int>(handles.size());
}

bool irs_match_scenario(const Scenario& scenario, const std::vector<IPLReflectionEffectParams>& params) {
    for (const IPLReflectionEffectParams& p : params) {
        if (!p.ir)
            return false;
    }
    const int expected = scenario.shared_ir ? 1 : scenario.num_effects;
    return count_distinct_irs(params) == expected;
}

FixtureKey fixture_for(const Scenario& scenario, bool load_only) {
    return {scenario.ir_duration_sec, scenario.shared_ir ? 1 : scenario.num_effects, load_only};
}

// One parameter set per effect; a shared IR is repeated for every effect.
std::vector<IPLReflectionEffectParams> params_per_effect(const Scenario& scenario, std::vector<IPLReflectionEffectParams> irs) {
    if (scenario.shared_ir && !irs.empty())
        irs.resize(static_cast<size_t>(scenario.num_effects), irs.front());
    return irs;
}

std::optional<std::vector<IPLReflectionEffectParams>> irs_for(Phonon& phonon, const Scenario& scenario) {
    auto irs = phonon.simulate_irs(fixture_for(scenario, false));
    if (!irs)
        return std::nullopt;
    return params_per_effect(scenario, std::move(*irs));
}

// No CPU means any CPU.
bool pin_current_thread(std::optional<int> cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cpu) {
        CPU_SET(*cpu, &set);
    } else {
        for (unsigned int i = 0; i < std::max(1u, std::thread::hardware_concurrency()); i++)
            CPU_SET(i, &set);
    }
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0)
        return true;
    std::fprintf(stderr, "Could not pin a thread to CPU %d\n", cpu.value_or(-1));
    return false;
}

// Linux nice values are per thread and inherited by threads started afterwards. Raising the
// priority back needs CAP_SYS_NICE, so a non-zero --sim-nice may leave the audio thread niced.
void set_current_thread_nice(int nice) {
    if (getpriority(PRIO_PROCESS, static_cast<id_t>(gettid())) == nice)
        return;
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), nice) != 0)
        std::fprintf(stderr, "Could not set nice %d\n", nice);
}

// Runs reflections passes on a second thread at a fixed interval, like the Nexus simulation worker.
class SimulationLoad {
  public:
    SimulationLoad(Phonon& phonon, const Options& options, FixtureKey key) : phonon_(phonon), options_(options), key_(key) {}
    SimulationLoad(const SimulationLoad&) = delete;
    SimulationLoad& operator=(const SimulationLoad&) = delete;
    SimulationLoad(SimulationLoad&&) = delete;
    SimulationLoad& operator=(SimulationLoad&&) = delete;
    ~SimulationLoad() { stop(); }

    void start() {
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable())
            thread_.join();
    }

    // Params of the newest finished pass, at most once per pass. Never blocks the caller.
    std::optional<std::vector<IPLReflectionEffectParams>> take_params() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !pending_)
            return std::nullopt;
        std::optional<std::vector<IPLReflectionEffectParams>> fresh = std::move(pending_);
        pending_.reset();
        return fresh;
    }

    // Valid after stop().
    int runs() const { return runs_; }
    double run_mean_us() const { return runs_ > 0 ? total_us_ / runs_ : 0.0; }

  private:
    void loop() {
        pin_current_thread(options_.sim_cpu);
        set_current_thread_nice(options_.sim_nice);
        const auto interval = std::chrono::milliseconds(options_.sim_interval_ms);
        while (!stop_.load()) {
            const auto start = std::chrono::steady_clock::now();
            std::vector<IPLReflectionEffectParams> fresh = phonon_.run_reflections(key_, options_.sim_rays, options_.sim_bounces);
            total_us_ += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            runs_++;
            if (!key_.load_only) {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_ = std::move(fresh);
            }
            std::this_thread::sleep_until(start + interval);
        }
    }

    Phonon& phonon_;
    const Options& options_;
    FixtureKey key_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::optional<std::vector<IPLReflectionEffectParams>> pending_;
    int runs_ = 0;
    double total_us_ = 0.0;
};

std::optional<FixtureKey> load_fixture(Phonon& phonon, const Scenario& scenario, SimLoad load) {
    if (load == SimLoad::None)
        return std::nullopt;
    const FixtureKey key = fixture_for(scenario, load == SimLoad::Other);
    if (!phonon.simulate_irs(key))
        return std::nullopt;
    return key;
}

// Applies every effect once per block, as that many voices would on the audio thread, then
// applies the mixer once. Warm-up blocks are run but not recorded.
std::optional<Result> run_scenario(Phonon& phonon, const Scenario& scenario, const Options& options) {
    // Steam Audio starts its simulation threads with the simulator; they keep the CPUs of the thread
    // that created it. Pin before the fixtures exist, so pin one scenario per run.
    pin_current_thread(options.sim_cpu);
    set_current_thread_nice(options.sim_nice);
    auto irs = irs_for(phonon, scenario);
    if (!irs || !irs_match_scenario(scenario, *irs)) {
        std::fprintf(stderr, "%s: IR handles are null or not as distinct as the scenario requires\n", scenario.name.c_str());
        return std::nullopt;
    }
    std::vector<IPLReflectionEffectParams> params = *irs;
    const int ir_size = params.front().irSize;

    ScenarioRig rig(phonon.context());
    if (!rig.init(scenario.num_effects, ir_size, options.sample_rate)) {
        std::fprintf(stderr, "%s: could not create effects or buffers\n", scenario.name.c_str());
        return std::nullopt;
    }

    const std::optional<FixtureKey> load_key = load_fixture(phonon, scenario, options.sim_load);
    if (options.sim_load != SimLoad::None && !load_key) {
        std::fprintf(stderr, "%s: could not create the simulation load\n", scenario.name.c_str());
        return std::nullopt;
    }
    std::optional<SimulationLoad> load;
    if (load_key) {
        load.emplace(phonon, options, *load_key);
        load->start();
    }
    pin_current_thread(options.audio_cpu);
    set_current_thread_nice(0);

    IPLReflectionEffectParams mixer_params = params.front();
    std::vector<double> apply_us;
    std::vector<double> block_us;
    std::vector<double> mixer_us;
    apply_us.reserve(static_cast<size_t>(options.blocks) * static_cast<size_t>(scenario.num_effects));
    block_us.reserve(static_cast<size_t>(options.blocks));
    mixer_us.reserve(static_cast<size_t>(options.blocks));

    // Paced runs start one block per block period, like an audio callback, instead of back to back.
    const auto block_period = std::chrono::duration<double>(static_cast<double>(kFrameSize) / options.sample_rate);
    const auto paced_start = std::chrono::steady_clock::now();
    const int total_blocks = options.warmup_blocks + options.blocks;
    for (int block = 0; block < total_blocks; block++) {
        if (options.paced)
            std::this_thread::sleep_until(paced_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(block_period * block));
        const bool record = block >= options.warmup_blocks;
        if (load) {
            if (auto fresh = load->take_params())
                params = params_per_effect(scenario, std::move(*fresh));
        }
        double block_total = 0.0;
        for (int i = 0; i < scenario.num_effects; i++) {
            const auto start = std::chrono::steady_clock::now();
            iplReflectionEffectApply(rig.effect(i), &params[static_cast<size_t>(i)], rig.input(i), rig.output(), rig.mixer());
            const double us = elapsed_us(start, std::chrono::steady_clock::now());
            block_total += us;
            if (record)
                apply_us.push_back(us);
        }
        const auto mix_start = std::chrono::steady_clock::now();
        iplReflectionMixerApply(rig.mixer(), &mixer_params, rig.mixer_output());
        const double mix = elapsed_us(mix_start, std::chrono::steady_clock::now());
        if (record) {
            block_us.push_back(block_total);
            mixer_us.push_back(mix);
        }
    }

    if (load)
        load->stop();

    Result result;
    result.scenario = scenario;
    if (load) {
        result.sim_runs = load->runs();
        result.sim_run_mean_us = load->run_mean_us();
    }
    result.ir_size_samples = ir_size;
    result.distinct_irs = count_distinct_irs(params);
    result.apply_mean_us = mean(apply_us);
    result.apply_p95_us = percentile(apply_us, 0.95);
    result.apply_p99_us = percentile(apply_us, 0.99);
    result.apply_max_us = max_of(apply_us);
    result.block_mean_us = mean(block_us);
    result.block_max_us = max_of(block_us);
    result.mixer_mean_us = mean(mixer_us);
    return result;
}

// Time-domain size of the IRs the scenario touches. Steam Audio keeps them partitioned in the
// frequency domain internally, which is larger; this is a lower bound on the working set.
double ir_footprint_mb(const Result& r) {
    const double bytes_per_ir = static_cast<double>(r.ir_size_samples) * kChannels * sizeof(float);
    return bytes_per_ir * r.distinct_irs / kBytesPerMegabyte;
}

std::string read_first_line_with(const char* path, const char* prefix) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind(prefix, 0) != 0)
            continue;
        const size_t colon = line.find(':');
        return colon == std::string::npos ? line : line.substr(colon + 2);
    }
    return "";
}

std::string read_file_trimmed(const std::string& path) {
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return text;
}

struct CpuInfo {
    std::string model;
    std::string caches;
};

CpuInfo read_cpu_info() {
    CpuInfo info;
    info.model = read_first_line_with("/proc/cpuinfo", "model name");
    if (info.model.empty())
        info.model = "unknown";
    for (int index = 0; index < 8; index++) {
        const std::string dir = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(index) + "/";
        const std::string level = read_file_trimmed(dir + "level");
        if (level.empty())
            break;
        const std::string type = read_file_trimmed(dir + "type");
        if (type == "Instruction")
            continue;
        if (!info.caches.empty())
            info.caches += ", ";
        info.caches += "L" + level + " " + read_file_trimmed(dir + "size");
    }
    if (info.caches.empty())
        info.caches = "unknown";
    return info;
}

std::string cpu_label(std::optional<int> cpu) {
    return cpu ? std::to_string(*cpu) : "any";
}

void print_table(const CpuInfo& cpu, const std::vector<Result>& results, const Options& options) {
    std::printf("CPU: %s\nCaches (per core 0): %s\n", cpu.model.c_str(), cpu.caches.c_str());
    std::printf("Steam Audio %d.%d.%d, %d Hz, %d frames, order %d, %d blocks after %d warm-up\n\n", STEAMAUDIO_VERSION_MAJOR,
                STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, options.sample_rate, kFrameSize, kAmbisonicsOrder, options.blocks,
                options.warmup_blocks);
    std::printf("Simulation load: %s, %d thread(s), %d rays, %d bounces, every %d ms, nice %d; audio CPU %s, simulation CPU %s\n\n",
                sim_load_name(options.sim_load), options.sim_threads, options.sim_rays, options.sim_bounces, options.sim_interval_ms, options.sim_nice,
                cpu_label(options.audio_cpu).c_str(), cpu_label(options.sim_cpu).c_str());
    std::printf("%-14s %7s %6s %5s %9s %11s %10s %10s %10s %12s %11s %10s %9s %11s\n", "scenario", "effects", "IR s", "IRs", "IR MB",
                "apply mean", "apply p95", "apply p99", "apply max", "block mean", "block max", "mixer", "sim runs", "sim pass");
    for (const Result& r : results) {
        std::printf("%-14s %7d %6.1f %5d %9.1f %8.1f us %7.1f us %7.1f us %7.1f us %9.1f us %8.1f us %7.1f us %9d %8.0f us\n",
                    r.scenario.name.c_str(), r.scenario.num_effects, r.scenario.ir_duration_sec, r.distinct_irs, ir_footprint_mb(r),
                    r.apply_mean_us, r.apply_p95_us, r.apply_p99_us, r.apply_max_us, r.block_mean_us, r.block_max_us, r.mixer_mean_us,
                    r.sim_runs, r.sim_run_mean_us);
    }
}

void print_json(const CpuInfo& cpu, const std::vector<Result>& results, const Options& options) {
    std::printf("{\n  \"cpu\": \"%s\",\n  \"caches\": \"%s\",\n  \"steam_audio\": \"%d.%d.%d\",\n", cpu.model.c_str(), cpu.caches.c_str(),
                STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
    std::printf("  \"sample_rate\": %d,\n  \"frame_size\": %d,\n  \"order\": %d,\n  \"blocks\": %d,\n", options.sample_rate, kFrameSize,
                kAmbisonicsOrder, options.blocks);
    std::printf("  \"sim_load\": \"%s\",\n  \"sim_threads\": %d,\n  \"sim_interval_ms\": %d,\n  \"audio_cpu\": \"%s\",\n  \"sim_cpu\": \"%s\",\n",
                sim_load_name(options.sim_load), options.sim_threads, options.sim_interval_ms, cpu_label(options.audio_cpu).c_str(),
                cpu_label(options.sim_cpu).c_str());
    std::printf("  \"scenarios\": [\n");
    for (size_t i = 0; i < results.size(); i++) {
        const Result& r = results[i];
        std::printf("    {\"name\": \"%s\", \"effects\": %d, \"ir_duration_sec\": %.1f, \"distinct_irs\": %d, \"ir_mb\": %.2f, "
                    "\"apply_mean_us\": %.2f, \"apply_p95_us\": %.2f, \"apply_p99_us\": %.2f, \"apply_max_us\": %.2f, "
                    "\"block_mean_us\": %.2f, \"block_max_us\": %.2f, \"mixer_mean_us\": %.2f, \"sim_runs\": %d, \"sim_run_mean_us\": %.1f}%s\n",
                    r.scenario.name.c_str(), r.scenario.num_effects, r.scenario.ir_duration_sec, r.distinct_irs, ir_footprint_mb(r),
                    r.apply_mean_us, r.apply_p95_us, r.apply_p99_us, r.apply_max_us, r.block_mean_us, r.block_max_us, r.mixer_mean_us,
                    r.sim_runs, r.sim_run_mean_us,
                    i + 1 < results.size() ? "," : "");
    }
    std::printf("  ]\n}\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parse_options(argc, argv);
    if (!options)
        return 2;
    const std::vector<Scenario> scenarios = selected_scenarios(*options);
    if (scenarios.empty()) {
        std::fprintf(stderr, "No scenario named '%s'\n", options->only.c_str());
        return 2;
    }

    Phonon phonon;
    if (!phonon.init(options->sample_rate, options->sim_threads)) {
        std::fprintf(stderr, "Could not create the Steam Audio context or scene\n");
        return 1;
    }

    std::vector<Result> results;
    for (const Scenario& scenario : scenarios) {
        std::optional<Result> result = run_scenario(phonon, scenario, *options);
        if (!result)
            return 1;
        results.push_back(*result);
    }

    const CpuInfo cpu = read_cpu_info();
    if (options->json)
        print_json(cpu, results, *options);
    else
        print_table(cpu, results, *options);
    return 0;
}
