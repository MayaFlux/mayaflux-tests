#pragma once

#include <fstream>

#include "MayaFlux/IO/CodecContext/FFmpegMuxContext.hpp"
#include "MayaFlux/IO/CodecContext/VideoEncodeContext.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace MayaFlux::Test::FFmpegFixture {

inline std::string make_wav(uint16_t channels, uint32_t rate, uint32_t frames)
{
    const uint32_t data_size = frames * channels * 2U;
    std::string out;

    auto put32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    };
    auto put16 = [&out](uint16_t v) {
        out.push_back(static_cast<char>(v & 0xFFU));
        out.push_back(static_cast<char>((v >> 8) & 0xFFU));
    };

    out += "RIFF";
    put32(36U + data_size);
    out += "WAVE";
    out += "fmt ";
    put32(16U);
    put16(1);
    put16(channels);
    put32(rate);
    put32(rate * channels * 2U);
    put16(static_cast<uint16_t>(channels * 2U));
    put16(16);
    out += "data";
    put32(data_size);
    out.append(data_size, '\0');
    return out;
}

inline std::string noise_bytes(size_t count)
{
    std::string out(count, '\0');
    uint32_t state = 0x9E3779B9U;
    for (auto& c : out) {
        state = state * 1664525U + 1013904223U;
        c = static_cast<char>(state >> 24);
    }
    return out;
}

inline void write_bytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/**
 * @brief Encode a small MPEG-4 part 2 clip in an MP4 container.
 *
 * MPEG-4 part 2 is the encoder ffmpeg ships built in, so the clip does not
 * depend on an external codec library being present.
 */
inline bool write_mpeg4_clip(const std::filesystem::path& path,
    uint32_t width, uint32_t height, uint32_t frames, double fps)
{
    IO::FFmpegMuxContext mux;
    if (!mux.open(path.string(), "mp4"))
        return false;

    IO::VideoEncodeContext enc;
    if (!enc.open(mux, width, height, fps, AV_PIX_FMT_RGBA, AV_CODEC_ID_MPEG4))
        return false;

    if (!mux.write_header())
        return false;

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4U);
    for (uint32_t f = 0; f < frames; ++f) {
        std::fill(pixels.begin(), pixels.end(), static_cast<uint8_t>(f * 7U));
        if (!enc.encode_frame(pixels.data(), pixels.size(), width, height, mux))
            return false;
    }

    if (!enc.drain(mux))
        return false;

    enc.close();
    mux.close();
    return true;
}

}
