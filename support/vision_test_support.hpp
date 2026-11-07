#pragma once

#include "gpu_dispatch_test_support.hpp"

#include "MayaFlux/Kakshya/NDData/NDData.hpp"
#include "MayaFlux/Kakshya/Source/TextureContainer.hpp"
#include "MayaFlux/Portal/Graphics/SamplerForge.hpp"
#include "MayaFlux/Portal/Graphics/TextureLoom.hpp"
#include "MayaFlux/Yantra/Executors/VisionGpuDispatch.hpp"

namespace MayaFlux::Test::Vision {

using ImageHandle = std::shared_ptr<Core::VKImage>;

inline constexpr float k_alias_tolerance = 1e-3F;

/**
 * @brief Fixture for Vision executor tests: a headless backend with the
 *        foundry and press up, plus the texture manager and sampler forge the
 *        Vision contexts draw on. Skips when no Vulkan device is present.
 */
class VisionGpuFixture : public GpuDispatch::GpuDispatchFixture<true> {
protected:
    void SetUp() override
    {
        GpuDispatchFixture<true>::SetUp();
        if (this->IsSkipped() || this->HasFatalFailure()) {
            return;
        }
        ASSERT_TRUE(Portal::Graphics::get_texture_manager().initialize(this->backend()));
        ASSERT_TRUE(Portal::Graphics::SamplerForge::instance().initialize(this->backend()));
    }

    void TearDown() override
    {
        if (this->backend()) {
            Portal::Graphics::SamplerForge::instance().shutdown();
            Portal::Graphics::get_texture_manager().shutdown();
        }
        // Vision tests assert on decoded results, not on the validation layer's
        // log stream. The shared base TearDown fails any test that logged a
        // validation message, which would make the Vision suite red in CI.
        GpuDispatchFixture<true>::teardown_portal();
    }
};

/**
 * @brief Upload a row-major RGBA32F pixel buffer as a new image.
 */
inline ImageHandle rgba_image(uint32_t w, uint32_t h, const std::vector<float>& pixels)
{
    const Kakshya::DataVariant variant { pixels };
    return Portal::Graphics::TextureLoom::instance().create_2d(variant, w, h, Portal::Graphics::ImageFormat::RGBA32F);
}

/**
 * @brief Read an RGBA32F image back as floats, four per texel, through its
 *        own dimensions.
 */
inline std::vector<float> read_floats(const ImageHandle& image)
{
    std::vector<float> pixels(static_cast<size_t>(image->get_width()) * image->get_height() * 4);
    Portal::Graphics::TextureLoom::instance().download_data(
        image, pixels.data(), pixels.size() * sizeof(float));
    return pixels;
}

/**
 * @brief Readback through TextureContainer's normalised view, the path the
 *        threshold and canny checks use.
 */
inline std::vector<float> read_normalised(const ImageHandle& image)
{
    Kakshya::TextureContainer readback(image, Portal::Graphics::ImageFormat::RGBA32F);
    const auto view = readback.as_normalised_float(0);
    return { view.begin(), view.end() };
}

inline float red_at(const std::vector<float>& pixels, uint32_t x, uint32_t y, uint32_t size)
{
    const size_t idx = (static_cast<size_t>(y) * size + x) * 4;
    return idx < pixels.size() ? pixels[idx] : -1.0F;
}

inline float max_abs_diff(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || a.size() != b.size()) {
        return std::numeric_limits<float>::infinity();
    }
    float m = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, std::abs(a[i] - b[i]));
    }
    return m;
}

inline size_t count_lit(const std::vector<float>& pixels)
{
    size_t n = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] > 0.5F) {
            ++n;
        }
    }
    return n;
}

/**
 * @brief size x size RGBA32F frame whose pixel (x, y) is fn(x, y).
 */
inline ImageHandle pattern_frame(uint32_t size, const std::function<glm::vec4(uint32_t, uint32_t)>& fn)
{
    std::vector<float> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const glm::vec4 c = fn(x, y);
            const size_t idx = (static_cast<size_t>(y) * size + x) * 4;
            pixels[idx + 0] = c.r;
            pixels[idx + 1] = c.g;
            pixels[idx + 2] = c.b;
            pixels[idx + 3] = c.a;
        }
    }
    return rgba_image(size, size, pixels);
}

/**
 * @brief Set one RGBA pixel of a row-major RGBA32F buffer, all four channels
 *        to v.
 */
inline void set_all_channels(std::vector<float>& pixels, uint32_t w, uint32_t x, uint32_t y, float v)
{
    const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
    pixels[idx + 0] = v;
    pixels[idx + 1] = v;
    pixels[idx + 2] = v;
    pixels[idx + 3] = v;
}

