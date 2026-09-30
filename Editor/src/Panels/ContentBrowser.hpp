#pragma once

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
    class SceneDocument;

    // Panneau « Contenu » : les fichiers du dossier assets du projet, en arbre. Double-clic sur une scène pour l'ouvrir,
    // clic droit pour la renommer ou en faire la scène de démarrage du jeu.
    // Le dossier est relu régulièrement : un fichier ajouté hors de l'éditeur apparaît tout seul.
    class ContentBrowser {
        public:
        void Update(Flux::Engine& engine, SceneDocument& document, double deltaSeconds);
        // Fichiers de scène du dossier assets (sous-dossiers compris), dans l'ordre du panneau : l'onglet « Scènes ».
        const std::vector<std::filesystem::path>& GetScenes() const { return m_scenes; }

        private:
        struct Entry
        {
            std::filesystem::path path;
            std::string name; // UTF-8
            bool directory = false;
            std::vector<Entry> children;
        };

        void Scan(const std::filesystem::path& root);
        void DrawEntry(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, const Entry& entry);
        void ShowContextMenu(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, const Entry& entry);
        void Delete(Flux::Engine& engine, SceneDocument& document, const Entry& entry);
        // Fenêtre « Renommer » ouverte par le menu clic droit d'une scène.
        void DrawRenameWindow(Flux::Engine& engine, SceneDocument& document);
        // false (avec un message) si le nom ne convient pas ou si le fichier n'a pas pu être renommé.
        bool Rename(Flux::Engine& engine, SceneDocument& document, const std::string& name);
        static bool IsStartupScene(const Flux::Engine& engine, const std::filesystem::path& path);
        // Game.exe chargera cette scène au lancement (enregistré dans le .fluxproject, recopié dans bin/ au build).
        static void SetStartupScene(Flux::Engine& engine, const std::filesystem::path& path);

        std::filesystem::path m_root;
        std::vector<Entry> m_entries;
        std::vector<std::filesystem::path> m_scenes;
        double m_sinceScan = 0.0;
        std::filesystem::path m_renaming; // scène en cours de renommage (vide : fenêtre fermée)
        std::string m_newName;            // sans « .scene.json »
    };
}
