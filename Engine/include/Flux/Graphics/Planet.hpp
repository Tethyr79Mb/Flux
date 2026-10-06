#pragma once

#include "Flux/Assets/Model.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Vector3.hpp"

#include <array>
#include <cstdint>
#include <vector>

// Planète en cube-sphère : un cube dont chaque face est une grille, poussée sur la sphère. Chaque face est un arbre
// de tuiles (quadtree) : une tuile de niveau n se coupe en 4 de niveau n + 1, de plus en plus fines près de la caméra.
// Tout est calculé en double dans le repère de la planète (centre à l'origine) ; une tuile garde ses sommets en float
// relatifs à son centre, qui ne dépasse pas quelques kilomètres : précision au millimètre même loin du centre.
// Recherche et choix : /mnt/project-files/Recherche/cube-sphere-planetes.md.
namespace Flux::Planet {
    // Quadrillage d'une tuile (de côté), le même à tous les niveaux.
    constexpr std::uint32_t TileQuads = 32;
    constexpr std::uint32_t TileSide = TileQuads + 1;
    // Sommets d'une tuile : la grille, puis une jupe (copie de chaque sommet du bord, descendue sous le terrain) qui
    // cache les fentes avec une voisine d'un autre niveau.
    constexpr std::uint32_t TileVertexCount = TileSide * TileSide + 4 * TileSide;
    // Niveau le plus fin possible (au-delà, les coordonnées de la grille ne tiennent plus sur 32 bits).
    constexpr std::uint32_t MaxLevel = 24;

    // Une tuile : sa face (0 à 5 : +X, -X, +Y, -Y, +Z, -Z), son niveau, sa place dans la face (0 à 2^niveau - 1).
    struct TileKey
    {
        std::uint8_t face = 0;
        std::uint8_t level = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;

        constexpr bool operator==(const TileKey&) const = default;
        // Clé unique (5 bits de niveau, 3 de face, 28 + 28 de position).
        constexpr std::uint64_t Pack() const {
            return (static_cast<std::uint64_t>(level) << 59) | (static_cast<std::uint64_t>(face) << 56)
                 | (static_cast<std::uint64_t>(x) << 28) | static_cast<std::uint64_t>(y);
        }
        // Enfant 0 à 3 : (x, y) puis (x + 1, y), (x, y + 1), (x + 1, y + 1) au niveau suivant.
        constexpr TileKey Child(std::uint32_t index) const {
            return {face, static_cast<std::uint8_t>(level + 1), 2 * x + (index & 1u), 2 * y + (index >> 1)};
        }
    };

    // Direction (unitaire) du point (s, t) de la face, s et t de -1 à 1. Projection « ajustement tangent » : le point
    // du cube est (tan(s.π/4), tan(t.π/4)), ce qui égalise les mailles (au plus 20 % d'écart d'aire, contre 70 % en
    // normalisant le cube tel quel) et garde un inverse exact (FaceFromDirection).
    FLUX_API Math::Vector3d DirectionFromFace(std::uint32_t face, double s, double t);
    // Inverse : la face où tombe la direction et ses coordonnées (s, t) de -1 à 1.
    FLUX_API void FaceFromDirection(const Math::Vector3d& direction, std::uint32_t& face, double& s, double& t);

    // Forme du terrain : rayon, relief (bruit fractal évalué sur la direction 3D, donc sans couture entre les faces),
    // le même pour une même graine.
    class FLUX_API Terrain {
        public:
        Terrain() : Terrain(1000.0, 0.0, 1) {}
        Terrain(double radius, double relief, std::int32_t seed);

        double GetRadius() const { return m_radius; }
        double GetRelief() const { return m_relief; }
        // Niveau le plus fin utile : environ un mètre et demi entre deux sommets.
        std::uint32_t GetMaxLevel() const { return m_maxLevel; }
        // Octaves du bruit pour une tuile de ce niveau : celles dont le détail est plus grand que l'écart de ses
        // sommets (les plus fines ne se verraient pas et coûteraient cher).
        std::uint32_t GetOctaves(std::uint32_t level) const;
        // Hauteur au-dessus du rayon (m, de -relief à +relief environ) dans cette direction (unitaire).
        double GetHeight(const Math::Vector3d& direction, std::uint32_t octaves) const;
        // Relief que les octaves omises peuvent encore ajouter (m) : profondeur minimale des jupes.
        double GetOmittedRelief(std::uint32_t octaves) const;

        private:
        double Noise(const Math::Vector3d& p) const;

        double m_radius;
        double m_relief;
        std::uint32_t m_maxLevel;
        std::array<std::uint8_t, 512> m_permutation{};
    };

    // Sphère englobante (dans le repère de la planète) et longueur d'un bord d'une tuile, sans la générer.
    struct TileBounds
    {
        Math::Vector3d center;
        double radius = 0.0;
        double edge = 0.0;
    };
    FLUX_API TileBounds GetTileBounds(const Terrain& terrain, TileKey key);

    // Sommets d'une tuile, relatifs à `center` (son centre sur la sphère, repère de la planète), et le rayon de la
    // sphère qui les englobe, jupes comprises.
    struct TileMesh
    {
        Math::Vector3d center;
        double radius = 0.0;
        std::vector<ModelVertex> vertices; // TileVertexCount
    };
    FLUX_API void BuildTile(const Terrain& terrain, TileKey key, TileMesh& mesh);
    // Indices communs à toutes les tuiles (la grille, puis les jupes), dans le sens trigonométrique vu de dehors.
    FLUX_API std::vector<std::uint32_t> BuildTileIndices();
}
