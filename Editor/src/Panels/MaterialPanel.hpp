#pragma once

#include <Flux/Assets/LayeredMaterial.hpp>

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
    // Panneau « Matériau » : couches d'un matériau multicouche (.fluxmat), ouvert par un double-clic dans le Contenu ou
    // « Modifier » dans l'inspecteur d'un modèle. Chaque réglage se voit sur les modèles qui l'utilisent (aperçu, sans
    // toucher au fichier : le modèle est relu au relâché) ; Ctrl+S l'enregistre, fermer sans enregistrer revient au
    // fichier. Ctrl+Z / Ctrl+Y annulent et rétablissent quand la souris est sur le panneau.
    class MaterialPanel {
        public:
        static bool IsMaterial(const std::filesystem::path& path);
        // Matériau neuf : une couche d'aluminium brossé posée partout, à changer ensuite.
        static std::string NewMaterialText();

        // Propose d'abord d'enregistrer le fichier ouvert s'il est modifié.
        void Open(Flux::Engine& engine, const std::filesystem::path& path);
        // Ctrl+S (enregistre), et Ctrl+Z / Ctrl+Y si la souris était sur le panneau : true si Ctrl+Z / Ctrl+Y est pris
        // ici (la scène ne doit pas l'avoir aussi). À appeler avant les panneaux.
        bool HandleShortcuts(Flux::Engine& engine);
        void Update(Flux::Engine& engine);
        // Fermeture de l'éditeur : propose d'enregistrer ; false si l'utilisateur annule.
        bool ConfirmClose(Flux::Engine& engine);

        // Le Contenu a déplacé, renommé ou supprimé un fichier (ou un dossier et ce qu'il contient).
        void FileMoved(Flux::Engine& engine, const std::filesystem::path& from, const std::filesystem::path& to);
        void FileDeleted(Flux::Engine& engine, const std::filesystem::path& path);

        private:
        bool IsOpen() const { return !m_path.empty(); }
        // Fichier illisible : rien n'a pu changer (et rien ne s'enregistrerait).
        bool IsModified() const { return IsOpen() && m_error.empty() && Write() != m_saved; }
        std::string Write() const { return Flux::SerializeLayeredMaterial(m_material); }
        bool Read(const std::string& text);
        bool Load(Flux::Engine& engine);
        bool Save(Flux::Engine& engine);
        void Close(Flux::Engine& engine);
        bool ConfirmDiscard(Flux::Engine& engine);
        void Undo(Flux::Engine& engine, bool redo);
        void Preview(Flux::Engine& engine, const std::string& text);
        void Commit(Flux::Engine& engine);

        bool DrawMaterial(Flux::Engine& engine, Flux::UI::Context& ui);
        bool DrawLayer(Flux::Engine& engine, Flux::UI::Context& ui, Flux::LayeredMaterialLayer& layer);
        // Image d'une couche ou du masque ; vide permis (« Sans »).
        bool DrawImage(Flux::Engine& engine, Flux::UI::Context& ui, const char* label, std::string& value);

        std::filesystem::path m_path;
        std::string m_error; // fichier illisible : rien à régler
        Flux::LayeredMaterial m_material;
        std::string m_saved; // texte du fichier sur le disque (Write des valeurs lues)
        std::filesystem::file_time_type m_savedTime{};
        // Annuler / refaire : un texte par pas, m_steps[m_position] est l'état affiché.
        std::vector<std::string> m_steps;
        std::size_t m_position = 0;
        std::string m_previewed; // dernier texte montré aux modèles
        std::chrono::steady_clock::time_point m_lastCheck{};
        bool m_pending = false; // modifié depuis le dernier pas d'historique
        bool m_hovered = false;
        bool m_placed = false;
        bool m_focus = false;
    };
}
