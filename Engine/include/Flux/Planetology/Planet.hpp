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
namespace Flux::Planetology {
    // Quadrillage d'une tuile (de côté), le même à tous les niveaux.
    constexpr std::uint32_t TileQuads = 32;
    constexpr std::uint32_t TileSide = TileQuads + 1;
    // Sommets d'une tuile : la grille, puis une jupe (copie de chaque sommet du bord, descendue sous le terrain) qui
    // cache les fentes avec une voisine d'un autre niveau.
    constexpr std::uint32_t TileVertexCount = TileSide * TileSide + 4 * TileSide;
    // Paquets d'une tuile pour les mesh shaders (shaders/PlanetMesh.slang) : 4 x 4 carrés de 8 x 8 mailles, chacun
    // avec ses bouts de jupe ; un quart de tuile (TileKey::Child) en couvre 2 x 2.
    constexpr std::uint32_t TileMeshletQuads = 8;
    constexpr std::uint32_t TileMeshletSide = TileQuads / TileMeshletQuads;
    constexpr std::uint32_t TileMeshletCount = TileMeshletSide * TileMeshletSide;
    // Niveau le plus fin possible (au-delà, les coordonnées de la grille ne tiennent plus sur 32 bits).
    constexpr std::uint32_t MaxLevel = 24;
    // Fraction de la distance de coupe de la parente où une tuile commence à glisser vers la forme de sa parente.
    constexpr double MorphStart = 0.7;

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
        static constexpr TileKey Unpack(std::uint64_t packed) {
            return {static_cast<std::uint8_t>((packed >> 56) & 7u), static_cast<std::uint8_t>(packed >> 59),
                    static_cast<std::uint32_t>((packed >> 28) & 0xFFFFFFFu), static_cast<std::uint32_t>(packed & 0xFFFFFFFu)};
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
        // mountainWidth : largeur des montagnes en mètres (deux sommets voisins sont à environ largeur / 6), 0 : à l'échelle du rayon
        // (environ un rayon).
        Terrain(double radius, double relief, std::int32_t seed, double mountainWidth = 0.0);

        double GetRadius() const { return m_radius; }
        double GetRelief() const { return m_relief; }
        // Niveau le plus fin utile : environ un mètre et demi entre deux sommets.
        std::uint32_t GetMaxLevel() const { return m_maxLevel; }
        // Octaves du bruit pour une tuile de ce niveau : celles dont le détail est plus grand que l'écart de ses
        // sommets (les plus fines ne se verraient pas et coûteraient cher).
        std::uint32_t GetOctaves(std::uint32_t level) const;
        // Zones du relief en un point (0 à 1), d'où les biomes de la végétation tirent leurs conditions : collines, et
        // montagnes avec leurs piémonts (plus larges que les chaînes elles-mêmes). Le reste est de la plaine.
        struct Zones
        {
            double hills = 0.0;
            double mountains = 0.0;
        };
        // Hauteur au-dessus du rayon (m, de -relief à +relief environ) dans cette direction (unitaire) ; zones : celles
        // du point, si demandées.
        double GetHeight(const Math::Vector3d& direction, std::uint32_t octaves, Zones* zones = nullptr) const;
        // Relief que les octaves omises peuvent encore ajouter (m) : profondeur minimale des jupes.
        double GetOmittedRelief(std::uint32_t octaves) const;
        // Table de permutation du bruit (tirée de la graine).
        const std::array<std::uint8_t, 512>& GetPermutation() const { return m_permutation; }
        // Fréquence en plus des couches qui suivent la largeur des montagnes (1 : aucune), et le nombre d'octaves
        // qu'elle décale (log2 arrondi au-dessus).
        double GetScale() const { return m_scale; }
        std::uint32_t GetScaleShift() const { return m_shift; }

        private:
        // Bruit de Perlin et, si demandée, sa pente (par rapport à p ; nulle sinon).
        struct Sample
        {
            double value;
            Math::Vector3d gradient;
        };
        Sample Noise(const Math::Vector3d& p, bool withGradient) const;
        double Fbm(const Math::Vector3d& point, const Math::Vector3d& direction, std::uint32_t layer, std::uint32_t octaves,
                   double slopeScale) const;
        // Sommets des montagnes autour de `point` : leur hauteur (0 à 1) et sa pente `slope` (par radian, tangente).
        double Peaks(const Math::Vector3d& point, const Math::Vector3d& direction, std::uint32_t octaves, Math::Vector3d& slope) const;
        // Ravines sur une pente `slope` (m/m, tangente), d'amplitude `amount` (zone de montagne, 0 à 1) : fraction du
        // relief à ajouter.
        double Gullies(const Math::Vector3d& point, const Math::Vector3d& direction, std::uint32_t octaves,
                       Math::Vector3d slope, double amount) const;
        // Déplacement (radians, tangent à la sphère) du point où se lisent les couches déformées.
        Math::Vector3d Warp(const Math::Vector3d& direction, std::uint32_t layer, double amount) const;

        double m_radius;
        double m_relief;
        std::uint32_t m_maxLevel;
        double m_scale = 1.0;
        std::uint32_t m_shift = 0;
        std::array<std::uint8_t, 512> m_permutation{};
    };

