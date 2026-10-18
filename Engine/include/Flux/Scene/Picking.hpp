#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/Scene.hpp"

#include <functional>
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

    // Distance le long du rayon jusqu'à la boîte box (repère local ; par défaut le cube unité centré) placée par
    // transform ; nullopt s'il la manque. Un rayon qui part de l'intérieur touche à 0.
    FLUX_API std::optional<double> IntersectBox(const Ray& ray, const RelativeTransform& transform, const Math::Box3f& box = {});

    // Boîte locale du maillage d'une entité, nullopt si elle n'est pas dessinée en 3D (Engine::GetMeshBounds).
    using MeshBoundsFunction = std::function<std::optional<Math::Box3f>(Entity)>;

    // Entité dessinée la plus proche touchée par le rayon (par sa boîte englobante) ; NullEntity si aucune.
    // transforms : ceux de Scene::ComputeRelativeTransforms, relatifs à l'origine du rayon.
    FLUX_API Entity PickEntity(std::span<const RelativeTransform> transforms, const Ray& ray, const MeshBoundsFunction& boundsOf);

    // Encombrement d'un objet autour de son origine, dans les axes du monde.
    struct Extent
    {
        Math::Vector3d min;
        Math::Vector3d max;
    };

    // Encombrement des boîtes des maillages de parts (un objet et ses descendants : le bas d'un vaisseau, ce sont
    // ses roues) autour de origin, dans le même espace relatif que leurs transformations ; nullopt si aucune n'a de
    // boîte.
    FLUX_API std::optional<Extent> ComputeExtent(std::span<const RelativeTransform> parts, const Math::Vector3d& origin,
                                                 const MeshBoundsFunction& boundsOf);

    // Boîte sur laquelle un objet peut être posé (son maillage, placé par transform).
    struct GroundBox
    {
        RelativeTransform transform;
        Math::Box3f box;
    };

    // Hauteur y où mettre l'origine d'un objet d'encombrement extent, placé en origin, pour que son bas touche ce
    // qui est dessous : le dessus d'une boîte, ou le plan y = planeY s'il est donné. Cinq rayons verticaux (le
    // centre et les coins de l'encombrement, un peu rentrés) partent de la hauteur from ; le plus haut point touché
    // gagne. Une boîte d'où part un rayon ne compte pas pour lui : l'objet ne remonte pas sur le toit d'une pièce où
    // il se trouve. nullopt : rien dessous.
    FLUX_API std::optional<double> FindGroundHeight(std::span<const GroundBox> boxes, const Math::Vector3d& origin,
                                                    const Extent& extent, double from, std::optional<double> planeY);

    // Paramètre t du point de la droite point + t * direction (unitaire) le plus proche du rayon ;
    // nullopt si la droite est presque parallèle au rayon (le point le plus proche file à l'infini).
    FLUX_API std::optional<double> ClosestPointOnLine(const Ray& ray, const Math::Vector3d& point,
                                                      const Math::Vector3d& direction);
}
