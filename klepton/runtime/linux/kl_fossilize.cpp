// kl_fossilize.cpp — Alyx's Vulkan pipelines, recorded and replayed with Valve's Fossilize.
//
// On the headset a pipeline the Metal compiler has not seen costs 330-580 ms, and Source 2
// creates them one at a time as new views appear: a walk into a new area froze the frame for
// up to 9 s. The OS Metal cache keeps them across launches (2-8 ms when warm), so the fix is
// to have compiled everything BEFORE the guest asks:
//
//   KL_VK_FOZ_RECORD=<file.foz>|auto   record every sampler, set/pipeline layout, shader
//       module, render pass and pipeline the guest creates (its own create infos), appending
//       to a Fossilize stream archive ("auto": $HOME/Documents/klfz/recorded.foz).
//   KL_VK_FOZ_REPLAY=<file.foz|dir>[:...]   at vkCreateDevice, replay those databases on
//       background threads (KL_VK_FOZ_THREADS, default 4) through the layer's own entry
//       points, so each pipeline goes through the same fix-ups and MoltenVK state as the
//       guest's and produces the same MSL, which is what the Metal cache is keyed on. Each
//       replayed pipeline is destroyed at once; only the warm cache is kept.
//
// The shipped hlvr/vulkan_shader_cache.foz cannot serve: its SPIR-V predates this build's
// shaders (generator 0x80008 against the game's 0x8000B; 0 of 184 modules match).
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include <vulkan/vulkan.h>
#include "../gfx/kl_vk_pipe_hooks.h"
#include "rapidjson/document.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

using namespace Fossilize;

namespace {

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

std::string default_dir() {
    const char *d = getenv("KL_VK_FOZ_DIR");
    if (d && *d) return d;
    const char *h = getenv("HOME");
    return std::string(h && *h ? h : ".") + "/Documents/klfz";
}

// ---- recording ---------------------------------------------------------------------------
StateRecorder *g_rec;
DatabaseInterface *g_rec_db;
std::atomic<unsigned> g_rec_pipes{0}, g_rec_fail{0};
// slow.txt beside the recording (h_pipeline_time) and the time recording began
std::string g_slow_path;
std::mutex g_slow_lock;
double g_rec_t0;
// Replay progress for the app's loading screen (kl_foz_progress): pipelines listed in the databases so far, pipelines
// compiled (or failed) so far, and the state: 0 no replay, 1 replaying, 2 done.
std::atomic<unsigned> g_prog_total{0}, g_prog_done{0};
std::atomic<int> g_prog_state{0};
// Pipelines replayed and those over 100 ms, over every database of a replay (prewarm.log)
std::atomic<unsigned> g_sum_compiled{0}, g_sum_slow{0};
std::atomic<double> g_prog_t0{0};

void rec_init() {
    const char *e = getenv("KL_VK_FOZ_RECORD");
    if (!e || !*e || !strcmp(e, "0")) return;
    std::string path = e;
    if (path == "1" || path == "auto") {
        path = default_dir();
        mkdir(path.c_str(), 0755);
        path += "/recorded.foz";
    }
    g_rec_db = create_stream_archive_database(path.c_str(), DatabaseMode::Append);
    if (!g_rec_db || !g_rec_db->prepare()) {
        fprintf(stderr, "  [foz] cannot open %s for recording; recording off\n", path.c_str());
        delete g_rec_db;
        g_rec_db = nullptr;
        return;
    }
    g_slow_path = path.substr(0, path.find_last_of('/') + 1) + "slow.txt";
    g_rec_t0 = now_s();
    g_rec = new StateRecorder;
    g_rec->set_database_enable_compression(true);
    g_rec->set_database_enable_checksum(true);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "hlvr", 1, "Source2", 1, VK_API_VERSION_1_1 };
    if (!g_rec->record_application_info(app)) fprintf(stderr, "  [foz] application info not recorded\n");
    // Synchronized by default: each record is written and flushed before the create returns, so a
    // crash loses nothing. With the recording thread, the 4 server crashes of the Chapter 1 crawl
    // could drop what was still queued, and a cold run after replaying that database still compiled
    // 8 pipelines for over 100 ms. KL_VK_FOZ_RECORD_SYNC=0 goes back to the thread.
    const char *sy = getenv("KL_VK_FOZ_RECORD_SYNC");
    bool sync = !(sy && !strcmp(sy, "0"));
    if (sync) g_rec->init_recording_synchronized(g_rec_db);
    else g_rec->init_recording_thread(g_rec_db);
    fprintf(stderr, "  [foz] recording the guest's pipelines to %s (%s)\n", path.c_str(),
            sync ? "synchronized: flushed per record" : "recording thread");
}