    // Distance (de la caméra à la sphère englobante d'une tuile) en deçà de laquelle une tuile de ce niveau est coupée
    // en 4. La même pour toutes les tuiles d'un niveau (la projection tangente donne le même angle à toutes les
    // tuiles le long des axes d'une face) : deux voisines changent de niveau à la même distance, ce qui permet le
    // glissement d'une forme à l'autre sans fente. `splitFactor` : GetSplitFactor (Flux/Planetology/GroundDetail.hpp).
    FLUX_API double GetSplitDistance(const Terrain& terrain, std::uint32_t level, double splitFactor);

    // Sphère englobante (dans le repère de la planète) et longueur d'un bord d'une tuile, sans la générer.
    struct TileBounds
    {
        // Point de référence des sommets : au rayon de la planète, au-dessus du milieu de la tuile.
        Math::Vector3d origin;
        // Sphère englobante, centrée entre le plus bas et le plus haut : une tuile de 30 m posée à 900 m d'altitude
        // a un rayon de quelques dizaines de mètres, pas de 900 (centre au rayon de la planète).
        Math::Vector3d center;
        double radius = 0.0;
        double edge = 0.0;
    };
    // Sans hauteurs connues : du plus bas au plus haut de tout le relief de la planète (très prudent pour une petite
    // tuile). lowest, highest : distances au centre de la planète entre lesquelles est sa surface (les jupes, plus
    // bas, sont ajoutées).
    FLUX_API TileBounds GetTileBounds(const Terrain& terrain, TileKey key);
    FLUX_API TileBounds GetTileBounds(const Terrain& terrain, TileKey key, double lowest, double highest);

    // Sommets d'une tuile, relatifs à `center` (son centre sur la sphère, repère de la planète), et le rayon de la
    // sphère qui les englobe, jupes comprises.
    // Où va un sommet quand sa tuile prend la forme de sa parente (moins détaillée) : au même endroit que le sommet de
    // la parente s'il en a un, sinon sur le bord du triangle de la parente. Envoyé au GPU à côté de ModelVertex.
    struct MorphVertex
    {
        Math::Vector3f offset; // de la position du sommet à celle de la parente
        Math::Vector3f normal; // normale de la parente à cet endroit
    };

    struct TileMesh
    {
        Math::Vector3d center;
        double radius = 0.0;                // englobe aussi la forme de la parente
        double lowest = 0.0;                // plus petite distance au centre de la planète (sa forme et celle de la parente) : l'océan la couvre-t-il ?
        std::vector<ModelVertex> vertices;  // TileVertexCount
        std::vector<MorphVertex> morph;     // TileVertexCount ; niveau 0 : aucun glissement
    };
    // Référence sur le CPU (en double) de la tuile que le GPU calcule pour l'affichage (shaders/PlanetTile.slang) ;
    // les tests vérifient que les deux donnent le même terrain.
    FLUX_API void BuildTile(const Terrain& terrain, TileKey key, TileMesh& mesh);

