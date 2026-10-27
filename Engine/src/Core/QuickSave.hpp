#pragma once

// Sauvegarde rapide (F8 / F9, F7 l'efface) : une seule, à côté de l'exécutable. La scène est un fichier de scène
// ordinaire (on peut l'ouvrir dans l'éditeur) ; à côté, ce qu'elle ne garde pas : le siège où est assis le joueur,
// l'état des systèmes du jeu (Engine::AddGameSaveData) et la scène d'où la partie était partie, telle qu'elle était
// (fusion avec la scène modifiée depuis dans l'éditeur, voir SceneMerge). Engine décide quand sauvegarder et charge
// la scène.

#include "Flux/Core/Engine.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    class QuickSave {
        public:
        // Données des systèmes du jeu, remplacées par nom (voir Engine::AddGameSaveData).
        void AddData(std::string name, Engine::SaveFunction save, Engine::LoadFunction load);
        void RemoveData(std::string_view name);
        void ClearData() { m_data.clear(); }

        // Demandés pendant l'image, faits à la fin (Engine::RunQuickSave).
        struct Requests
        {
            bool save = false;
            bool load = false;
            bool resumes = false; // chargement demandé par Engine::ResumeQuickSave : son propre message
        };
        void RequestSave() { m_requests.save = true; }
        void RequestLoad(bool resumes);
        Requests TakeRequests() { return std::exchange(m_requests, {}); }

        static std::filesystem::path GetSceneFile();
        static bool Exists();
        // false s'il n'y avait pas de sauvegarde.
        static bool Delete();
        // Copie de la scène d'où la partie était partie (SetScene), à fusionner avec sa version actuelle.
        static std::filesystem::path GetBaseSceneFile();

        // Fichier de la scène chargée (Engine::LoadScene) : son texte est gardé tel qu'au chargement, pour la fusion
        // si l'éditeur la modifie ensuite. Vide : scène sans fichier (mode Jouer de l'éditeur), pas de fusion.
        void SetScene(std::filesystem::path path, std::string text);
        void SetScene(const std::filesystem::path& path);
        // Scène d'où partait la partie sauvegardée, si la sauvegarde le dit et que sa copie est lisible : son fichier,
        // son texte d'alors, et si le fichier a changé depuis.
        struct Source
        {
            std::filesystem::path path;
            std::string text;
            bool changed = false;
        };
        static std::optional<Source> ReadSource(const Project* project);

        // Scène, siège du joueur, systèmes du jeu ; l'erreur en texte.
        std::expected<void, std::string> Write(Engine& engine, Entity player) const;
        // Après le chargement de la scène sauvegardée : joueur remis sur son siège, systèmes du jeu repris.
        void Restore(Engine& engine, Entity player) const;

        private:
        struct Data
        {
            std::string name;
            Engine::SaveFunction save;
            Engine::LoadFunction load;
        };
        std::vector<Data> m_data;
        Requests m_requests;
        std::filesystem::path m_scenePath;
        std::string m_sceneText;
    };
}