void note_pipe(bool ok) {
    unsigned n = ok ? ++g_rec_pipes : g_rec_pipes.load();
    if (!ok) ++g_rec_fail;
    if (ok && (n & (n - 1)) == 0 && n >= 64)
        fprintf(stderr, "  [foz] %u pipelines recorded (%u refused)\n", n, g_rec_fail.load());
}

void h_sampler(VkSampler h, const VkSamplerCreateInfo *ci) { if (g_rec) (void)g_rec->record_sampler(h, *ci); }
void h_set_layout(VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo *ci) { if (g_rec) (void)g_rec->record_descriptor_set_layout(h, *ci); }
void h_pipeline_layout(VkPipelineLayout h, const VkPipelineLayoutCreateInfo *ci) { if (g_rec) (void)g_rec->record_pipeline_layout(h, *ci); }
void h_shader_module(VkShaderModule h, const VkShaderModuleCreateInfo *ci) { if (g_rec) (void)g_rec->record_shader_module(h, *ci); }
void h_render_pass(VkRenderPass h, const VkRenderPassCreateInfo *ci) { if (g_rec) (void)g_rec->record_render_pass(h, *ci); }
void h_render_pass2(VkRenderPass h, const VkRenderPassCreateInfo2 *ci) { if (g_rec) (void)g_rec->record_render_pass2(h, *ci); }
void h_graphics_pipeline(VkPipeline h, const VkGraphicsPipelineCreateInfo *ci) {
    if (g_rec) note_pipe(g_rec->record_graphics_pipeline(h, *ci, nullptr, 0));
}
void h_compute_pipeline(VkPipeline h, const VkComputePipelineCreateInfo *ci) {
    if (g_rec) note_pipe(g_rec->record_compute_pipeline(h, *ci, nullptr, 0));
}
// The guest's slow compiles (KL_VK_FOZ_SLOW_MS, default 20 ms; 0 = off), one line each in slow.txt beside the
// recording: "<pipeline hash> <ms> <seconds since start>". A warm Metal cache answers in a few ms, so these are the
// pipelines a session actually stalled on, which the pipeline list should hold.
void h_pipeline_time(VkPipeline h, double ms) {
    static double thr = -1;
    if (thr < 0) { const char *e = getenv("KL_VK_FOZ_SLOW_MS"); thr = e ? atof(e) : 20.0; }
    if (!g_rec || thr <= 0 || ms < thr || g_slow_path.empty()) return;
    Hash hash = 0;
    if (!g_rec->get_hash_for_graphics_pipeline_handle(h, &hash)) return;
    std::lock_guard<std::mutex> lk(g_slow_lock);
    if (FILE *f = fopen(g_slow_path.c_str(), "a")) {
        fprintf(f, "%016llx %.1f %.1f\n", (unsigned long long)hash, ms, now_s() - g_rec_t0);
        fclose(f);
    }
}

// ---- replay ------------------------------------------------------------------------------
struct Fns {
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
};

struct Job { bool graphics; const void *ci; };

