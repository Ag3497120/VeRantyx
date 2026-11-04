// Exercise the actual Windows bridge and vtable layouts without SteamVR.
#include <assert.h>
#include "../src/openvr_capture_proxy.cpp"

static unsigned sets = 0, clears = 0;
static const char* lastModel;
static const vr::HmdMatrix34_t* lastTransform;
static const vr::Compositor_StageRenderSettings* lastSettings;
static uint32_t lastSize;
static vr::EVRCompositorError answer = vr::VRCompositorError_None;
static vr::EVRCompositorError VR_CALLTYPE Set(void*, const char* model, const vr::HmdMatrix34_t* transform,
    const vr::Compositor_StageRenderSettings* settings, uint32_t size) {
    ++sets; lastModel = model; lastTransform = transform; lastSettings = settings; lastSize = size;
    return answer;
}
static void VR_CALLTYPE Clear(void*) { ++clears; }
static void** Table(size_t stage) {
    auto table = (void**)VirtualAlloc(nullptr, 64 * sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(table); table[stage] = (void*)&Set; table[stage + 1] = (void*)&Clear;
    return table;
}
static void Read(char* data, size_t capacity) {
    FILE* file = fopen(kStageFile, "rb"); assert(file);
    const size_t count = fread(data, 1, capacity - 1, file);
    assert(feof(file) && !ferror(file)); data[count] = 0; fclose(file);
    assert(GetFileAttributesA(kStageTemporary) == INVALID_FILE_ATTRIBUTES);
}

int main() {
    SetEnvironmentVariableA("VERANTYX_LOADING_MAP", "1");
    DeleteFileA(kStageFile);
    char json[2048], previous[2048], relative[128];
    assert(StageModel("Z:\\a path\\hlvr\\models\\ui\\dioramas\\a2_pistol.obj", relative, sizeof(relative)));
    assert(strcmp(relative, "models/ui/dioramas/a2_pistol.obj") == 0);
    for (const char* bad : {"models/ui/dioramas/../outside.obj", "models/ui/dioramas/sub/a.obj",
                           "other/models/ui/dioramas-not/a.obj", "models/ui/dioramas/a.json", "startup.obj"})
        assert(!StageModel(bad, relative, sizeof(relative)));
    for (unsigned version : {26u, 29u}) {
        const size_t stage = version == 26 ? 46 : 48;
        auto table = Table(stage);
        char name[32]; snprintf(name, sizeof(name), "IVRCompositor_%03u", version);
        assert(InstallSubmitHook(&table, name));
        assert(table[stage] == (void*)&HookedSetStage && table[stage + 1] == (void*)&HookedClearStage);
        auto set = (SetStageFn)table[stage]; auto clear = (ClearStageFn)table[stage + 1];
        const char* model = "Z:\\a path\\game\\hlvr\\models\\ui\\dioramas\\a2_quarantine_entrance.obj";
        vr::HmdMatrix34_t transform = {}; transform.m[0][0] = transform.m[1][1] = transform.m[2][2] = 1;
        transform.m[1][3] = 1.25f;
        auto settings = vr::DefaultStageRenderSettings();
        settings.m_PrimaryColor.r = 0.25f; settings.m_bGreyscale = true; settings.m_bBackfaceCulling = true;
        assert(set(&table, model, &transform, &settings, sizeof(settings)) == vr::VRCompositorError_None);
        assert(lastModel == model && lastTransform == &transform && lastSettings == &settings && lastSize == sizeof(settings));
        Read(json, sizeof(json));
        assert(strstr(json, "\"model\":\"models/ui/dioramas/a2_quarantine_entrance.obj\""));
        assert(strstr(json, "\"transform\":[1,0,0,0,0,1,0,1.25,0,0,1,0]"));
        assert(strstr(json, "\"settings\":[0.25,1,1,1,1,1,1,1,0,0,0]"));
        assert(strstr(json, "\"cull\":1,\"greyscale\":1,\"wireframe\":0"));
        strcpy(previous, json);
        answer = vr::VRCompositorError_RequestFailed;
        assert(set(&table, "bad", nullptr, nullptr, 0) == answer);
        Read(json, sizeof(json)); assert(strcmp(json, previous) == 0);
        answer = vr::VRCompositorError_None;
        // A second request atomically replaces a real file and supplies defaults.
        assert(set(&table, model, nullptr, nullptr, 0) == answer);
        Read(json, sizeof(json)); assert(strcmp(json, previous) != 0);
        assert(strstr(json, "\"transform\":[1,0,0,0,0,1,0,0,0,0,1,0]"));
        transform.m[0][0] = NAN;
        assert(set(&table, model, &transform, &settings, sizeof(settings)) == answer);
        assert(GetFileAttributesA(kStageFile) == INVALID_FILE_ATTRIBUTES);
        assert(set(&table, model, nullptr, &settings, 1) == answer);
        assert(GetFileAttributesA(kStageFile) == INVALID_FILE_ATTRIBUTES);
        assert(set(&table, model, nullptr, nullptr, 0) == answer);
        clear(&table); assert(GetFileAttributesA(kStageFile) == INVALID_FILE_ATTRIBUTES);
    }
    assert(sets == 12 && clears == 2);
    auto unknown = Table(48);
    assert(InstallSubmitHook(&unknown, "IVRCompositor_030"));
    assert(unknown[48] == (void*)&Set && unknown[49] == (void*)&Clear);
    SetEnvironmentVariableA("VERANTYX_LOADING_MAP", "0");
    auto disabled = Table(48);
    assert(InstallSubmitHook(&disabled, "IVRCompositor_029"));
    assert(disabled[48] == (void*)&Set && disabled[49] == (void*)&Clear);
    puts("Loading stage bridge: 026/029 forwarding, defaults, replacement, validation, clear and opt-in passed");
    return 0;
}
