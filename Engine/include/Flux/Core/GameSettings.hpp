#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Planetology/CloudQuality.hpp"
#include "Flux/Planetology/GroundDetail.hpp"
#include "Flux/Renderer/Upscaling.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    // Réglages du joueur pour Ars Solaris.exe, gardés dans Reglages.json à côté de l'exe (les touches restent dans
    // Touches.json, partagé avec l'éditeur). Lus par GameStartup avant de créer le moteur, réécrits à la fermeture.
    struct GameSettings {
        Math::Vector2i windowSize{1280, 720}; // zone de dessin de la fenêtre restaurée
        bool maximized = false;
        bool fullscreen = false; // plein écran sans bordure (Alt+Entrée ou F11 en jeu)
        bool vsync = true;
        bool temporalAA = true;    // anticrénelage temporel (TAA), en Natif
        Upscaling upscaling = DefaultUpscaling;
        float sharpness = DefaultSharpness; // netteté ajoutée par FSR, 0 à 1
        GroundDetail groundDetail = DefaultGroundDetail;
        CloudQuality cloudQuality = DefaultCloudQuality;
        float volume = 1.0f;       // volume général, 0 à 1
        bool showEngineStats = true; // panneau « Moteur » (images/s, temps CPU et GPU)
    };

    namespace GameSettingsFile {
        // Lecture tolérante : une valeur absente ou fausse garde sa valeur par défaut et ajoute un avertissement
        // (warnings) ; seul un fichier illisible (pas du JSON, mauvais format) est refusé.
        FLUX_API std::expected<GameSettings, std::string> Parse(std::string_view text, std::vector<std::string>* warnings = nullptr);
        FLUX_API std::string ToString(const GameSettings& settings);

        // Le fichier, à côté de l'exe.
        FLUX_API std::filesystem::path GetPath();
        // Sans fichier : les valeurs par défaut (premier lancement). Les avertissements vont dans le journal.
        FLUX_API std::expected<GameSettings, std::string> Load(const std::filesystem::path& path);
        FLUX_API std::expected<void, std::string> Save(const GameSettings& settings, const std::filesystem::path& path);
    }
}
