#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Vector3.hpp"

#include <array>
#include <cstdint>

namespace Flux {
    struct Camera;

    // Ombres du soleil en cascades : la vue est découpée en tranches de plus en plus lointaines, chacune couverte par
    // sa propre carte d'ombres (même résolution) : précises tout près, grossières au loin.
    inline constexpr int ShadowCascadeCount = 4;
    // Fin de chaque tranche, en mètres devant la caméra ; au-delà de la dernière, pas d'ombre.
    inline constexpr std::array<float, ShadowCascadeCount> ShadowCascadeEnds{6.0f, 20.0f, 60.0f, 200.0f};
    // Distance vers le soleil jusqu'où un obstacle fait encore de l'ombre dans une tranche, en mètres.
    inline constexpr float ShadowCasterDistance = 300.0f;
    // Profondeur des cartes proches vers le soleil, en avant de chaque tranche : la pénombre cherche les obstacles
    // jusqu'à 500 m d'un point (NearMaxBlocker, Mesh.slang), à 250 m au plus du centre de la dernière carte. Plus
    // près (300 m), une tour à 320 m était aplatie devant la carte : sa distance, donc la largeur de sa pénombre,
    // dépendait de la place de la carte, c'est-à-dire du regard.
    inline constexpr float ShadowDepthDistance = 800.0f;
    // Les objets (pas le sol des planètes, qu'ont les cartes du relief) font de l'ombre de plus loin encore : aplatis
    // devant tout dans la carte (PipelineDesc::depthClamp), sans coûter de précision à sa profondeur. Une tour, une
    // station ou un grand vaisseau sous un soleil rasant porte son ombre à des centaines de mètres.
    inline constexpr float ShadowFlattenDistance = 3000.0f;

    // Ombres du relief au loin : deux tranches de plus, jusqu'à 3 km puis 40 km, dans leur propre carte où seul le
    // sol des planètes est dessiné. Une montagne de 2 km sous un soleil bas porte son ombre sur des dizaines de
    // kilomètres : d'où la distance des obstacles.
    inline constexpr int TerrainShadowCascadeCount = 2;
    inline constexpr std::array<float, TerrainShadowCascadeCount> TerrainShadowCascadeEnds{3000.0f, 40000.0f};
    inline constexpr float TerrainShadowCasterDistance = 30000.0f;

    struct ShadowCascade
    {
        // Position relative à la caméra -> x, y dans [-1, 1] sur la carte, profondeur dans [0, 1] (0 : côté soleil).
        Math::Matrix4f lightViewProjection;
        Math::Vector3f center; // relatif à la caméra
        float radius = 0.0f;   // demi-côté du carré couvert, en mètres
        float texelSize = 0.0f; // côté d'un texel de la carte, en mètres
        float depthRange = 0.0f; // profondeur couverte le long des rayons du soleil, en mètres
    };

    // Cascades de la caméra pour un soleil dans la direction `toSun` (unitaire, vers le soleil), cartes de
    // `resolution` texels de côté. Chaque tranche est enfermée dans une sphère (taille fixe quand la caméra tourne) et
    // son centre est calé sur la grille des texels du monde : les bords d'ombre ne scintillent pas quand la caméra
    // bouge. Les obstacles jusqu'à `casterDistance` mètres vers le soleil projettent leur ombre dans la tranche.
    FLUX_API std::array<ShadowCascade, ShadowCascadeCount> ComputeShadowCascades(const Camera& camera, float aspect,
                                                                                 const Math::Vector3f& toSun,
                                                                                 std::uint32_t resolution,
                                                                                 float casterDistance = ShadowCasterDistance);

    // Cascades du relief au loin : chacune couvre la sphère autour de la caméra de rayon sa fin (TerrainShadowCascadeEnds)
    // x `margin`, quelle que soit la direction du regard. La marge (1,1 : 10 %) laisse la carte, gardée plusieurs
    // images, couvrir encore tout quand la caméra a un peu bougé.
    FLUX_API std::array<ShadowCascade, TerrainShadowCascadeCount> ComputeTerrainShadowCascades(const Camera& camera, float aspect,
                                                                                               const Math::Vector3f& toSun,
                                                                                               std::uint32_t resolution,
                                                                                               float margin = 1.1f);