// Creates the dependency objects inline (the parse thread) and queues the pipelines for the
// compile threads. Fossilize keeps every parsed create info alive while the StateReplayer lives.
struct Creator : StateCreatorInterface {
    VkDevice dev;
    const Fns &f;
    std::vector<VkSampler> samplers;
    std::vector<VkDescriptorSetLayout> set_layouts;
    std::vector<VkPipelineLayout> layouts;
    std::vector<VkShaderModule> modules;
    std::vector<VkRenderPass> passes;
    std::mutex mu;
    std::condition_variable cv, done_cv;
    std::deque<Job> queue;
    bool closed = false;
    unsigned queued = 0, compiled = 0, failed = 0, slow = 0;
    double compile_s = 0;

    Creator(VkDevice d, const Fns &fns) : dev(d), f(fns) {}
    unsigned dep_fails = 0;
    void dep_fail(const char *what, VkResult r) {
        if (dep_fails++ < 4) fprintf(stderr, "  [foz] replay: creating a %s failed (VkResult %d)\n", what, (int)r);
    }

    bool enqueue_create_sampler(Hash, const VkSamplerCreateInfo *ci, VkSampler *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateSampler(dev, ci, nullptr, out)) { dep_fail("sampler", r); return false; }
        samplers.push_back(*out);
        return true;
    }
    bool enqueue_create_descriptor_set_layout(Hash, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayout *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateDescriptorSetLayout(dev, ci, nullptr, out)) { dep_fail("set_layout", r); return false; }
        set_layouts.push_back(*out);
        return true;
    }
    bool enqueue_create_pipeline_layout(Hash, const VkPipelineLayoutCreateInfo *ci, VkPipelineLayout *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreatePipelineLayout(dev, ci, nullptr, out)) { dep_fail("layout", r); return false; }
        layouts.push_back(*out);
        return true;
    }
    bool enqueue_create_shader_module(Hash, const VkShaderModuleCreateInfo *ci, VkShaderModule *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateShaderModule(dev, ci, nullptr, out)) { dep_fail("module", r); return false; }
        modules.push_back(*out);
        return true;
    }
    bool enqueue_create_render_pass(Hash, const VkRenderPassCreateInfo *ci, VkRenderPass *out) override {
        *out = VK_NULL_HANDLE;
        if (VkResult r = f.CreateRenderPass(dev, ci, nullptr, out)) { dep_fail("pass", r); return false; }
        passes.push_back(*out);
        return true;
    }
    bool enqueue_create_render_pass2(Hash, const VkRenderPassCreateInfo2 *ci, VkRenderPass *out) override {
        *out = VK_NULL_HANDLE;
        if (!f.CreateRenderPass2 || f.CreateRenderPass2(dev, ci, nullptr, out) != VK_SUCCESS) return false;
        passes.push_back(*out);
        return true;
    }
    void push(bool graphics, const void *ci) {
        std::lock_guard<std::mutex> l(mu);
        queue.push_back({ graphics, ci });
        queued++;
        cv.notify_one();
    }
    bool enqueue_create_graphics_pipeline(Hash, const VkGraphicsPipelineCreateInfo *ci, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        push(true, ci);
        return true;
    }
    bool enqueue_create_compute_pipeline(Hash, const VkComputePipelineCreateInfo *ci, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        push(false, ci);
        return true;
    }
    bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *out) override {
        *out = VK_NULL_HANDLE;
        return true;
    }

    void worker() {
        klvk_set_replay_thread(1);
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(mu);
                cv.wait(l, [&] { return closed || !queue.empty(); });
                if (queue.empty()) return;
                j = queue.front();
                queue.pop_front();
            }
            double t0 = now_s();
            VkPipeline p = VK_NULL_HANDLE;
            VkResult r = j.graphics
                ? f.CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, (const VkGraphicsPipelineCreateInfo *)j.ci, nullptr, &p)
                : f.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, (const VkComputePipelineCreateInfo *)j.ci, nullptr, &p);
            double dt = now_s() - t0;
            if (p) f.DestroyPipeline(dev, p, nullptr);
            std::lock_guard<std::mutex> l(mu);
            compiled++;
            g_prog_done.fetch_add(1, std::memory_order_relaxed);
            compile_s += dt;
            if (dt > 0.1) slow++;
            if (r != VK_SUCCESS) failed++;
            done_cv.notify_all();
        }
    }
    void destroy_objects() {
        for (auto h : modules) f.DestroyShaderModule(dev, h, nullptr);
        for (auto h : layouts) f.DestroyPipelineLayout(dev, h, nullptr);
        for (auto h : set_layouts) f.DestroyDescriptorSetLayout(dev, h, nullptr);
        for (auto h : passes) f.DestroyRenderPass(dev, h, nullptr);
        for (auto h : samplers) f.DestroySampler(dev, h, nullptr);
    }
};

