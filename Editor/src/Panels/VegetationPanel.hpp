#pragma once

#include <Flux/Planetology/Vegetation.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Flux {
    class Engine;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    // Panneau « Végétation » : réglages d'un biome (.biome.json) ou d'une espèce d'arbre (.espece.json), ouvert par un
    // double-clic dans le Contenu. Chaque réglage se voit tout de suite sur les planètes (aperçu, sans toucher au
    // fichier) ; Ctrl+S l'enregistre, fermer sans enregistrer revient au fichier. Ctrl+Z / Ctrl+Y annulent et
    // rétablissent quand la souris est sur le panneau.
    class VegetationPanel {
        public:
        static bool IsBiome(const std::filesystem::path& path);
        static bool IsSpecies(const std::filesystem::path& path);

        // Propose d'abord d'enregistrer le fichier ouvert s'il est modifié.
        void Open(Flux::Engine& engine, const std::filesystem::path& path);
        // Ctrl+S (enregistre), et Ctrl+Z / Ctrl+Y si la souris était sur le panneau : true si Ctrl+Z / Ctrl+Y est pris
        // ici (la scène ne doit pas l'avoir aussi). À appeler avant les panneaux.
        bool HandleShortcuts(Flux::Engine& engine);
        void Update(Flux::Engine& engine);
        // Fermeture de l'éditeur : propose d'enregistrer ; false si l'utilisateur annule.
        bool ConfirmClose(Flux::Engine& engine);

        // Le Contenu a déplacé ou renommé from (un fichier, ou un dossier et ce qu'il contient), et réécrit les
        // biomes qui désignaient oldReference.
        void FileMoved(const std::filesystem::path& from, const std::filesystem::path& to, const std::string& oldReference,
                       const std::string& newReference);
        void FileDeleted(const std::filesystem::path& path);

        private:
        enum class Kind { None, Biome, Species };

        // Fichier illisible : rien n'a pu changer (et rien ne s'enregistrerait).
        bool IsModified() const { return m_kind != Kind::None && m_error.empty() && Write() != m_saved; }
        std::string Write() const;
        bool Read(const std::string& text);
        bool Load();
        bool Save(Flux::Engine& engine);
        void Close();
        // Propose d'enregistrer avant de quitter le fichier ; false si l'utilisateur annule.
        bool ConfirmDiscard(Flux::Engine& engine);
        void Undo(bool redo);
        void Preview(const std::string& text);
        void Commit();

        bool DrawBiome(Flux::Engine& engine, Flux::UI::Context& ui);
        bool DrawSpecies(Flux::Engine& engine, Flux::UI::Context& ui);
        bool DrawAsset(Flux::Engine& engine, Flux::UI::Context& ui, const char* label, Flux::AssetType type,
                       std::string& value);

        Kind m_kind = Kind::None;
        std::filesystem::path m_path;
        std::string m_reference; // m_path relatif aux assets, avec des « / » : le nom de l'aperçu
        std::string m_error;     // fichier illisible : rien à régler
        Flux::Planetology::VegetationBiome m_biome;
        Flux::Planetology::TreeSpecies m_species;
        std::string m_saved;     // texte du fichier sur le disque (Write des valeurs lues)
        std::filesystem::file_time_type m_savedTime{};
        // Annuler / refaire : un texte par pas, m_steps[m_position] est l'état affiché.
        std::vector<std::string> m_steps;
        std::size_t m_position = 0;
        std::string m_previewed; // dernier texte montré aux planètes
        std::chrono::steady_clock::time_point m_lastPreview{};
        std::chrono::steady_clock::time_point m_lastCheck{};
        bool m_pending = false; // modifié depuis le dernier pas d'historique
        std::filesystem::path m_openNext; // espèce demandée depuis un biome (« Modifier »), ouverte après le panneau
        bool m_hovered = false;
        bool m_placed = false;
        bool m_focus = false;
    };
}
