#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace MayaFlux::Test::IntervalReference {

using Intervals = std::vector<std::pair<size_t, size_t>>;

/**
 * @brief One case of the Silero get_speech_timestamps fixture, with its settings as the fixture states them.
 */
struct Case {
    std::string name;
    double enter {};
    uint32_t min_run {};
    uint32_t min_gap {};
    uint32_t padding {};
    uint32_t gap_at_limit {};
    uint32_t max_run {};
    bool longest {};
    uint32_t length {};
    std::vector<double> scores;
    Intervals expect;
};

inline std::filesystem::path fixture_path()
{
    return "tests/yantra/gpu/interval_reference/interval_reference.txt";
}

/**
 * @brief Every case of the fixture, empty when the file is not present.
 */
inline std::vector<Case> load()
{
    std::vector<Case> cases;
    std::ifstream file(fixture_path());
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "case") {
            cases.emplace_back();
            in >> cases.back().name;
        } else if (key == "params") {
            auto& c = cases.back();
            int longest = 0;
            in >> c.enter >> c.min_run >> c.min_gap >> c.padding >> c.gap_at_limit >> c.max_run >> longest >> c.length;
            c.longest = longest != 0;
        } else if (key == "scores") {
            float value = 0.0F;
            while (in >> value) {
                cases.back().scores.push_back(static_cast<double>(value));
            }
        } else if (key == "expect") {
            size_t start = 0;
            size_t end = 0;
            while (in >> start >> end) {
                cases.back().expect.emplace_back(start, end);
            }
        }
    }
    return cases;
}

/**
 * @brief Alternating high and low score runs with occasional gate dropouts, the same for a given seed.
 */
inline void random_gated(uint32_t seed, size_t count, std::vector<double>& scores, std::vector<double>& gate)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    scores.resize(count);
    gate.resize(count);
    bool speaking = false;
    for (size_t i = 0; i < count; ++i) {
        if (unit(rng) < 0.04) {
            speaking = !speaking;
        }
        scores[i] = speaking ? 0.55 + 0.4 * unit(rng) : 0.3 * unit(rng);
        gate[i] = unit(rng) < 0.07 ? 0.0 : 1.0;
    }
}

} // namespace MayaFlux::Test::IntervalReference
