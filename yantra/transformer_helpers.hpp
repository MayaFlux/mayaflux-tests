#pragma once

#include "MayaFlux/Yantra/Transformers/Transformer.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace MayaFlux::Test {

/**
 * @brief Builds a transformer input with one DataVariant per channel.
 * @param channels Per-channel samples.
 */
inline Yantra::Datum<std::vector<Kakshya::DataVariant>> to_datum(
    const std::vector<std::vector<double>>& channels)
{
    std::vector<Kakshya::DataVariant> data;
    data.reserve(channels.size());
    for (const auto& channel : channels) {
        data.emplace_back(channel);
    }
    return Yantra::Datum<std::vector<Kakshya::DataVariant>>(data);
}

/**
 * @brief Returns the samples of one channel of a transformer result.
 * @param datum Transformer result.
 * @param index Channel index.
 */
inline std::vector<double> channel_samples(
    const Yantra::Datum<std::vector<Kakshya::DataVariant>>& datum,
    std::size_t index)
{
    return std::get<std::vector<double>>(datum.data[index]);
}

/**
 * @brief Sine wave of the given length.
 */
inline std::vector<double> sine_samples(std::size_t samples, double frequency, double sample_rate = 48000.0, double amplitude = 1.0)
{
    std::vector<double> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        out[i] = amplitude * std::sin(2.0 * std::numbers::pi * frequency * t);
    }
    return out;
}

/**
 * @brief Linear ramp from -1 to +1 inclusive.
 */
inline std::vector<double> ramp_samples(std::size_t samples)
{
    std::vector<double> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = -1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(samples - 1);
    }
    return out;
}

}
