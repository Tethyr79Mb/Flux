#pragma once

#include "Flux/Export.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace Flux {
    // Projet de jeu (fichier .fluxproject, JSON) : ce que l'éditeur ouvre, et ce que Ars Solaris.exe lance.
    //     { "format": "flux.project", "version": 1, "name": "Flux Demo",
    //       "assets": "assets", "startupScene": "scenes/Demo.scene.json", "menu": "Interfaces/Options.fluxui" }
    // Les chemins du fichier sont relatifs : le même projet sert dans le dépôt (éditeur) et dans bin/ (jeu).
    struct Project
    {
        std::string name;
        std::filesystem::path file;            // le .fluxproject lui-même
        std::filesystem::path assetsDirectory; // absolu
        std::filesystem::path startupScene;    // relatif au dossier des assets
        // Menu du jeu (.fluxui, relatif au dossier des assets, avec des « / ») : présent dans toutes les scènes en jeu,
        // par-dessus leurs interfaces ; une fenêtre « Ouvrir avec Échap » l'ouvre. Vide : aucun.
        std::string menu;

        std::filesystem::path GetStartupScenePath() const { return assetsDirectory / startupScene; }
    };

    namespace ProjectFile {
        inline constexpr int FormatVersion = 1;

        // file : chemin du projet, auquel les chemins du fichier sont relatifs.
        FLUX_API std::expected<Project, std::string> Parse(std::string_view json, const std::filesystem::path& file);
        FLUX_API std::expected<Project, std::string> Load(const std::filesystem::path& file);

        // Texte du fichier (chemins relatifs, avec des « / ») ; les dossiers doivent rester sous celui du projet.
        FLUX_API std::string ToString(const Project& project);
        // Écrit project.file (d'abord dans un fichier temporaire : une coupure n'abîme pas l'ancien).
        FLUX_API std::expected<void, std::string> Save(const Project& project);
    }
}
