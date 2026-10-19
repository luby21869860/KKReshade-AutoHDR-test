#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "screenshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#define SCREENSHOT_SHM_NAME "KKReshade_Screenshot_SHM"
#define SCREENSHOT_CONTROL_SHM_NAME "KKReshade_Screenshot_Control_SHM"

namespace
{
    static HANDLE hMapFile = nullptr;
    static HANDLE hControlMapFile = nullptr;
    static screenshot::ScreenshotMemory* shm = nullptr;
    static reshade::api::resource staging_resource = { 0 };
    static bool* control_shm = nullptr;
    static bool last_control_state = false;
    static int screenshot_count = 0;
    static unsigned int runtime_generation = 0;
    static unsigned int frames_after_runtime_init = 0;
    static unsigned int effects_ready_streak = 0;
    static bool effects_ready_for_capture = false;
    static unsigned int active_technique_count = 0;
    static unsigned int enabled_technique_count = 0;
    static uint64_t last_effect_signature = 0;
    static bool have_good_frame = false;

    struct effect_state_probe
    {
        unsigned int technique_count = 0;
        unsigned int enabled_count = 0;
        unsigned int disabled_count = 0;
        uint64_t signature = 1469598103934665603ull;
    };

    static void signature_add(uint64_t* hash, const char* text)
    {
        if (text == nullptr)
            return;
        while (*text != '\0')
        {
            *hash ^= static_cast<unsigned char>(*text++);
            *hash *= 1099511628211ull;
        }
        *hash ^= 0xFFu;
        *hash *= 1099511628211ull;
    }

    static void inspect_technique(reshade::api::effect_runtime* runtime, reshade::api::effect_technique technique, void* user_data)
    {
        auto* state = static_cast<effect_state_probe*>(user_data);
        ++state->technique_count;
        char technique_name[256] = {};
        char effect_name[256] = {};
        runtime->get_technique_name(technique, technique_name);
        runtime->get_technique_effect_name(technique, effect_name);
        const bool enabled = runtime->get_technique_state(technique);
        if (enabled)
            ++state->enabled_count;
        else
            ++state->disabled_count;
        signature_add(&state->signature, effect_name);
        signature_add(&state->signature, technique_name);
        signature_add(&state->signature, enabled ? "enabled" : "disabled");
    }

    // This checks ReShade's current global effects state and the set of techniques
    // exposed by the runtime. It does NOT prove that third-party add-ons or external
    // neural-rendering stages have completed successfully; the stable-frame gate is
    // a conservative heuristic, not a render-completion guarantee.
    static bool effects_ready(reshade::api::effect_runtime* runtime, effect_state_probe* result)
    {
        if (!runtime->get_effects_state())
            return false;

        runtime->enumerate_techniques(nullptr, inspect_technique, result);

        // An empty technique list immediately after runtime creation is treated as
        // not ready. A scene with no ReShade techniques cannot be verified as rendered.
        return result->technique_count > 0;
    }

}

namespace screenshot
{
    namespace
    {
        static float half_to_float(uint16_t h)
        {
            const uint32_t sign = (h & 0x8000u) << 16;
