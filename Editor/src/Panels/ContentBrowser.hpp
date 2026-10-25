#pragma once

#include <Flux/Math/Vector2.hpp>
#include <Flux/UI/DrawList.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Flux {
    class Engine;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    class InterfaceEditor;
    class SceneDocument;
    class VegetationPanel;

    // Panneau « Contenu » : les assets du projet (modèles, images...), comme un explorateur de fichiers : l'arbre des
    // dossiers à gauche, le contenu du dossier choisi à droite en vignettes. On y crée des dossiers et on y range
    // les fichiers en les glissant sur un dossier ; les scènes qui les utilisent sont mises à jour. Les scènes sont à
    // part, dans l'onglet « Scènes », qui les montre avec DrawSceneBrowser (mêmes dossiers, mêmes gestes).
    // Le dossier est relu régulièrement : un fichier ajouté hors de l'éditeur apparaît tout seul.
    class ContentBrowser {
        public:
        void Update(Flux::Engine& engine, SceneDocument& document, double deltaSeconds);
        // Éditeur où s'ouvrent les interfaces (.fluxui) : double-clic, « Nouvelle interface », et qui suit les fichiers
        // déplacés ou supprimés.
        void SetInterfaceEditor(InterfaceEditor* editor) { m_interfaces = editor; }
        // Panneau où s'ouvrent les biomes et les espèces d'arbres : double-clic, « Nouveau biome », « Nouvelle espèce ».
        void SetVegetationPanel(VegetationPanel* panel) { m_vegetation = panel; }
        // Fichiers de scène du dossier assets (sous-dossiers compris), triés par dossier puis par nom.
        const std::vector<std::filesystem::path>& GetScenes() const { return m_scenes; }
        // Prefabs (.prefab.json) du dossier assets, triés de même : le panneau « Placer » les propose.
        const std::vector<std::filesystem::path>& GetPrefabs() const { return m_prefabs; }
        // Menu clic droit d'une scène (à appeler juste après sa ligne) : ouvrir, renommer, scène de démarrage,
        // afficher dans l'explorateur, supprimer.
        void ShowSceneMenu(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document,
                           const std::filesystem::path& path);
        static bool IsStartupScene(const Flux::Engine& engine, const std::filesystem::path& path);
        // Les scènes du projet en vignettes, rangées dans leurs dossiers (ceux qui en contiennent, et les vides) :
        // double-clic sur une scène pour l'ouvrir, glisser sur un dossier pour l'y ranger. height : hauteur de la
        // zone des vignettes. Appelé dans un panneau, avant Update dans la frame (le glisser-déposer finit là).
        void DrawSceneBrowser(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, float height);

        private:
        struct Entry
        {
            std::filesystem::path path;
            std::string name; // UTF-8
            bool directory = false;
            std::vector<Entry> children;
        };

        // Fenêtre qui demande un nom : nouveau dossier (dans path) ou nouveau nom d'un fichier ou dossier (path).
        enum class NameRequest { None, NewFolder, NewInterface, NewBiome, NewSpecies, Rename };

        // Fichier ou dossier glissé vers un dossier.
        struct Drag
        {
            std::filesystem::path path; // vide : rien
            Flux::Math::Vector2f pressMouse;
            bool dragging = false;
            std::filesystem::path target; // dossier survolé cette frame (vide : aucun)
        };

        void Scan(const std::filesystem::path& root);
        // Fichiers et dossiers de tous les assets dont le nom contient m_search, en une liste (vignettes à plat).
        const Entry& SearchResults();
        const Entry* FindFolder(const Entry& tree, const std::filesystem::path& path) const;
        void DrawFolder(Flux::UI::Context& ui, const Entry& folder);
        void DrawTiles(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, const Entry& shown,
                       std::filesystem::path& folder, bool scenes);
        // Icône de l'explorateur de Windows pour ce dossier ou ce type de fichier, demandée une fois par type ;
        // nullptr si Windows n'en donne pas (la vignette dessine alors la sienne).
        const Flux::UI::TextureInfo* GetIcon(Flux::Engine& engine, const Entry& entry);
        void ShowContextMenu(Flux::UI::Context& ui, Flux::Engine& engine, SceneDocument& document, const Entry& entry,
                             std::filesystem::path& folder);
        // Dossier survolé pendant un glisser-déposer : cible, et cadre autour de sa ligne ou de sa vignette.
        void DropTarget(Flux::UI::Context& ui, const std::filesystem::path& folder);
        void DrawNameWindow(Flux::Engine& engine, SceneDocument& document);
        // Nouveau biome ou nouvelle espèce (fichier) dans m_requestPath, ouvert aussitôt ; false (avec un message) si
        // c'est impossible.
        bool CreateVegetationFile(Flux::Engine& engine);
        // Déplace ou renomme un fichier ou un dossier des assets, et met à jour ce qui le désigne : champs des scènes
        // (fichiers et scène ouverte), scène de démarrage. false (avec un message) si c'est impossible.
        bool Move(Flux::Engine& engine, SceneDocument& document, const std::filesystem::path& from, const std::filesystem::path& to);
        void Delete(Flux::Engine& engine, SceneDocument& document, const std::filesystem::path& path);
        // Fenêtre « Renommer » ouverte par le menu clic droit d'une scène.
        void DrawRenameWindow(Flux::Engine& engine, SceneDocument& document);
        // false (avec un message) si le nom ne convient pas ou si le fichier n'a pas pu être renommé.
        bool Rename(Flux::Engine& engine, SceneDocument& document, const std::string& name);
        // Ars Solaris.exe chargera cette scène au lancement (enregistré dans le .fluxproject, recopié dans bin/ au build).
        static void SetStartupScene(Flux::Engine& engine, const std::filesystem::path& path);

        std::filesystem::path m_root;
        Entry m_tree;      // le dossier des assets, sans les scènes
        Entry m_sceneTree; // les scènes et leurs dossiers
        std::filesystem::path m_sceneFolder; // dossier affiché dans l'onglet Scènes
        std::filesystem::path m_shownScene;  // scène ouverte à la dernière frame (son dossier est affiché quand elle change)
        std::filesystem::path m_folder;   // dossier affiché à droite
        std::filesystem::path m_selected; // vignette sélectionnée
        Drag m_drag;
        NameRequest m_request = NameRequest::None;
        std::filesystem::path m_requestPath;
        std::string m_requestName;
        // Icônes du système par extension en minuscules (« / » : dossier) ; nullopt : pas d'icône, ne pas redemander.
        std::unordered_map<std::string, std::optional<Flux::UI::TextureInfo>> m_icons;
        std::vector<std::filesystem::path> m_scenes;
        std::vector<std::filesystem::path> m_prefabs;
        std::vector<std::filesystem::path> m_interfaceFiles; // .fluxui du projet : leurs images suivent un déplacement
        std::vector<std::filesystem::path> m_biomeFiles;     // .biome.json : leurs espèces suivent un déplacement
        std::vector<std::filesystem::path> m_speciesFiles;   // .espece.json : la première sert de modèle à une nouvelle
        InterfaceEditor* m_interfaces = nullptr;
        VegetationPanel* m_vegetation = nullptr;
        double m_sinceScan = 0.0;
        std::uint64_t m_scanCount = 0; // lectures du disque : les résultats de recherche d'avant sont périmés
        std::string m_search;
        Entry m_results;
        std::string m_resultsFor;
        std::uint64_t m_resultsScan = 0;
        std::filesystem::path m_renaming; // scène en cours de renommage (vide : fenêtre fermée)
        std::string m_newName;            // sans « .scene.json »
    };
}
