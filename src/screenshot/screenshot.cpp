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
            const uint32_t exponent = (h >> 10) & 0x1Fu;
            const uint32_t mantissa = h & 0x03FFu;

            uint32_t bits = sign;
            if (exponent == 0)
            {
                if (mantissa != 0)
                {
                    // Normalize subnormal half values.
                    uint32_t m = mantissa;
                    int e = -1;
                    do
                    {
                        ++e;
                        m <<= 1;
                    } while ((m & 0x0400u) == 0);
                    m &= 0x03FFu;
                    const uint32_t exp32 = static_cast<uint32_t>(127 - 14 - e);
                    bits |= exp32 << 23;
                    bits |= m << 13;
                }
            }
            else if (exponent == 0x1Fu)
            {
                bits |= 0x7F800000u | (mantissa << 13);
            }
            else
            {
                bits |= (exponent + (127 - 15)) << 23;
                bits |= mantissa << 13;
            }

            float result;
            memcpy(&result, &bits, sizeof(result));
            return result;
        }

        static uint8_t linear_to_srgb8(float value)
        {
            // DLSS 5/AutoHDR uses an scRGB-style FP16 back buffer. The VideoExport
            // shared-memory contract is fixed at 8-bit RGBA, so convert the linear
            // scRGB value to display-referred sRGB and clamp to the 8-bit range.
            value = std::clamp(value, 0.0f, 1.0f);
            const float srgb = value <= 0.0031308f
                ? value * 12.92f
                : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        static void convert_fp16_rgba_to_rgba8(const uint16_t *source, uint8_t *destination, uint32_t width, uint32_t height)
        {
            const size_t pixel_count = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < pixel_count; ++i)
            {
                destination[i * 4 + 0] = linear_to_srgb8(half_to_float(source[i * 4 + 0]));
                destination[i * 4 + 1] = linear_to_srgb8(half_to_float(source[i * 4 + 1]));
                destination[i * 4 + 2] = linear_to_srgb8(half_to_float(source[i * 4 + 2]));
                destination[i * 4 + 3] = static_cast<uint8_t>(std::clamp(half_to_float(source[i * 4 + 3]) * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }
    }

    void on_reshade_finish_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view)
    {
        bool current_control_state = control_shm ? *control_shm : true;

        if (current_control_state != last_control_state)
        {
            if (current_control_state)
            {
                reshade::log::message(reshade::log::level::info, "Capture started via SHM trigger.");
                screenshot_count = 0;
            }
            else
            {
                char buffer[128];
                wsprintfA(buffer, "Capture stopped via SHM trigger. Total frames captured: %d", screenshot_count);
                reshade::log::message(reshade::log::level::info, buffer);
            }
            last_control_state = current_control_state;
        }

        if (!current_control_state || shm == nullptr)
        {
            return;
        }

        reshade::api::device* device = runtime->get_device();
        const reshade::api::resource backbuffer = runtime->get_current_back_buffer();
        const reshade::api::resource_desc backbuffer_desc = device->get_resource_desc(backbuffer);
        const uint32_t width = shm->width;
        const uint32_t height = shm->height;

        if (width == 0 || height == 0 || backbuffer_desc.type != reshade::api::resource_type::texture_2d)
        {
            return;
        }

        const size_t pixel_count = static_cast<size_t>(width) * height;
        if (frames_after_runtime_init < 5)
        {
            char buffer[256];
            wsprintfA(buffer, "KKReshade diagnostic: generation=%u frame_after_init=%u capture_frame=%d size=%ux%u format=%u",
                runtime_generation, frames_after_runtime_init + 1, screenshot_count + 1, width, height,
                static_cast<unsigned int>(backbuffer_desc.texture.format));
            reshade::log::message(reshade::log::level::info, buffer);
        }
        ++frames_after_runtime_init;

        // Capture gate: after runtime recreation, require the global ReShade effects
        // state and a non-empty technique list to remain stable for 60 consecutive
        // finish-effects callbacks. Intentionally disabled techniques are allowed.
        // During recovery, do not overwrite shared memory, so VideoExport keeps the
        // last frame captured after the effects callback.
        effect_state_probe probe;
        const bool runtime_effects_ready = effects_ready(runtime, &probe);
        active_technique_count = probe.technique_count;
        enabled_technique_count = probe.enabled_count;
        if (runtime_effects_ready && (last_effect_signature == 0 || last_effect_signature == probe.signature))
            ++effects_ready_streak;
        else
            effects_ready_streak = runtime_effects_ready ? 1u : 0u;
        last_effect_signature = runtime_effects_ready ? probe.signature : 0;

        effects_ready_for_capture = effects_ready_streak >= 60;
        if (!effects_ready_for_capture)
        {
            if (frames_after_runtime_init <= 5 || (frames_after_runtime_init % 30) == 0)
            {
                char gate_buffer[320];
                wsprintfA(gate_buffer, "KKReshade capture gate: generation=%u frame_after_init=%u runtime_ready=%u techniques=%u enabled=%u disabled=%u ready_streak=%u capture=%s last_good_frame=%u",
                    runtime_generation, frames_after_runtime_init, runtime_effects_ready ? 1u : 0u,
                    probe.technique_count, probe.enabled_count, probe.disabled_count,
                    effects_ready_streak, have_good_frame ? "HOLD_LAST" : "WAIT", have_good_frame ? 1u : 0u);
                reshade::log::message(reshade::log::level::info, gate_buffer);
            }
            return;
        }

        if (backbuffer_desc.texture.format == reshade::api::format::r16g16b16a16_float)
        {
            // ReShade's capture_screenshot waits for the current back buffer and returns
            // tightly packed data using the back buffer's native bytes-per-pixel. This
            // avoids reading the previous frame from our own staging resource.
            std::vector<uint16_t> fp16(pixel_count * 4);
            if (runtime->capture_screenshot(fp16.data()))
            {
                convert_fp16_rgba_to_rgba8(fp16.data(), shm->imageBytes, width, height);
                ++screenshot_count;
                have_good_frame = true;
            }
            return;
        }

        if (backbuffer_desc.texture.format == reshade::api::format::r8g8b8a8_unorm ||
            backbuffer_desc.texture.format == reshade::api::format::r8g8b8a8_unorm_srgb)
        {
            std::vector<uint8_t> pixels(pixel_count * COLOR_CHANNELS);
            if (runtime->capture_screenshot(pixels.data()))
            {
                memcpy(shm->imageBytes, pixels.data(), pixels.size());
                ++screenshot_count;
                have_good_frame = true;
            }
            return;
        }

        reshade::log::message(reshade::log::level::warning, "KKReshade: unsupported backbuffer format for VideoExport screenshot capture.");
    }

    void on_init_effect_runtime(const reshade::api::effect_runtime* runtime)
    {
        ++runtime_generation;
        frames_after_runtime_init = 0;
        effects_ready_streak = 0;
        effects_ready_for_capture = false;
        active_technique_count = 0;
        enabled_technique_count = 0;
        last_effect_signature = 0;

        uint32_t width = 0;
        uint32_t height = 0;
        runtime->get_screenshot_width_and_height(&width, &height);

        if (shm != nullptr)
        {
            shm->width = width;
            shm->height = height;
        }

        char buffer[256];
        wsprintfA(buffer, "KKReshade capture gate: runtime INIT generation=%u size=%ux%u capture_enabled=%u",
            runtime_generation, width, height, (control_shm && *control_shm) ? 1u : 0u);
        reshade::log::message(reshade::log::level::info, buffer);
    }

    void on_destroy_effect_runtime(reshade::api::effect_runtime* runtime)
    {
        char buffer[256];
        wsprintfA(buffer, "KKReshade capture gate: runtime DESTROY generation=%u captured_frames=%d; holding last good frame during recovery",
            runtime_generation, screenshot_count);
        reshade::log::message(reshade::log::level::warning, buffer);

        if (shm != nullptr)
        {
            shm->width = 0;
            shm->height = 0;
        }
    }

    void start()
    {
        reshade::log::message(reshade::log::level::info, "Addon initializing...");

        hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ScreenshotMemory) + MAX_IMAGE_SIZE, SCREENSHOT_SHM_NAME);
        if (!hMapFile)
        {
            char buf[128];
            wsprintfA(buf, "Failed to create Image SHM! Error: %lu", GetLastError());
            reshade::log::message(reshade::log::level::error, buf);
            return;
        }

        shm = static_cast<ScreenshotMemory*>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ScreenshotMemory) + MAX_IMAGE_SIZE));
        if (!shm)
        {
            CloseHandle(hMapFile);
            hMapFile = nullptr;
            reshade::log::message(reshade::log::level::error, "Failed to map Image SHM!");
            return;
        }

        shm->channels = COLOR_CHANNELS;

        hControlMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bool), SCREENSHOT_CONTROL_SHM_NAME);
        if (hControlMapFile)
        {
            control_shm = static_cast<bool*>(MapViewOfFile(hControlMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bool)));
            if (control_shm)
            {
                *control_shm = true;
            }
            reshade::log::message(reshade::log::level::info, "Control SHM created/opened successfully.");
        }
        else
        {
            char buf[128];
            wsprintfA(buf, "Failed to create Control SHM! Error: %lu", GetLastError());
            reshade::log::message(reshade::log::level::error, buf);
        }
    }

    void stop()
    {
        if (shm) UnmapViewOfFile(shm);
        if (hMapFile) CloseHandle(hMapFile);

        if (control_shm) UnmapViewOfFile(control_shm);
        if (hControlMapFile) CloseHandle(hControlMapFile);

        shm = nullptr;
        hMapFile = nullptr;
        control_shm = nullptr;
        hControlMapFile = nullptr;
    }
} // namespace screenshot