std::vector<std::string> replay_files() {
    std::vector<std::string> out;
    const char *e = getenv("KL_VK_FOZ_REPLAY");
    if (!e || !*e || !strcmp(e, "0")) return out;
    std::string list = (!strcmp(e, "1") || !strcmp(e, "auto")) ? default_dir() : e;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t c = list.find(':', pos);
        std::string item = list.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        pos = c == std::string::npos ? list.size() + 1 : c + 1;
        if (item.empty()) continue;
        struct stat st;
        if (stat(item.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (DIR *d = opendir(item.c_str())) {
                while (struct dirent *de = readdir(d)) {
                    size_t n = strlen(de->d_name);
                    if (n > 4 && !strcmp(de->d_name + n - 4, ".foz")) out.push_back(item + "/" + de->d_name);
                }
                closedir(d);
            }
        } else {
            out.push_back(item);
        }
    }
    return out;
}

void replay_db(VkDevice dev, const Fns &f, const std::string &path, unsigned nthreads) {
    DatabaseInterface *db = create_stream_archive_database(path.c_str(), DatabaseMode::ReadOnly);
    if (!db || !db->prepare()) {
        fprintf(stderr, "  [foz] replay: cannot open %s\n", path.c_str());
        delete db;
        return;
    }
    double t0 = now_s();
    StateReplayer replayer;
    Creator creator(dev, f);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < nthreads; i++) workers.emplace_back([&] {
        pthread_setname_np("klfz replay");
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        creator.worker();
    });
    unsigned parse_fail = 0, entries = 0;
    std::vector<uint8_t> buf;
    // Dependencies first: the replayer resolves pipeline layouts and render passes only from
    // objects already replayed (shader modules and samplers it can also pull from the database).
    for (ResourceTag tag : { RESOURCE_SAMPLER, RESOURCE_DESCRIPTOR_SET_LAYOUT, RESOURCE_PIPELINE_LAYOUT,
                             RESOURCE_RENDER_PASS, RESOURCE_SHADER_MODULE,
                             RESOURCE_GRAPHICS_PIPELINE, RESOURCE_COMPUTE_PIPELINE }) {
        size_t n = 0;
        if (!db->get_hash_list_for_resource_tag(tag, &n, nullptr)) continue;
        std::vector<Hash> hashes(n);
        if (!db->get_hash_list_for_resource_tag(tag, &n, hashes.data())) continue;
        if (tag == RESOURCE_GRAPHICS_PIPELINE || tag == RESOURCE_COMPUTE_PIPELINE)
            g_prog_total.fetch_add((unsigned)n, std::memory_order_relaxed);
        for (Hash h : hashes) {
            size_t sz = 0;
            if (!db->read_entry(tag, h, &sz, nullptr, PAYLOAD_READ_NO_FLAGS)) { parse_fail++; continue; }
            buf.resize(sz);
            if (!db->read_entry(tag, h, &sz, buf.data(), PAYLOAD_READ_NO_FLAGS)) { parse_fail++; continue; }
            entries++;
            if (!replayer.parse(creator, db, buf.data(), sz)) parse_fail++;
        }
    }
    {
        std::lock_guard<std::mutex> l(creator.mu);
        creator.closed = true;
        creator.cv.notify_all();
    }
    for (auto &t : workers) t.join();
    creator.destroy_objects();
    delete db;
    fprintf(stderr, "  [foz] replay %s: %u pipelines (%u failed, %u unparsed) in %.1f s on %u threads; "
                    "%.1f s of compiles, %u over 100 ms\n",
            path.c_str(), creator.compiled, creator.failed, parse_fail, now_s() - t0, nthreads,
            creator.compile_s, creator.slow);
    g_sum_compiled.fetch_add(creator.compiled);
    g_sum_slow.fetch_add(creator.slow);
    (void)entries;
}

