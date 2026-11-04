#pragma once

#include "MayaFlux/Kakshya/Source/SoundFileContainer.hpp"

namespace MayaFlux::Test {

/**
 * @brief A populated audio container with one sample vector per channel.
 *
 * Builds the same SoundFileContainer the granular workflow produces, so its
 * structure, organisation and data accessors are those of a real container.
 *
 * @param channels   Per-channel samples. Every channel must be non-empty.
 * @param sample_rate Sample rate in Hz.
 */
[[nodiscard]] inline std::shared_ptr<Kakshya::SoundFileContainer> make_audio_container(
    std::vector<std::vector<double>> channels,
    uint32_t sample_rate = 48000)
{
    const auto count = static_cast<uint32_t>(channels.size());
    return Kakshya::make_sound_file_container(
        std::move(channels), count, sample_rate, Kakshya::OrganizationStrategy::PLANAR);
}

/**
 * @brief A populated single-channel audio container.
 * @param samples Samples of the one channel.
 * @param sample_rate Sample rate in Hz.
 */
[[nodiscard]] inline std::shared_ptr<Kakshya::SoundFileContainer> make_audio_container(
    const std::vector<double>& samples,
    uint32_t sample_rate = 48000)
{
    return make_audio_container(std::vector<std::vector<double>> { samples }, sample_rate);
}

}
