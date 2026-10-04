#pragma once

#include <Flux/UI/Context.hpp>

#include <filesystem>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Fenêtre « Paramètres » (Fichier > Paramètres...), un onglet par sujet. Les réglages de l'éditeur (thème, volume)
    // sont enregistrés à côté de FluxEditor.exe et relus au démarrage.
    class SettingsWindow {
        public:
        // Relit les réglages enregistrés et les applique (thème de l'interface, volume du son).
        void Load(Flux::Engine& engine);
        void Open() { m_open = true; }
        void Draw(Flux::Engine& engine);

        private:
        enum class Tab { Graphics, Sound, Interface };

        void Save() const;
        static std::filesystem::path FilePath();

        bool m_open = false;
        Tab m_tab = Tab::Graphics;
        Flux::UI::Theme m_theme = Flux::UI::Theme::Dark;
        float m_volume = 1.0f; // volume général, 0 à 1
        bool m_volumeChanged = false; // pas encore enregistré (curseur tenu)
    };
}