/**
 * @brief size x size frame of a solid colour inside a normalised region and
 *        black outside it.
 */
inline ImageHandle solid_region_frame(uint32_t size, const Kinesis::Vision::BoundingBox& region, glm::vec4 color)
{
    std::vector<float> pixels(static_cast<size_t>(size) * size * 4, 0.0F);
    const auto x0 = static_cast<uint32_t>(region.x * static_cast<float>(size));
    const auto y0 = static_cast<uint32_t>(region.y * static_cast<float>(size));
    const auto x1 = static_cast<uint32_t>((region.x + region.w) * static_cast<float>(size));
    const auto y1 = static_cast<uint32_t>((region.y + region.h) * static_cast<float>(size));
    for (uint32_t y = y0; y < y1 && y < size; ++y) {
        for (uint32_t x = x0; x < x1 && x < size; ++x) {
            const size_t idx = (static_cast<size_t>(y) * size + x) * 4;
            pixels[idx + 0] = color.r;
            pixels[idx + 1] = color.g;
            pixels[idx + 2] = color.b;
            pixels[idx + 3] = color.a;
        }
    }
    return rgba_image(size, size, pixels);
}

/**
 * @brief Irregular sum-of-sines intensity. A regular pattern gives exact ties
 *        in Harris response, which extract_peaks' strict NMS rejects, so the
 *        local maxima of this texture are unique.
 */
inline float wave_intensity(float x, float y)
{
    const float v = 0.5F
        + 0.25F * std::sin(x * 0.13F + y * 0.07F)
        + 0.15F * std::sin(x * 0.05F - y * 0.19F)
        + 0.10F * std::sin(x * 0.29F + y * 0.11F + 1.7F);
    return std::clamp(v, 0.0F, 1.0F);
}

/**
 * @brief Wave-textured frame sampled at (x - dx, y - dy), so a positive shift
 *        moves the pattern in +x and +y.
 */
inline ImageHandle wave_frame(uint32_t size, float dx, float dy)
{
    std::vector<float> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float v = wave_intensity(static_cast<float>(x) - dx, static_cast<float>(y) - dy);
            set_all_channels(pixels, size, x, y, v);
            pixels[(static_cast<size_t>(y) * size + x) * 4 + 3] = 1.0F;
        }
    }
    return rgba_image(size, size, pixels);
}

/**
 * @brief One plane wave of the flow texture.
 */
struct FlowWave {
    float kx;
    float ky;
    float phase;
};

/**
 * @brief Fixed two-scale wave set: eight long waves for pyramid structure and
 *        twenty short waves for tracking windows. Seeded, so every frame
 *        shares one analytic texture and a fractional shift is exact.
 */
inline const std::vector<FlowWave>& flow_waves()
{
    static const std::vector<FlowWave> waves = [] {
        constexpr float two_pi = 6.2831853F;
        constexpr uint32_t long_count = 8;
        constexpr uint32_t short_count = 20;
        uint32_t state = 0x9E3779B9U;
        const auto next = [&state]() {
            state = state * 1664525U + 1013904223U;
            return static_cast<float>(state >> 8) / 16777216.0F;
        };
        std::vector<FlowWave> out;
        out.reserve(size_t { long_count } + short_count);
        for (uint32_t i = 0; i < long_count + short_count; ++i) {
            const float wavelength = i < long_count ? 32.0F + 80.0F * next() : 10.0F + 14.0F * next();
            const float k = two_pi / wavelength;
            const float angle = two_pi * next();
            out.push_back({ .kx = k * std::cos(angle), .ky = k * std::sin(angle), .phase = two_pi * next() });
        }
        return out;
    }();
    return waves;
}

inline float flow_intensity(float x, float y)
{
    float sum = 0.0F;
    for (const auto& w : flow_waves()) {
        sum += std::sin(w.kx * x + w.ky * y + w.phase);
    }
    return std::clamp(0.5F + 0.3F * sum / std::sqrt(static_cast<float>(flow_waves().size())), 0.02F, 0.98F);
}

/**
 * @brief Flow texture displaced by (dx, dy), or flat mid grey when blank.
 */
inline ImageHandle flow_frame(uint32_t size, float dx, float dy, bool blank)
{
    std::vector<float> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float v = blank ? 0.5F : flow_intensity(static_cast<float>(x) - dx, static_cast<float>(y) - dy);
            set_all_channels(pixels, size, x, y, v);
            pixels[(static_cast<size_t>(y) * size + x) * 4 + 3] = 1.0F;
        }
    }
    return rgba_image(size, size, pixels);
}

