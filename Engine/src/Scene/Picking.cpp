#include "Flux/Scene/Picking.hpp"

#include "Flux/Graphics/Camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Flux {
    using Math::Vector3d;

    ScreenProjection::ScreenProjection(const Camera& camera, Math::Vector2f viewMin, Math::Vector2f viewMax)
        : m_right(camera.GetRight()),
          m_up(camera.GetUp()),
          m_forward(camera.GetForward()),
          m_near(camera.nearPlane),
          m_focal(0.5 * (viewMax.y - viewMin.y) / std::tan(0.5 * camera.verticalFov)),
          m_center((viewMin + viewMax) * 0.5f) {}

    Ray ScreenProjection::GetRay(Math::Vector2f pixel) const {
        // Pixels carrés : la même focale horizontalement et verticalement ; y de l'écran vers le bas.
        const double x = (pixel.x - m_center.x) / m_focal;
        const double y = (m_center.y - pixel.y) / m_focal;
        return {{}, Math::Normalize(m_forward + m_right * x + m_up * y)};
    }

    double ScreenProjection::GetDepth(const Vector3d& point) const {
        return Math::Dot(point, m_forward);
    }

    std::optional<Math::Vector2f> ScreenProjection::ToScreen(const Vector3d& point) const {
        const double depth = GetDepth(point);
        if (depth < m_near) {
            return std::nullopt;
        }
        const double scale = m_focal / depth;
        return Math::Vector2f{m_center.x + static_cast<float>(Math::Dot(point, m_right) * scale),
                              m_center.y - static_cast<float>(Math::Dot(point, m_up) * scale)};
    }

    bool ScreenProjection::ProjectSegment(Vector3d a, Vector3d b, Math::Vector2f& screenA, Math::Vector2f& screenB) const {
        const double depthA = GetDepth(a);
        const double depthB = GetDepth(b);
        if (depthA < m_near && depthB < m_near) {
            return false;
        }
        // Une extrémité derrière le plan proche : ramenée sur lui.
        if (depthA < m_near) {
            a = a + (b - a) * ((m_near - depthA) / (depthB - depthA));
        } else if (depthB < m_near) {
            b = b + (a - b) * ((m_near - depthB) / (depthA - depthB));
        }
        const auto project = [&](const Vector3d& p) {
            const double scale = m_focal / std::max(GetDepth(p), m_near);
            return Math::Vector2f{m_center.x + static_cast<float>(Math::Dot(p, m_right) * scale),
                                  m_center.y - static_cast<float>(Math::Dot(p, m_up) * scale)};
        };
        screenA = project(a);
        screenB = project(b);
        return true;
    }

    double ScreenProjection::PixelsToMeters(double pixels, double depth) const {
        return pixels * depth / m_focal;
    }

    std::optional<double> IntersectBox(const Ray& ray, const RelativeTransform& transform, const Math::Box3f& box) {
        const Vector3d scale(transform.scale);
        if (scale.x == 0.0 || scale.y == 0.0 || scale.z == 0.0) {
            return std::nullopt; // boîte aplatie : rien à toucher
        }
        // Rayon dans l'espace local, où la boîte redevient box. Le paramètre t est conservé
        // (la direction n'est pas renormalisée) : c'est toujours une distance le long du rayon d'origine.
        const Math::Quaternionf inverse = transform.rotation.Conjugate();
        const Vector3d origin = inverse.Rotate(ray.origin - transform.position);
        const Vector3d direction = inverse.Rotate(ray.direction);
        const double o[3] = {origin.x / scale.x, origin.y / scale.y, origin.z / scale.z};
        const double d[3] = {direction.x / scale.x, direction.y / scale.y, direction.z / scale.z};
        const double low[3] = {box.min.x, box.min.y, box.min.z};
        const double high[3] = {box.max.x, box.max.y, box.max.z};

        double tEntry = 0.0;
        double tExit = std::numeric_limits<double>::max();
        for (int axis = 0; axis < 3; ++axis) {
            if (d[axis] == 0.0) {
                if (o[axis] < low[axis] || o[axis] > high[axis]) {
                    return std::nullopt; // parallèle à cette tranche et en dehors
                }
                continue;
            }
            double t0 = (low[axis] - o[axis]) / d[axis];
            double t1 = (high[axis] - o[axis]) / d[axis];
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            tEntry = std::max(tEntry, t0);
            tExit = std::min(tExit, t1);
            if (tEntry > tExit) {
                return std::nullopt;
            }
        }
        return tEntry;
    }

    Entity PickEntity(std::span<const RelativeTransform> transforms, const Ray& ray, const MeshBoundsFunction& boundsOf) {
        Entity best = NullEntity;
        double bestDistance = std::numeric_limits<double>::max();
        for (const RelativeTransform& transform : transforms) {
            const std::optional<Math::Box3f> box = boundsOf(transform.entity);
            if (!box) {
                continue;
            }
            const std::optional<double> distance = IntersectBox(ray, transform, *box);
            if (distance && *distance < bestDistance) {
                bestDistance = *distance;
                best = transform.entity;
            }
        }
        return best;
    }

    std::optional<double> ClosestPointOnLine(const Ray& ray, const Vector3d& point, const Vector3d& direction) {
        // Minimise |ray.origin + s * ray.direction - point - t * direction|, les deux directions unitaires.
        const Vector3d w = ray.origin - point;
        const double b = Math::Dot(ray.direction, direction);
        const double denominator = 1.0 - b * b;
        if (denominator < 1.0e-6) {
            return std::nullopt;
        }
        return (Math::Dot(direction, w) - b * Math::Dot(ray.direction, w)) / denominator;
    }
}
