#pragma once

#include "gtest/gtest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "MayaFlux/Portal/Text/FontDiscovery.hpp"
#include "MayaFlux/Portal/Text/FontFace.hpp"
#include "MayaFlux/Portal/Text/TypeFaceFoundry.hpp"

namespace MayaFlux::Test {

/**
 * @brief Path of a system font that every supported platform is expected to ship.
 *
 * Tries generic and per-platform families through find_font and keeps the first
 * result that exists on disk. The Noto families come last: on macOS the font
 * directories only hold per-script Noto files that lack Latin glyphs, and
 * find_font matches file names by substring. Nothing is bundled with the tests; a host with no
 * resolvable font fails the suite rather than skipping it.
 */
inline const std::optional<std::string>& system_font_path()
{
    static const std::optional<std::string> path = [] () -> std::optional<std::string> {
        const std::vector<std::string_view> families = {
            "sans-serif", "DejaVu Sans", "Liberation Sans",
            "Arial", "Helvetica", "Segoe UI", "Menlo", "Noto Sans"
        };
        for (const auto family : families) {
            if (const auto found = Portal::Text::find_font(family);
                found && std::filesystem::exists(*found)) {
                return found;
            }
        }
        return std::nullopt;
    }();
    return path;
}

/**
 * @brief Second, distinct system font file, for fallback and reload cases.
 *
 * @return Path differing from system_font_path(), or nullopt when the host only
 *         exposes one resolvable file.
 */
inline const std::optional<std::string>& second_system_font_path()
{
    static const std::optional<std::string> path = [] () -> std::optional<std::string> {
        const auto& first = system_font_path();
        const std::vector<std::string_view> families = {
            "monospace", "DejaVu Sans Mono", "Liberation Mono", "Liberation Serif",
            "Courier New", "Courier", "Times New Roman", "Times", "Consolas", "Menlo", "Georgia", "Noto Serif"
        };
        for (const auto family : families) {
            if (const auto found = Portal::Text::find_font(family);
                found && std::filesystem::exists(*found) && found != first) {
                return found;
            }
        }
        return std::nullopt;
    }();
    return path;
}

/**
 * @brief Brings the FreeType library up for a test and loads the system font.
 *
 * The foundry is a process singleton, so a library already initialized by
 * something else is left running at teardown. The face is always unloaded
 * before the library can go away.
 */
class SystemFontTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto& foundry = Portal::Text::TypeFaceFoundry::instance();
        m_owns_library = !foundry.is_initialized();
        ASSERT_TRUE(foundry.initialize());

        const auto& path = system_font_path();
        ASSERT_TRUE(path.has_value()) << "no system font could be resolved on this host";
        font_path = *path;
        ASSERT_TRUE(face.load(font_path)) << font_path;
    }

    void TearDown() override
    {
        face.unload();
        if (m_owns_library) {
            Portal::Text::TypeFaceFoundry::instance().shutdown();
        }
    }

    Portal::Text::FontFace face;
    std::string font_path;

private:
    bool m_owns_library { false };
};

} // namespace MayaFlux::Test