// Set while kl_foz_prewarm makes its own device: that device's replay is the prewarm's to run, not the hook's.
std::atomic<bool> g_prewarming{false};

void start_replay(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, bool block);

void h_device_created(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (g_prewarming.load()) return;
    // KL_VK_FOZ_REPLAY_BLOCK=1 holds vkCreateDevice until the replay is done: a cold cache then
    // costs load time before the first frame instead of racing the guest into its first views.
    const char *b = getenv("KL_VK_FOZ_REPLAY_BLOCK");
    start_replay(dev, gdpa, b && *b && strcmp(b, "0"));
}

void start_replay(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, bool block) {
    std::vector<std::string> files = replay_files();
    if (files.empty()) return;
    static Fns f;
#define L(n) f.n = (PFN_vk##n)gdpa(dev, "vk" #n)
    L(CreateSampler); L(DestroySampler); L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
    L(CreatePipelineLayout); L(DestroyPipelineLayout); L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateRenderPass); L(CreateRenderPass2); L(DestroyRenderPass);
    L(CreateGraphicsPipelines); L(CreateComputePipelines); L(DestroyPipeline);
#undef L
    unsigned nthreads = 4;
    if (const char *t = getenv("KL_VK_FOZ_THREADS")) nthreads = (unsigned)atoi(t);
    if (nthreads < 1) nthreads = 1;
    fprintf(stderr, "  [foz] replaying %zu database(s) on %u threads to warm the Metal cache%s\n",
            files.size(), nthreads, block ? " (holding device creation until done)" : " in the background");
    auto run = [dev, files, nthreads, block] {
        pthread_setname_np("klfz replay main");
        klvk_set_replay_thread(1);
        // Behind the guest's own loading: the utility class, not the render path's.
        if (!block) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        double t0 = now_s();
        g_prog_t0.store(t0); g_prog_state.store(1);
        for (const auto &p : files) replay_db(dev, f, p, nthreads);
        g_prog_state.store(2);
        fprintf(stderr, "  [foz] replay done in %.1f s\n", now_s() - t0);
        klvk_set_replay_thread(0);
    };
    if (block) run();
    else std::thread(run).detach();
}

// The guest's device features, in the recording's application info (so a report says which device the pipelines were
// made for). Before the device exists, so before anything is recorded against the application info.
void h_device_info(const VkDeviceCreateInfo *ci) {
    if (!g_rec) return;
    bool ok = ci->pEnabledFeatures ? g_rec->record_physical_device_features(*ci->pEnabledFeatures)
                                   : g_rec->record_physical_device_features(ci->pNext);
    if (!ok) fprintf(stderr, "  [foz] device features not recorded\n");
}

const klvk_pipe_hooks g_hooks = {
    h_device_created, h_sampler, h_set_layout, h_pipeline_layout, h_shader_module,
    h_render_pass, h_render_pass2, h_graphics_pipeline, h_compute_pipeline, h_pipeline_time, h_device_info,
};

} // namespace

