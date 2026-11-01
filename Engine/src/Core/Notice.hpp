#pragma once

#include "Flux/Math/Vector2.hpp"

#include <string>

namespace Flux {
    namespace UI {
        class DrawList;
    }

    // Message court du moteur en haut de l'écran (« Partie sauvegardée »), quelques secondes.
    class Notice {
        public:
        void Show(std::string text);
        bool IsShown() const { return m_seconds > 0.0; }
        // loading : l'écran de chargement le cache, il attend la fin pour décompter.
        void Draw(UI::DrawList& list, Math::Vector2f screen, double deltaSeconds, bool loading);

        private:
        std::string m_text;
        double m_seconds = 0.0;
    };
}
