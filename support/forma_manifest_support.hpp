#pragma once

#include "MayaFlux/Transitive/Reflect/RecordBinding.hpp"

namespace MayaFlux::Test {

/**
 * @brief An in-memory element tree that is not XML.
 *
 * Lets the Forma manifest layer be exercised through a Reflect::RecordAccess
 * of its own, so nothing in these tests depends on a file or a parser.
 * Rows are assigned in preorder by manifest_numbered.
 */
struct FormaManifestElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<FormaManifestElement> children;
    uint64_t row {};
};

}

namespace MayaFlux::Reflect {

template <>
struct RecordAccess<Test::FormaManifestElement> {
    using Source = Test::FormaManifestElement;

    static std::string_view name(const Source& source) { return source.name; }
    static uint64_t location(const Source& source) { return source.row; }

    static std::optional<std::string_view> attribute(const Source& source, std::string_view name)
    {
        for (const auto& [key, value] : source.attributes) {
            if (key == name)
                return std::string_view(value);
        }
        return std::nullopt;
    }

    template <typename Visitor>
    static bool for_each_attribute(const Source& source, Visitor&& visit)
    {
        for (const auto& [key, value] : source.attributes) {
            if (!visit(std::string_view(key), std::string_view(value)))
                return false;
        }
        return true;
    }

    template <typename Visitor>
    static bool for_each_child(const Source& source, Visitor&& visit)
    {
        for (const Source& child : source.children) {
            if (!visit(child))
                return false;
        }
        return true;
    }
};

}

namespace MayaFlux::Test {

inline FormaManifestElement manifest_node(std::string name,
    std::vector<std::pair<std::string, std::string>> attributes = {},
    std::vector<FormaManifestElement> children = {})
{
    return FormaManifestElement {
        .name = std::move(name),
        .attributes = std::move(attributes),
        .children = std::move(children),
    };
}

inline void number_manifest_rows(FormaManifestElement& element, uint64_t& next)
{
    element.row = next++;
    for (FormaManifestElement& child : element.children)
        number_manifest_rows(child, next);
}

inline FormaManifestElement manifest_numbered(FormaManifestElement element)
{
    uint64_t next = 0;
    number_manifest_rows(element, next);
    return element;
}

inline std::string manifest_attribute(const FormaManifestElement& element, std::string_view name)
{
    const auto found = Reflect::RecordAccess<FormaManifestElement>::attribute(element, name);
    return found ? std::string(*found) : std::string();
}

/**
 * @brief A valid document: two templates and one surface of three instances.
 *
 * The factory element is named stub. The first child of the root is the
 * templates element, the second is the surface main.
 */
inline FormaManifestElement manifest_document()
{
    return manifest_numbered(manifest_node("forma", { { "version", "1" } },
        {
            manifest_node("templates", {},
                {
                    manifest_node("template", { { "id", "first" } },
                        { manifest_node("stub", { { "size", "3" } }) }),
                    manifest_node("template", { { "id", "second" } },
                        { manifest_node("stub", { { "size", "5" } }) }),
                }),
            manifest_node("surface", { { "id", "main" } },
                {
                    manifest_node("instance", { { "id", "a" }, { "template", "first" }, { "x", "1" } }),
                    manifest_node("instance", { { "id", "b" }, { "template", "second" }, { "x", "2" } }),
                    manifest_node("instance", { { "id", "c" }, { "template", "first" }, { "x", "3" } }),
                }),
        }));
}

}
