#pragma once

#include "Flux/Math/Vector3.hpp"
#include "Flux/SimExport.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Flux::Planetology {
    // Vagues de l'océan (Tessendorf 2001, « Simulating Ocean Water ») : la mer est une somme d'ondes de toutes tailles
    // et de toutes directions, tirées d'un spectre (JONSWAP : mer levée par un vent qui souffle sur `fetch` mètres,
    // Hasselmann 1973), chacune avançant à sa vitesse (eau profonde : omega² = g k). Le GPU en fait la somme à chaque
    // image par FFT (Engine/src/Planetology/OceanWaves, shaders/OceanWaves.slang) ; ici, le spectre de départ, calculé
    // une fois par réglage et envoyé tel quel.
    //
    // Trois cascades : trois carrés de OceanWaveSize² ondes qui se répètent sur OceanCascadeLengths mètres. La première
    // porte la houle, la dernière les vaguelettes ; chaque onde n'est que dans une cascade (OceanCascadeBand), et les
    // longueurs ne sont pas multiples l'une de l'autre, pour que la répétition ne se voie pas.
    struct OceanWaveSettings
    {
        float windSpeed = 8.0f;     // m/s, à 10 m au-dessus de l'eau (8 : mer agitée, vagues de 1 m environ)
        float windDirection = 0.0f; // degrés, vers où souffle le vent, depuis l'axe x du plan de l'eau
        float fetch = 100000.0f;    // m de mer sur lesquels le vent a soufflé : plus long, des vagues plus hautes et longues
        float spread = 1.0f;        // dispersion des directions autour du vent (0,5 : houle régulière, 2 : mer croisée)
        float choppiness = 0.9f;    // crêtes pointues (déplacement horizontal, 0 : vagues rondes) ; au-delà de 1, repliées
        std::uint32_t seed = 0;

        bool operator==(const OceanWaveSettings&) const = default;
    };

    inline constexpr std::uint32_t OceanWaveSize = 256;
    inline constexpr std::uint32_t OceanCascadeCount = 3;
    inline constexpr std::array<float, OceanCascadeCount> OceanCascadeLengths{250.0f, 37.0f, 7.3f};
    inline constexpr float OceanGravity = 9.81f;
    // La mer se répète après cette durée (s) : vitesses des ondes arrondies (shaders/OceanWaves.slang).
    inline constexpr float OceanRepeatPeriod = 200.0f;

    // Nombres d'onde (rad/m) gardés par la cascade : [x, y[.
    FLUX_SIM_API std::array<float, 2> OceanCascadeBand(std::uint32_t cascade);

    // Onde (m, n) d'une cascade (m, n de 0 à OceanWaveSize - 1) : vecteur d'onde k = 2 pi (m - N/2, n - N/2) / longueur,
    // dans le plan de l'eau (x, z).
    FLUX_SIM_API std::array<float, 2> OceanWaveVector(std::uint32_t cascade, std::uint32_t m, std::uint32_t n);

    // Vitesse angulaire (rad/s) de l'onde de nombre d'onde k (rad/m) : eau profonde, arrondie à OceanRepeatPeriod.
    FLUX_SIM_API float OceanAngularSpeed(float k);

    // Spectre de départ d'une cascade, OceanWaveSize² entrées, [n x taille + m] : h0(k) (réel, imaginaire), puis le
    // conjugué de h0(-k), en mètres. Hauteur au temps t : somme sur k de
    // (h0(k) e^(-i omega t) + conj(h0(-k)) e^(i omega t)) e^(i k.x), réelle (omega : OceanAngularSpeed).
    using OceanSpectrum = std::vector<std::array<float, 4>>;
    FLUX_SIM_API OceanSpectrum BuildOceanSpectrum(const OceanWaveSettings& settings, std::uint32_t cascade);

    // Écart type de la hauteur de l'eau (m) que donne le spectre de toutes les cascades ; les marins parlent de « hauteur
    // significative », quatre fois plus grande (le tiers des plus hautes vagues, du creux à la crête).
    FLUX_SIM_API float OceanHeightDeviation(const OceanWaveSettings& settings);

    // Plan de l'eau où se posent les cascades, tangent à la mer sous la caméra : les vagues sont des carrés qui se
    // répètent dans un plan, la mer une sphère. Le point d'ancrage suit la caméra (à la verticale, sur la mer) ; ses
    // coordonnées dans le plan avancent d'autant, en double, et ses axes sont transportés sans tourner sur eux-mêmes : un
    // point fixe de la planète, près de la caméra, garde les mêmes coordonnées d'une image à l'autre (les vagues ne
    // glissent pas quand on avance), même à des milliers de km de l'endroit où le plan a été posé.
    class FLUX_SIM_API OceanWavePlane {
        public:
        // La caméra a sauté plus loin que ça (m) en une image : plan reposé (téléportation, autre planète).
        static constexpr double ResetDistance = 50000.0;

        void Reset() { m_valid = false; }
        // `camera` : position de la caméra dans le repère de la planète (m) ; `seaRadius` : rayon de la mer (m).
        void Follow(const Math::Vector3d& camera, double seaRadius);

        bool IsValid() const { return m_valid; }
        Math::Vector3d GetAnchor() const { return m_up * m_radius; } // dans le repère de la planète
        const Math::Vector3d& GetUp() const { return m_up; }
        const Math::Vector3d& GetEast() const { return m_east; }   // axe x des cascades
        const Math::Vector3d& GetNorth() const { return m_north; } // axe z
        // Coordonnées (x, z) de l'ancrage dans le plan (m).
        const std::array<double, 2>& GetOffset() const { return m_offset; }
        // Coordonnées dans le plan d'un point de la planète proche de l'ancrage (ce que font les shaders de l'eau).
        std::array<double, 2> GetCoordinates(const Math::Vector3d& point) const;

        private:
        Math::Vector3d m_up{0.0, 1.0, 0.0};
        Math::Vector3d m_east{1.0, 0.0, 0.0};
        Math::Vector3d m_north{0.0, 0.0, -1.0};
        double m_radius = 0.0;
        std::array<double, 2> m_offset{};
        bool m_valid = false;
    };
}
