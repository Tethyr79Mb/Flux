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

    // Panneau « Contenu » : les assets du projet (modèles, images...), en arbre ; les scènes sont à part, dans
    // l'onglet « Scènes », qui utilise GetScenes et ShowSceneMenu d'ici.
    // Le dossier est relu régulièrement : un fichier ajouté hors de l'éditeur apparaît tout seul.
    class ContentBrowser {
        public:
        void Update(Flux::Engine& engine, SceneDocument& document, double deltaSeconds);
        // Fichiers de scène du dossier assets (sous-dossiers compris), triés par dossier puis par nom.
        const std::vector<std::filesystem::path>& GetScenes() const { return m_scenes; }
        // Menu clic droit d'une scène (à appeler juste après sa ligne) : ouvrir, renommer, scène de démarrage,
        // afficher dans l'explorateur, supprimer.
        void ShowSceneMenu(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document,
                           const std::filesystem::path& path);
        static bool IsStartupScene(const Flux::Engine& engine, const std::filesystem::path& path);

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
        void Delete(Flux::Engine& engine, SceneDocument& document, const std::filesystem::path& path);
        // Fenêtre « Renommer » ouverte par le menu clic droit d'une scène.
        void DrawRenameWindow(Flux::Engine& engine, SceneDocument& document);
        // false (avec un message) si le nom ne convient pas ou si le fichier n'a pas pu être renommé.
        bool Rename(Flux::Engine& engine, SceneDocument& document, const std::string& name);
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
