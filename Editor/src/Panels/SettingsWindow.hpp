#pragma once

#include <Flux/Renderer/Renderer.hpp>
#include <Flux/UI/Context.hpp>

#include <filesystem>
#include <string>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Fenêtre « Paramètres » (Fichier > Paramètres...) : catégories à gauche, réglages groupés à droite, une aide sous
    // chacun. Les réglages de l'éditeur (thème, taille de l'interface, volume, graphismes) sont enregistrés à côté de
    // FluxEditor.exe et relus au démarrage.
    class SettingsWindow {
        public:
        // Relit les réglages enregistrés et les applique (thème et taille de l'interface, volume du son, graphismes).
        void Load(Flux::Engine& engine);
        void Open() { m_open = true; }
        void Draw(Flux::Engine& engine);

        private:
        enum class Tab { Graphics, Sound, Keys, Interface, System };

        void DrawGraphics(Flux::Engine& engine);
        void DrawSound(Flux::Engine& engine);
        void DrawKeys(Flux::Engine& engine);
        void DrawInterface(Flux::Engine& engine);
        void DrawSystem(Flux::Engine& engine);
        // Donne au moteur les graphismes choisis ici (préréglage, valeurs par défaut).
        void ApplyGraphics(Flux::Engine& engine) const;
        void Save() const;
        void ApplyStyle(Flux::Engine& engine) const;
        static std::filesystem::path FilePath();

        bool m_open = false;
        Tab m_tab = Tab::Graphics;
        std::string m_keySearch; // filtre des actions de l'onglet Touches
        Flux::UI::Theme m_theme = Flux::UI::Theme::Dark;
        float m_uiScale = 1.0f; // taille de l'interface, x celle d'origine
        float m_volume = 1.0f; // volume général, 0 à 1
        bool m_volumeChanged = false; // pas encore enregistré (curseur tenu)
        bool m_bloomEnabled = true;
        float m_bloom = Flux::Renderer::DefaultBloom; // force quand il est activé, 0 à 1
        bool m_bloomChanged = false; // pas encore enregistré (curseur tenu)
        bool m_temporalAA = true;
        Flux::Upscaling m_upscaling = Flux::DefaultUpscaling;
        float m_sharpness = Flux::DefaultSharpness;
        bool m_sharpnessChanged = false; // pas encore enregistré (curseur tenu)
        Flux::GroundDetail m_groundDetail = Flux::DefaultGroundDetail;
        Flux::CloudQuality m_cloudQuality = Flux::DefaultCloudQuality;
        Flux::VegetationDistance m_vegetationDistance = Flux::DefaultVegetationDistance;
    };
}
