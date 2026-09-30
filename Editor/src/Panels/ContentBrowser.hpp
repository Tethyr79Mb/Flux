#pragma once

#include <Flux/Math/Vector2.hpp>

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

    // Panneau « Contenu » : les assets du projet (modèles, images...), comme un explorateur de fichiers : l'arbre des
    // dossiers à gauche, le contenu du dossier choisi à droite en vignettes. On y crée des dossiers et on y range
    // les fichiers en les glissant sur un dossier ; les scènes qui les utilisent sont mises à jour. Les scènes sont à
    // part, dans l'onglet « Scènes », qui utilise GetScenes et ShowSceneMenu d'ici.
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

        // Fenêtre qui demande un nom : nouveau dossier (dans path) ou nouveau nom d'un fichier ou dossier (path).
        enum class NameRequest { None, NewFolder, Rename };

        // Fichier ou dossier glissé vers un dossier.
        struct Drag
        {
            std::filesystem::path path; // vide : rien
            Flux::Math::Vector2f pressMouse;
            bool dragging = false;
            std::filesystem::path target; // dossier survolé cette frame (vide : aucun)
        };

        void Scan(const std::filesystem::path& root);
        const Entry* FindFolder(const std::filesystem::path& path) const;
        void DrawFolder(Flux::UI::Context& ui, const Entry& folder);
        void DrawFiles(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, const Entry& folder);
        void ShowContextMenu(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, const Entry& entry);
        // Dossier survolé pendant un glisser-déposer : cible, et cadre autour de sa ligne ou de sa vignette.
        void DropTarget(Flux::UI::Context& ui, const std::filesystem::path& folder);
        void DrawNameWindow(Flux::Engine& engine, SceneDocument& document);
        // Déplace ou renomme un fichier ou un dossier des assets, et met à jour ce qui le désigne : champs des scènes
        // (fichiers et scène ouverte), scène de démarrage. false (avec un message) si c'est impossible.
        bool Move(Flux::Engine& engine, SceneDocument& document, const std::filesystem::path& from, const std::filesystem::path& to);
        void Delete(Flux::Engine& engine, SceneDocument& document, const std::filesystem::path& path);
        // Fenêtre « Renommer » ouverte par le menu clic droit d'une scène.
        void DrawRenameWindow(Flux::Engine& engine, SceneDocument& document);
        // false (avec un message) si le nom ne convient pas ou si le fichier n'a pas pu être renommé.
        bool Rename(Flux::Engine& engine, SceneDocument& document, const std::string& name);
        // Game.exe chargera cette scène au lancement (enregistré dans le .fluxproject, recopié dans bin/ au build).
        static void SetStartupScene(Flux::Engine& engine, const std::filesystem::path& path);

        std::filesystem::path m_root;
        Entry m_tree; // le dossier des assets, sans les scènes
        std::filesystem::path m_folder;   // dossier affiché à droite
        std::filesystem::path m_selected; // vignette sélectionnée
        Drag m_drag;
        NameRequest m_request = NameRequest::None;
        std::filesystem::path m_requestPath;
        std::string m_requestName;
        std::vector<std::filesystem::path> m_scenes;
        double m_sinceScan = 0.0;
        std::filesystem::path m_renaming; // scène en cours de renommage (vide : fenêtre fermée)
        std::string m_newName;            // sans « .scene.json »
    };
}