// The pipeline replay's progress, for the app's loading screen: returns 0 (no replay this run), 1 (replaying) or 2
// (done); `done`/`total` are pipelines compiled and listed so far (the total grows as each database is opened), and
// `elapsed` the seconds since the replay began.
extern "C" int kl_foz_progress(unsigned *done, unsigned *total, double *elapsed) {
    int st = g_prog_state.load();
    if (done) *done = g_prog_done.load();
    if (total) *total = g_prog_total.load();
    if (elapsed) *elapsed = st ? now_s() - g_prog_t0.load() : 0;
    return st;
}

extern "C" void kl_fossilize_install(void) {
    static bool done;
    if (done) return;
    done = true;
    const char *r = getenv("KL_VK_FOZ_RECORD"), *p = getenv("KL_VK_FOZ_REPLAY");
    bool rec = r && *r && strcmp(r, "0"), rep = p && *p && strcmp(p, "0");
    if (!rec && !rep) return;
    if (rec) rec_init();
    klvk_set_pipe_hooks(&g_hooks);
}

extern "C" void *kl_vulkan_sym(const char *name);

// One line per "Preparing graphics" before Play in prewarm.log beside the recording, which a stutter report carries
// (diagnostics/): "<unix time it began> <seconds> <pipelines> <over 100 ms> <result>" (result 0, or why it could not:
// -1 the device, -2 no usable device info). The launcher's own log line comes before the game's log file exists.
extern "C" void kl_foz_prewarm_log(double seconds, unsigned pipelines, unsigned slow, int rc) {
    std::string path = default_dir() + "/prewarm.log";
    if (FILE *f = fopen(path.c_str(), "a")) {
        fprintf(f, "%ld %.1f %u %u %d\n", (long)time(NULL) - (long)seconds, seconds, pipelines, slow, rc);
        fclose(f);
    }
}

