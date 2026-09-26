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

    // Panneau « Contenu » : les fichiers du dossier assets du projet, en arbre. Double-clic sur une scène pour l'ouvrir.
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

        std::filesystem::path m_root;
        std::vector<Entry> m_entries;
        double m_sinceScan = 0.0;
    };
}
