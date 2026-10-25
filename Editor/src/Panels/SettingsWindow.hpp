#pragma once

#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <filesystem>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Fenêtre « Paramètres » (Fichier > Paramètres...), un onglet par sujet. Les réglages de l'éditeur (thème, taille de
    // l'interface, volume, bloom, anticrénelage) sont enregistrés à côté de FluxEditor.exe et relus au démarrage.
    class SettingsWindow {
        public:
        // Relit les réglages enregistrés et les applique (thème et taille de l'interface, volume du son, bloom, anticrénelage).
        void Load(Flux::Engine& engine);
        void Open() { m_open = true; }
        void Draw(Flux::Engine& engine);

        private:
        enum class Tab { Graphics, Sound, Keys, Interface };

        void Save() const;
        void ApplyStyle(Flux::Engine& engine) const;
        static std::filesystem::path FilePath();

        bool m_open = false;
        Tab m_tab = Tab::Graphics;
        Flux::UI::Theme m_theme = Flux::UI::Theme::Dark;
        float m_uiScale = 1.0f; // taille de l'interface, x celle d'origine
        float m_volume = 1.0f; // volume général, 0 à 1
        bool m_volumeChanged = false; // pas encore enregistré (curseur tenu)
        bool m_bloomEnabled = true;
        float m_bloom = Flux::Renderer::DefaultBloom; // force quand il est activé, 0 à 1
        bool m_bloomChanged = false; // pas encore enregistré (curseur tenu)
        bool m_temporalAA = true;
        Flux::GroundDetail m_groundDetail = Flux::DefaultGroundDetail;
    };
}
