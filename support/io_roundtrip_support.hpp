#pragma once

#include <fstream>
#include <glm/glm.hpp>

#include "MayaFlux/IO/FileWriter.hpp"

namespace MayaFlux::Test {

/**
 * @brief Byte, path and corruption helpers shared by the file format round trip tests.
 */
namespace RoundTrip {

    inline std::string path_in_cwd(const std::string& name)
    {
        return (std::filesystem::current_path() / name).string();
    }

    inline std::string read_bytes(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    inline void write_bytes(const std::string& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    inline bool exists(const std::string& path)
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }

    inline std::string truncated(const std::string& bytes, size_t keep)
    {
        return bytes.substr(0, std::min(keep, bytes.size()));
    }

    inline std::string with_byte_flipped(std::string bytes, size_t index)
    {
        if (index < bytes.size()) {
            bytes[index] = static_cast<char>(~static_cast<unsigned char>(bytes[index]));
        }
        return bytes;
    }

    inline std::string with_prefix(const std::string& prefix, size_t total_size, char fill = '\x5a')
    {
        std::string bytes = prefix;
        if (bytes.size() < total_size) {
            bytes.append(total_size - bytes.size(), fill);
        }
        return bytes;
    }

    inline std::string garbage(size_t size, uint32_t seed = 1U)
    {
        std::string bytes(size, '\0');
        uint32_t state = seed * 2654435761U + 1U;
        for (auto& byte : bytes) {
            state = state * 1664525U + 1013904223U;
            byte = static_cast<char>(state >> 24U);
        }
        return bytes;
    }

    inline std::string base64(const std::string& input)
    {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        output.reserve(((input.size() + 2U) / 3U) * 4U);
        for (size_t i = 0; i < input.size(); i += 3U) {
            const uint32_t b0 = static_cast<unsigned char>(input[i]);
            const uint32_t b1 = i + 1U < input.size() ? static_cast<unsigned char>(input[i + 1U]) : 0U;
            const uint32_t b2 = i + 2U < input.size() ? static_cast<unsigned char>(input[i + 2U]) : 0U;
            const uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;
            output.push_back(alphabet[(triple >> 18U) & 63U]);
            output.push_back(alphabet[(triple >> 12U) & 63U]);
            output.push_back(i + 1U < input.size() ? alphabet[(triple >> 6U) & 63U] : '=');
            output.push_back(i + 2U < input.size() ? alphabet[triple & 63U] : '=');
        }
        return output;
    }

    inline bool near(float a, float b, float tolerance = 1e-4F)
    {
        return std::abs(a - b) <= tolerance;
    }

    inline bool near(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4F)
    {
        return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(tolerance)));
    }

    inline bool near(const glm::vec2& a, const glm::vec2& b, float tolerance = 1e-4F)
    {
        return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec2(tolerance)));
    }

    inline bool near(const glm::vec4& a, const glm::vec4& b, float tolerance = 1e-4F)
    {
        return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec4(tolerance)));
    }

}

}
