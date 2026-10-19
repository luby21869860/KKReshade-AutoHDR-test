#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "screenshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
            }
            return;
        }

        reshade::log::message(reshade::log::level::warning, "KKReshade: unsupported backbuffer format for VideoExport screenshot capture.");
    }

    void on_init_effect_runtime(const reshade::api::effect_runtime* runtime)
    {
        ++runtime_generation;
        frames_after_runtime_init = 0;

        uint32_t width = 0;
        uint32_t height = 0;
        runtime->get_screenshot_width_and_height(&width, &height);

        if (shm != nullptr)
        {
            shm->width = width;
            shm->height = height;
        }

        char buffer[256];
        wsprintfA(buffer, "KKReshade diagnostic: runtime INIT generation=%u size=%ux%u capture_enabled=%u",
            runtime_generation, width, height, (control_shm && *control_shm) ? 1u : 0u);
        reshade::log::message(reshade::log::level::info, buffer);
    }

    void on_destroy_effect_runtime(reshade::api::effect_runtime* runtime)
    {
        char buffer[256];
        wsprintfA(buffer, "KKReshade diagnostic: runtime DESTROY generation=%u captured_frames=%d",
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
