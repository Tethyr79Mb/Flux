#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/Scene.hpp"

#include <optional>
#include <span>

namespace Flux {
    struct Camera;

    // Demi-droite dans l'espace relatif à la caméra (celui de RelativeTransform) ; direction unitaire.
    struct Ray
    {
        Math::Vector3d origin;
        Math::Vector3d direction;
    };

    // Passage entre les pixels de la vue où la scène est dessinée et l'espace relatif à la caméra,
    // avec la même perspective que le rendu. En double : valable aussi loin que les positions.
    class FLUX_API ScreenProjection {
        public:
        ScreenProjection(const Camera& camera, Math::Vector2f viewMin, Math::Vector2f viewMax);

        // Rayon partant de la caméra et passant par ce pixel.
        Ray GetRay(Math::Vector2f pixel) const;
        // Distance devant la caméra, le long de son axe de visée (négative derrière elle).
        double GetDepth(const Math::Vector3d& point) const;
        // Pixel où se projette un point ; nullopt s'il est derrière le plan proche.
        std::optional<Math::Vector2f> ToScreen(const Math::Vector3d& point) const;
        // Projette un segment, coupé au plan proche ; false s'il est entièrement derrière.
        bool ProjectSegment(Math::Vector3d a, Math::Vector3d b, Math::Vector2f& screenA, Math::Vector2f& screenB) const;
        // Longueur en mètres qui mesure ce nombre de pixels à l'écran, à cette profondeur (gizmos de taille fixe).
        double PixelsToMeters(double pixels, double depth) const;

        private:
        Math::Vector3d m_right;
        Math::Vector3d m_up;
        Math::Vector3d m_forward;
        double m_near;
        double m_focal; // pixels : demi-hauteur de la vue / tan(demi-champ vertical)
        Math::Vector2f m_center;
    };

    // Distance le long du rayon jusqu'au cube unité centré (le maillage Cube) placé par transform ;
    // nullopt s'il le manque. Un rayon qui part de l'intérieur touche à 0.
    FLUX_API std::optional<double> IntersectCube(const Ray& ray, const RelativeTransform& transform);

    // Entité visible (MeshRendererComponent) la plus proche touchée par le rayon ; NullEntity si aucune.
    // transforms : ceux de Scene::ComputeRelativeTransforms, relatifs à l'origine du rayon.
    FLUX_API Entity PickEntity(const Scene& scene, std::span<const RelativeTransform> transforms, const Ray& ray);

    // Paramètre t du point de la droite point + t * direction (unitaire) le plus proche du rayon ;
    // nullopt si la droite est presque parallèle au rayon (le point le plus proche file à l'infini).
    FLUX_API std::optional<double> ClosestPointOnLine(const Ray& ray, const Math::Vector3d& point,
                                                      const Math::Vector3d& direction);
}