// Warm the Metal cache before the game starts, with no guest at all: make an instance and a device exactly as the game
// made its own (device-info.json, which the layer writes at the game's vkCreateDevice: KL_VK_DEVICE_INFO), through the
// layer's own entry points, and replay the databases on it (KL_VK_FOZ_REPLAY), blocking. Pipelines made on that
// device go through the same fix-ups and MoltenVK state as the game's, so the game's own replay then finds each one
// in the cache. Progress is kl_foz_progress's. Returns 0 when it replayed; -2 when the device info is missing or not
// usable (a feature struct this build cannot rebuild: try another copy), -1 when it could not otherwise (no databases,
// a create that failed). The game's own replay is the fallback. The device and instance are retired after the replay
// (klvk_retire_device), so the game's own device is the layer's first.
extern "C" int kl_foz_prewarm(const char *info_path) {
    kl_fossilize_install();
    if (replay_files().empty()) return -1;
    if (!info_path) return -2;
    std::ifstream in(info_path);
    if (!in) { fprintf(stderr, "  [foz] prewarm: no device info at %s\n", info_path); return -2; }
    std::stringstream ss; ss << in.rdbuf();
    std::string text = ss.str();
    rapidjson::Document d;
    d.Parse(text.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("instance") || !d.HasMember("device")) {
        fprintf(stderr, "  [foz] prewarm: %s is not device info\n", info_path);
        return -2;
    }
    auto hex = [](const char *h, std::vector<uint8_t> &out) {
        size_t n = strlen(h) / 2;
        out.resize(n);
        for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    };
    auto gipa = (PFN_vkGetInstanceProcAddr)kl_vulkan_sym("vkGetInstanceProcAddr");
    if (!gipa) return -1;
    const auto &ji = d["instance"], &jd = d["device"];

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = ji["app"].GetString(); app.applicationVersion = ji["app_version"].GetUint();
    app.pEngineName = ji["engine"].GetString(); app.engineVersion = ji["engine_version"].GetUint();
    app.apiVersion = ji["api"].GetUint();
    std::vector<const char *> iext;
    for (const auto &e : ji["extensions"].GetArray()) iext.push_back(e.GetString());
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)iext.size(); ici.ppEnabledExtensionNames = iext.data();

    std::vector<const char *> dext;
    for (const auto &e : jd["extensions"].GetArray()) dext.push_back(e.GetString());
    std::vector<VkDeviceQueueCreateInfo> qs;
    static const float prio[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    for (const auto &q : jd["queues"].GetArray()) {
        VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = q["family"].GetUint();
        qi.queueCount = std::min(q["count"].GetUint(), 16u);
        qi.flags = q["flags"].GetUint();
        qi.pQueuePriorities = prio;
        qs.push_back(qi);
    }
    VkPhysicalDeviceFeatures feats = {};
    bool have_feats = jd["enabled_features"].IsString();
    if (have_feats) {
        std::vector<uint8_t> b; hex(jd["enabled_features"].GetString(), b);
        if (b.size() != sizeof feats) { fprintf(stderr, "  [foz] prewarm: enabled_features size %zu\n", b.size()); return -2; }
        memcpy(&feats, b.data(), sizeof feats);
    }
    std::vector<std::vector<uint8_t>> chain;
    for (const auto &st : jd["pnext"].GetArray()) {
        size_t size = st["size"].GetUint();
        if (size <= sizeof(VkBaseOutStructure)) {
            fprintf(stderr, "  [foz] prewarm: device info has a feature struct (sType %u) this build cannot rebuild\n",
                    st["sType"].GetUint());
            return -2;
        }
        std::vector<uint8_t> b(size, 0), data;
        hex(st["data"].GetString(), data);
        if (data.size() != size - sizeof(VkBaseOutStructure)) return -2;
        ((VkBaseOutStructure *)b.data())->sType = (VkStructureType)st["sType"].GetUint();
        memcpy(b.data() + sizeof(VkBaseOutStructure), data.data(), data.size());
        chain.push_back(std::move(b));
    }
    for (size_t i = 0; i + 1 < chain.size(); i++)
        ((VkBaseOutStructure *)chain[i].data())->pNext = (VkBaseOutStructure *)chain[i + 1].data();
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = chain.empty() ? nullptr : chain[0].data();
    dci.queueCreateInfoCount = (uint32_t)qs.size(); dci.pQueueCreateInfos = qs.data();
    dci.enabledExtensionCount = (uint32_t)dext.size(); dci.ppEnabledExtensionNames = dext.data();
    dci.pEnabledFeatures = have_feats ? &feats : nullptr;

    // the layer would write this device's settings over the game's
    std::string keep_info = getenv("KL_VK_DEVICE_INFO") ? getenv("KL_VK_DEVICE_INFO") : "";
    unsetenv("KL_VK_DEVICE_INFO");
    double t0 = now_s();
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = ((PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance"))(&ici, nullptr, &inst);
    if (r == VK_SUCCESS) {
        uint32_t n = 1;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &phys);
        g_prewarming.store(true);
        r = phys ? ((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(phys, &dci, nullptr, &dev) : VK_ERROR_INITIALIZATION_FAILED;
        g_prewarming.store(false);
    }
    if (!keep_info.empty()) setenv("KL_VK_DEVICE_INFO", keep_info.c_str(), 1);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "  [foz] prewarm: could not make the device (%d)\n", (int)r);
        kl_foz_prewarm_log(now_s() - t0, 0, 0, -1);
        return -1;
    }
    fprintf(stderr, "  [foz] prewarm: device made as the game makes it (%zu + %zu extensions, %zu feature structs); replaying\n",
            iext.size(), dext.size(), chain.size());
    g_sum_compiled.store(0); g_sum_slow.store(0);
    start_replay(dev, (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr"), true);
    klvk_retire_device(inst, dev);
    fprintf(stderr, "  [foz] prewarm done in %.1f s\n", now_s() - t0);
    kl_foz_prewarm_log(now_s() - t0, g_sum_compiled.load(), g_sum_slow.load(), 0);
    return 0;
}

