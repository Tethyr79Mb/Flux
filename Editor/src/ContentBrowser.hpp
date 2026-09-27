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
    // clic droit pour en faire la scène de démarrage du jeu.
    // Le dossier est relu régulièrement : un fichier ajouté hors de l'éditeur apparaît tout seul.
    class ContentBrowser {
        public:
        void Update(Flux::Engine& engine, SceneDocument& document, double deltaSeconds);

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
        static bool IsStartupScene(const Flux::Engine& engine, const std::filesystem::path& path);
        // Game.exe chargera cette scène au lancement (enregistré dans le .fluxproject, recopié dans bin/ au build).
        static void SetStartupScene(Flux::Engine& engine, const std::filesystem::path& path);

        std::filesystem::path m_root;
        std::vector<Entry> m_entries;
        double m_sinceScan = 0.0;
    };
}