    // Ce que le GPU reçoit pour calculer une tuile en simple précision (TileInputs dans shaders/PlanetTile.slang) : ce
    // qui demande du double est fait ici. Un sommet est repéré par rapport au centre de la tuile (direction d_c) :
    // d = d_c + δ, où δ se calcule sans soustraire deux grands nombres voisins grâce aux colonnes et lignes ci-dessous ;
    // et chaque octave du bruit part de son point au centre de la tuile (coordonnée entière modulo 256 du réseau,
    // plus fraction), auquel le GPU ajoute δ x fréquence. Ainsi le float garde le millimètre même pour l'octave la plus
    // fine d'une planète de la taille de la Terre.
    struct TileGpuInputs
    {
        static constexpr std::uint32_t Columns = TileSide + 4; // décalages -2 à TileQuads + 2 de la grille de la face
        static constexpr std::uint32_t Octaves = 43;            // somme des octaves de chaque couche du relief

        std::array<float, 4> center{}; // xyz : d_c ; w : |v_c| (v_c = normale + a_c.s + b_c.t, avant normalisation)
        std::array<float, 4> centerLow{}; // xyz : d_c - center.xyz (le reste du double, pour la variante en double)
        std::array<float, 4> axisN{};  // xyz : normale de la face ; w : rayon
        std::array<float, 4> axisS{};  // xyz : axe s ; w : relief
        std::array<float, 4> axisT{};  // xyz : axe t ; w : profondeur des jupes
        std::array<std::uint32_t, 4> options{}; // x : octaves ; y : octaves de la parente ; z : niveau ; w : GetScaleShift
        // Colonne i (x : a - a_c, y : a + a_c, a = tan(s.π/4)) et ligne i (z : b - b_c, w : b + b_c), décalage i - 2.
        std::array<std::array<float, 4>, Columns> columns{};
        std::array<std::array<std::int32_t, 4>, Octaves> cells{}; // xyz : point du réseau (0 à 255)
        std::array<std::array<float, 4>, Octaves> fractions{};    // xyz : position dans la maille ; w : fréquence
        std::array<std::array<float, 4>, Octaves> fractionsLow{}; // reste de fractions (fractions + fractionsLow)
        std::array<std::uint32_t, 128> permutation{};             // 512 octets, 4 par entier
    };
    static_assert(sizeof(TileGpuInputs) == 96 + 16 * (TileGpuInputs::Columns + 3 * TileGpuInputs::Octaves) + 512);
    FLUX_API void PrepareTileGpu(const Terrain& terrain, TileKey key, TileGpuInputs& inputs);

    // Surface seule d'une tuile, pour la collision : ses TileSide x TileSide sommets (sans jupe, normales ni
    // glissement), relatifs à `center`, identiques à ceux de BuildTile (le sol touché est celui qu'on voit de près).
    FLUX_API void BuildTileSurface(const Terrain& terrain, TileKey key, Math::Vector3d& center, std::vector<Math::Vector3f>& positions);
    // Tuile de ce niveau où tombe une direction (unitaire).
    FLUX_API TileKey FindTile(const Math::Vector3d& direction, std::uint32_t level);
    // Indices communs à toutes les tuiles, dans le sens trigonométrique vu de dehors, rangés par quart de tuile : ceux de
    // l'enfant 0 (sa part de la grille, puis de ses jupes), puis de l'enfant 1, 2, 3 (TileKey::Child). Chaque quart en
    // compte TileQuarterIndices.
    constexpr std::uint32_t TileQuarterIndices = 6 * (TileQuads / 2) * (TileQuads / 2) + 2 * 6 * (TileQuads / 2);
    FLUX_API std::vector<std::uint32_t> BuildTileIndices();
}
