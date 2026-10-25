#pragma once

// Écran de chargement après chaque scène chargée en jeu (EngineConfig::loadingScreen) : la scène reste cachée et le
// jeu attend tant que le sol des planètes, ses textures, les modèles et les polices ne sont pas prêts.

#include "Flux/Math/Vector2.hpp"

#include <cstdint>

namespace Flux {
    struct RenderTimings;

    namespace UI {
        class DrawList;
    }

    class LoadingScreen {
        public:
        explicit LoadingScreen(bool enabled) : m_enabled(enabled) {}

        // Scène chargée en jeu : écran affiché jusqu'à ce que plus rien ne soit attendu (sans effet s'il est coupé).
        void Begin();
        // Après le rendu : le chargement est fini quand plus rien n'est attendu pendant quelques images.
        void Update(const RenderTimings& render, double deltaSeconds);
        // Par-dessus la scène : texte et barre d'avancement.
        void Draw(UI::DrawList& list, const RenderTimings& render, Math::Vector2f screen) const;
        bool IsActive() const { return m_active; }

        private:
        bool m_enabled = true;
        bool m_active = false;
        // Depuis combien de temps, images de suite sans rien à attendre, et le plus d'attentes vues (pour la barre
        // d'avancement).
        double m_seconds = 0.0;
        std::uint32_t m_quietFrames = 0;
        std::uint32_t m_mostPending = 0;
    };
}
