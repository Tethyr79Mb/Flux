#pragma once

// Sauvegarde rapide (F8 / F9, F7 l'efface) : une seule, à côté de l'exécutable. La scène est un fichier de scène
// ordinaire (on peut l'ouvrir dans l'éditeur) ; à côté, ce qu'elle ne garde pas : le siège où est assis le joueur et
// l'état des systèmes du jeu (Engine::AddGameSaveData). Engine décide quand sauvegarder et charge la scène.

#include "Flux/Core/Engine.hpp"

#include <expected>
#include <filesystem>
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
        // Faux si la scène de démarrage du projet a changé depuis la sauvegarde (ou si la sauvegarde ne le dit pas).
        static bool MatchesStartupScene(const Project& project);

        // État des systèmes du jeu (nom, texte donné par leur save), et sa reprise après le chargement d'une scène
        // (dans l'ordre d'ajout des systèmes). Servent aussi aux passages entre scènes (Engine::TakePassage).
        using SystemsState = std::vector<std::pair<std::string, std::string>>;
        SystemsState SaveSystems(Engine& engine) const;
        void LoadSystems(Engine& engine, const SystemsState& state) const;

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
    };
}