/**
 * @brief Flow texture with horizontal shear and an optional occluder.
 */
inline ImageHandle flow_warp_frame(uint32_t size, float dx, float dy, float shear, bool occluded)
{
    const float center = 0.5F * static_cast<float>(size - 1U);
    const uint32_t occlusion_lo = size * 5U / 16U;
    const uint32_t occlusion_hi = size * 11U / 16U;

    std::vector<float> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float source_y = static_cast<float>(y) - dy;
            const float source_x = static_cast<float>(x) - dx - shear * (source_y - center);
            const bool hidden = occluded
                && x >= occlusion_lo && x < occlusion_hi
                && y >= occlusion_lo && y < occlusion_hi;
            const float v = hidden ? 0.5F : flow_intensity(source_x, source_y);
            set_all_channels(pixels, size, x, y, v);
            pixels[(static_cast<size_t>(y) * size + x) * 4 + 3] = 1.0F;
        }
    }
    return rgba_image(size, size, pixels);
}

/**
 * @brief Border margin inside which a dense flow pixel is not judged: the
 *        coarsest level's window reaches radius * 2^(levels - 1) pixels, plus
 *        bilinear support.
 */
inline float flow_dense_margin(uint32_t radius, uint32_t levels)
{
    return static_cast<float>(radius * (1U << (levels - 1U)) + 4U);
}

inline std::vector<float> flow_download(const ImageHandle& image)
{
    return read_floats(image);
}

/**
 * @brief Sparse track accuracy against a known shift, judged only where the
 *        source and displaced windows both fit inside the frame.
 */
struct TrackStats {
    size_t points { 0 };
    size_t evaluated { 0 };
    size_t tracked { 0 };
    size_t tracked_any { 0 };
    size_t within { 0 };
    float mean_error { 0.0F };
    float max_error { 0.0F };
};

inline TrackStats measure_tracks(
    const std::vector<Kinesis::Vision::TrackResult>& tracks,
    uint32_t size, float dx, float dy, float tolerance)
{
    constexpr float margin = 8.0F;
    const auto extent = static_cast<float>(size);
    const auto inside = [&](const glm::vec2& px) {
        return px.x >= margin && px.y >= margin
            && px.x <= extent - 1.0F - margin && px.y <= extent - 1.0F - margin;
    };

    TrackStats stats;
    stats.points = tracks.size();
    float sum = 0.0F;
    for (const auto& t : tracks) {
        if (t.tracked) {
            ++stats.tracked_any;
        }
        const glm::vec2 source = t.previous * extent;
        if (!inside(source) || !inside(source + glm::vec2(dx, dy))) {
            continue;
        }
        ++stats.evaluated;
        if (!t.tracked) {
            continue;
        }
        const glm::vec2 flow = (t.position - t.previous) * extent;
        const float err = glm::length(flow - glm::vec2(dx, dy));
        ++stats.tracked;
        sum += err;
        stats.max_error = std::max(stats.max_error, err);
        if (err <= tolerance) {
            ++stats.within;
        }
    }
    stats.mean_error = stats.tracked > 0 ? sum / static_cast<float>(stats.tracked) : 0.0F;
    return stats;
}

/**
 * @brief Dense flow accuracy against a known shift. Confident pixels are
 *        those whose confidence channel clears the solver's own floor.
 */
struct DenseStats {
    size_t interior { 0 };
    size_t confident { 0 };
    size_t within { 0 };
    float mean_error { 0.0F };
    float max_error { 0.0F };
    float max_flow { 0.0F };
};

inline DenseStats measure_dense(
    const std::vector<float>& px, uint32_t size, float dx, float dy, float tolerance, float margin)
{
    constexpr float min_confidence = 1e-3F;
    const auto extent = static_cast<float>(size);
    const auto inside = [&](float x, float y) {
        return x >= margin && y >= margin && x <= extent - 1.0F - margin && y <= extent - 1.0F - margin;
    };

    DenseStats stats;
    float sum = 0.0F;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            if (!inside(fx, fy) || !inside(fx + dx, fy + dy)) {
                continue;
            }
            ++stats.interior;
            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            const glm::vec2 flow(px[i], px[i + 1]);
            stats.max_flow = std::max(stats.max_flow, glm::length(flow));
            if (px[i + 2] < min_confidence) {
                continue;
            }
            ++stats.confident;
            const float err = glm::length(flow - glm::vec2(dx, dy));
            sum += err;
            stats.max_error = std::max(stats.max_error, err);
            if (err <= tolerance) {
                ++stats.within;
            }
        }
    }
    stats.mean_error = stats.confident > 0 ? sum / static_cast<float>(stats.confident) : 0.0F;
    return stats;
}