    // Ombres des forêts au loin (MeshRenderer::DrawForestShadows) : une tranche de plus, de la fin des cascades proches
    // jusqu'à la portée des arbres, dans sa propre carte où seuls les arbres sont dessinés, refaite à chaque image.
    inline constexpr float ForestShadowCasterDistance = 300.0f;

    // Une cascade centrée sur la caméra, de rayon `radius` (mètres) : elle ne dépend que de sa position, pas de son
    // regard (forêts au loin : une ombre qui changeait d'intensité en tournant la tête).
    FLUX_API ShadowCascade ComputeShadowAround(const Camera& camera, const Math::Vector3f& toSun, float radius,
                                               std::uint32_t resolution, float casterDistance);

    // La même cascade (même carte) vue d'une caméra déplacée de `cameraOffset` (nouvelle position - ancienne, en
    // mètres) : sa matrice prend les positions relatives à la nouvelle caméra. Sert à garder la carte d'une image
    // précédente.
    FLUX_API ShadowCascade MoveShadowCascade(const ShadowCascade& cascade, const Math::Vector3f& cameraOffset);

    // Ce qui reçoit les ombres : le champ de la caméra, découpé en ses tranches. La carte d'une cascade couvre une
    // sphère autour de sa tranche (derrière la caméra compris) : un obstacle peut y être sans que son ombre tombe sur
    // rien de visible. Celui-là n'a pas à être dessiné dans la carte.
    class FLUX_API ShadowReceivers {
        public:
        ShadowReceivers() = default;
        ShadowReceivers(const Camera& camera, float aspect, const Math::Vector3f& toSun);

        // L'ombre de la sphère (relative à la caméra), prolongée de `length` mètres à l'opposé du soleil, peut tomber
        // dans le champ entre la caméra et la fin de la tranche `cascade` (les tranches plus proches filtrent leurs
        // pénombres larges dans cette cascade). Prudent : un obstacle gardé à tort coûte un peu, écarté à tort, une
        // ombre manque.
        bool Reaches(const Math::Vector3f& center, float radius, float length, std::size_t cascade) const;
        // Pareil pour la tranche [start, stop] (mètres devant la caméra), celles du relief au loin par exemple.
        bool ReachesRange(const Math::Vector3f& center, float radius, float length, float start, float stop) const;

        // Pour le GPU (shaders/Vegetation.slang) : normale vers l'extérieur de chaque bord du champ, passant par la
        // caméra ; direction de la caméra ; vers le soleil.
        const std::array<Math::Vector3f, 4>& GetSides() const { return m_sides; }
        const Math::Vector3f& GetForward() const { return m_forward; }
        const Math::Vector3f& GetToSun() const { return m_toSun; }

        private:
        std::array<Math::Vector3f, 4> m_sides{};
        Math::Vector3f m_forward{0.0f, 0.0f, -1.0f};
        Math::Vector3f m_toSun{0.0f, 1.0f, 0.0f};
    };

    // Début de la tranche `cascade`, en mètres devant la caméra (0 pour la première).
    inline float ShadowCascadeStart(std::size_t cascade) {
        return cascade == 0 ? 0.0f : ShadowCascadeEnds[cascade - 1];
    }

    // Début de la tranche du relief `cascade`, en mètres devant la caméra. La première part de la caméra : les cascades
    // proches ne voient pas les obstacles à plus de ShadowCasterDistance, une montagne à 1 km vers le soleil ne ferait
    // pas d'ombre sur le sol à ses pieds (SunVisibility y garde le plus sombre des deux).
    inline float TerrainShadowCascadeStart(std::size_t cascade) {
        return cascade == 0 ? 0.0f : TerrainShadowCascadeEnds[cascade - 1];
    }
}
