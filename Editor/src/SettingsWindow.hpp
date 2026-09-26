#pragma once

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Fenêtre « Paramètres » (Fichier > Paramètres...), un onglet par thème.
    class SettingsWindow {
        public:
        void Open() { m_open = true; }
        void Draw(Flux::Engine& engine);

        private:
        enum class Tab { Graphics };

        bool m_open = false;
        Tab m_tab = Tab::Graphics;
    };
}
