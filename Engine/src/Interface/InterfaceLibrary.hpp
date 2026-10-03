#pragma once

#include "Flux/Interface/Interface.hpp"
#include "Flux/Scene/Scene.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Flux {
    // Fichiers .fluxui chargés (relus quand ils changent sur le disque : l'éditeur d'interface enregistre, le jeu
    // se met à jour) et interfaces en jeu, une par entité : une copie du document que la souris modifie (cases,
    // curseurs, fenêtres déplacées) sans toucher au fichier.
    class InterfaceLibrary {
        public:
        struct Instance
        {
            std::string file;
            std::uint64_t version = 0; // du fichier copié : un fichier relu remplace la copie
            Interface::Document document;
            Interface::Interaction interaction;
            std::uint64_t lastFrame = 0;
        };

        // nullptr si le fichier est absent ou illisible (l'erreur est écrite une fois dans la console).
        const Interface::Document* Get(const std::filesystem::path& assets, std::string_view file);
        // Interface en jeu de l'entité, (re)copiée du fichier au besoin ; nullptr si le fichier manque.
        Instance* GetInstance(const std::filesystem::path& assets, Entity entity, std::string_view file, std::uint64_t frame);
        // Oublie les interfaces en jeu qui n'ont pas été dessinées depuis la frame donnée (entité supprimée, scène
        // changée), ou toutes (jeu arrêté).
        void ForgetInstancesBefore(std::uint64_t frame);
        void ClearInstances() { m_instances.clear(); }
        // Ouvre ou ferme l'élément de ce nom dans les interfaces en jeu ; false si aucune n'en a.
        bool SetWindowOpen(std::string_view name, bool open);
        Interface::Element* FindInInstances(std::string_view name);

        private:
        using Clock = std::chrono::steady_clock;

        struct Entry
        {
            Interface::Document document;
            bool valid = false;
            std::filesystem::file_time_type time{};
            Clock::time_point checked{};
            std::uint64_t version = 0;
        };

        Entry& Refresh(const std::filesystem::path& assets, std::string_view file);

        std::unordered_map<std::string, Entry> m_files;
        std::unordered_map<Entity, Instance> m_instances;
        std::uint64_t m_nextVersion = 1;
    };
}
