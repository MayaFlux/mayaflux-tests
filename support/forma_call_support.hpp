#pragma once

#include "MayaFlux/Portal/Forma/Primitives/FormFactory.hpp"
#include "MayaFlux/Transitive/Reflect/Describe.hpp"

namespace MayaFlux::Test {

/**
 * @brief What the probe functions below were called with, in call order.
 *
 * The functions are plain free functions, as a user's would be, so what a described call
 * passes them is observed here.
 */
inline std::vector<std::string>& forma_call_log()
{
    static std::vector<std::string> log;
    return log;
}

/** @brief A real fader form, so a placed element draws and hit tests, with defaults and an undecodable last parameter. */
inline Portal::Forma::Geometry::Form<float> forma_call_fader(Kinesis::AABB2D bounds, float handle_w,
    glm::vec3 track = glm::vec3(0.3F), const std::function<glm::vec2()>& live_offset = {})
{
    forma_call_log().push_back("fader " + std::to_string(bounds.min.x) + " " + std::to_string(bounds.max.y)
        + " " + std::to_string(handle_w) + " " + std::to_string(track.x) + (live_offset ? " offset" : " plain"));
    return Portal::Forma::Geometry::horizontal_fader(bounds, handle_w, track, glm::vec3(0.9F), live_offset);
}

/** @brief Defaults that only a trailing run may leave out. */
inline Portal::Forma::Geometry::Form<float> forma_call_triple(float a, float b = 1.F, float c = 2.F)
{
    forma_call_log().push_back("triple " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c));
    return Portal::Forma::Geometry::horizontal_fader(
        Kinesis::AABB2D { .min = { -0.1F, -0.1F }, .max = { 0.1F, 0.1F } }, 0.02F);
}

/** @brief A state that is a list, and no defaults. */
inline Portal::Forma::Geometry::Form<std::vector<float>> forma_call_samples(uint32_t count)
{
    forma_call_log().push_back("samples " + std::to_string(count));
    return Portal::Forma::Geometry::drawable_canvas(
        Kinesis::AABB2D { .min = { -0.5F, -0.5F }, .max = { 0.5F, 0.5F } }, glm::vec3(0.8F), 1.5F, {}, count);
}

/** @brief No parameters at all. */
inline Portal::Forma::Geometry::Form<bool> forma_call_flag()
{
    forma_call_log().push_back("flag");
    return Portal::Forma::Geometry::toggle(
        Kinesis::AABB2D { .min = { -0.2F, -0.2F }, .max = { 0.2F, 0.2F } });
}

}

MF_DESCRIBE_FUNCTION(&MayaFlux::Test::forma_call_fader)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::forma_call_triple)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::forma_call_samples)
MF_DESCRIBE_FUNCTION(&MayaFlux::Test::forma_call_flag)

#include "forma_call_support.signatures.hpp"
