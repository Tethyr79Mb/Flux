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

    // Ombres du relief au loin : deux tranches de plus, après la dernière cascade, dans leur propre carte où seul le
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

    // Cascades du relief au loin (TerrainShadowCascadeEnds, la première commence à la fin de la dernière cascade).
    // `margin` agrandit le carré couvert (1,1 : 10 %) : la carte, gardée plusieurs images, couvre encore la vue quand
    // la caméra a un peu bougé ou tourné.
    FLUX_API std::array<ShadowCascade, TerrainShadowCascadeCount> ComputeTerrainShadowCascades(const Camera& camera, float aspect,
                                                                                               const Math::Vector3f& toSun,
                                                                                               std::uint32_t resolution,
                                                                                               float margin = 1.1f);

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
        // dans la tranche `cascade` du champ. Prudent : un obstacle gardé à tort coûte un peu, écarté à tort, une
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

    // Début de la tranche du relief `cascade`, en mètres devant la caméra.
    inline float TerrainShadowCascadeStart(std::size_t cascade) {
        return cascade == 0 ? ShadowCascadeEnds[ShadowCascadeCount - 1] : TerrainShadowCascadeEnds[cascade - 1];
    }
}
