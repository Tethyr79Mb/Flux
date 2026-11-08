#pragma once

#include <algorithm>
#include <cstdint>

namespace Flux {
    // Horloge de la simulation : le jeu avance par pas constants, quelle que soit la cadence d'affichage. Le même pas
    // sur le client et le serveur rend leurs simulations comparables (prédiction, réconciliation) ; le rendu
    // interpole entre les deux derniers pas (GetAlpha).
    class SimulationClock {
        public:
        static constexpr double StepSeconds = 1.0 / 60.0;
        // Au-delà, le temps en trop est abandonné : sur une machine trop lente, le jeu ralentit au lieu de
        // s'enliser dans des pas de rattrapage qui coûtent eux-mêmes une image.
        static constexpr int MaxStepsPerFrame = 4;

        // Ajoute le temps écoulé (0 en pause) ; renvoie le nombre de pas à simuler maintenant (0 à MaxStepsPerFrame).
        int Advance(double seconds) {
            m_accumulator = std::min(m_accumulator + std::max(seconds, 0.0), StepSeconds * MaxStepsPerFrame);
            const int steps = static_cast<int>(m_accumulator / StepSeconds);
            m_accumulator -= steps * StepSeconds;
            m_step += static_cast<std::uint64_t>(steps);
            return steps;
        }

        // Part du pas suivant déjà écoulée, de 0 à 1 : où dessiner entre l'avant-dernier pas et le dernier.
        double GetAlpha() const { return std::clamp(m_accumulator / StepSeconds, 0.0, 1.0); }
        // Pas simulés depuis Reset : l'heure commune du client et du serveur.
        std::uint64_t GetStep() const { return m_step; }

        void Reset() {
            m_accumulator = 0.0;
            m_step = 0;
        }

        private:
        double m_accumulator = 0.0;
        std::uint64_t m_step = 0;
    };
}