/**
 * @brief Pixel box of a normalised ConnectedComponents box, inclusive on both
 *        ends, as {x0, y0, x1, y1}.
 */
inline std::array<uint32_t, 4> pixel_box(const Kinesis::Vision::BoundingBox& b, uint32_t w, uint32_t h)
{
    const auto x0 = static_cast<uint32_t>(std::round(b.x * static_cast<float>(w)));
    const auto y0 = static_cast<uint32_t>(std::round(b.y * static_cast<float>(h)));
    const auto x1 = x0 + static_cast<uint32_t>(std::round(b.w * static_cast<float>(w))) - 1U;
    const auto y1 = y0 + static_cast<uint32_t>(std::round(b.h * static_cast<float>(h))) - 1U;
    return { x0, y0, x1, y1 };
}

/**
 * @brief Pixel box spanned by a contour's normalised points, inclusive.
 */
inline std::array<uint32_t, 4> contour_pixel_box(const Kinesis::Vision::Contour& c, uint32_t w, uint32_t h)
{
    float min_x = 1.0F;
    float min_y = 1.0F;
    float max_x = 0.0F;
    float max_y = 0.0F;
    for (const auto& p : c.points) {
        min_x = std::min(min_x, p.x);
        min_y = std::min(min_y, p.y);
        max_x = std::max(max_x, p.x);
        max_y = std::max(max_y, p.y);
    }
    return {
        static_cast<uint32_t>(std::round(min_x * static_cast<float>(w))),
        static_cast<uint32_t>(std::round(min_y * static_cast<float>(h))),
        static_cast<uint32_t>(std::round(max_x * static_cast<float>(w))),
        static_cast<uint32_t>(std::round(max_y * static_cast<float>(h))),
    };
}

/**
 * @brief Named image slots of a result, one entry per ImageOutputs member, for
 *        checking which slots an op populated and which share a handle.
 */
inline std::vector<std::pair<const char*, ImageHandle>> named_slots(const Kinesis::Vision::VisionResult& r)
{
    const auto& i = r.images;
    return {
        { "gray", r.gray },
        { "flow", r.flow },
        { "rgba_to_hsv", i.rgba_to_hsv },
        { "gray_to_rgba", i.gray_to_rgba },
        { "downsample_2x", i.downsample_2x },
        { "threshold", i.threshold },
        { "threshold_adaptive", i.threshold_adaptive },
        { "threshold_otsu", i.threshold_otsu },
        { "normalize_inplace", i.normalize_inplace },
        { "normalize_range", i.normalize_range },
        { "gaussian_blur", i.gaussian_blur },
        { "filter_separable", i.filter_separable },
        { "sobel", i.sobel },
        { "scharr", i.scharr },
        { "canny", i.canny },
        { "erode", i.erode },
        { "dilate", i.dilate },
        { "open", i.open },
        { "close", i.close },
        { "morph_gradient", i.morph_gradient },
        { "harris_response", i.harris_response },
        { "confine", i.confine },
        { "component_colors", i.component_colors },
        { "contour_image", i.contour_image },
        { "flow_visualization", i.flow_visualization },
    };
}

/**
 * @brief Grid of n_columns x n_rows solid 3x3 squares, the layout the
 *        many-contour checks use, with their pixel boxes.
 */
struct BoxGrid {
    std::vector<float> pixels;
    std::vector<std::array<uint32_t, 4>> boxes;
};

inline BoxGrid square_grid(uint32_t w, uint32_t h, uint32_t columns, uint32_t rows,
    uint32_t origin, uint32_t pitch_x, uint32_t pitch_y, uint32_t side)
{
    BoxGrid grid;
    grid.pixels.assign(static_cast<size_t>(w) * h * 4, 0.0F);
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t col = 0; col < columns; ++col) {
            const uint32_t x0 = origin + col * pitch_x;
            const uint32_t y0 = origin + row * pitch_y;
            grid.boxes.push_back({ x0, y0, x0 + side - 1U, y0 + side - 1U });
            for (uint32_t y = y0; y < y0 + side; ++y) {
                for (uint32_t x = x0; x < x0 + side; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                    std::fill_n(grid.pixels.begin() + static_cast<std::ptrdiff_t>(idx), 4, 1.0F);
                }
            }
        }
    }
    return grid;
}

} // namespace MayaFlux::Test::Vision
