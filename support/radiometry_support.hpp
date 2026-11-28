#pragma once

#include <glm/glm.hpp>

namespace MayaFlux::Test::Radiometry {

/** @brief Unit direction at a cosine from +Z and an azimuth. */
inline glm::vec3 direction_at(double cos_theta, double phi)
{
    const double radius = std::sqrt(std::max(1.0 - cos_theta * cos_theta, 0.0));
    return { static_cast<float>(radius * std::cos(phi)), static_cast<float>(radius * std::sin(phi)),
        static_cast<float>(cos_theta) };
}

/** @brief Midpoint rule over the +Z hemisphere, per solid angle. */
template <typename Fn>
double integrate_hemisphere(const Fn& fn, int rings = 512, int sectors = 512)
{
    const double d_cos = 1.0 / rings;
    const double d_phi = 2.0 * std::numbers::pi / sectors;
    double sum = 0.0;
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < sectors; ++j) {
            sum += static_cast<double>(fn(direction_at((i + 0.5) * d_cos, (j + 0.5) * d_phi)));
        }
    }
    return sum * d_cos * d_phi;
}

inline bool finite(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace MayaFlux::Test::Radiometry
