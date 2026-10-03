#pragma once

#include <Flux/Interface/Interface.hpp>
#include <Flux/Math/Vector2.hpp>
#include <Flux/UI/Context.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Éditeur d'interface : les fichiers .fluxui (menus, fenêtres du jeu) ouverts en onglets au-dessus de la vue, à
    // côté de l'onglet « Scène ». Quand une interface est affichée, elle remplace la vue 3D : l'écran du jeu à la
    // résolution choisie, où l'on clique, glisse et redimensionne les éléments ; le panneau « Interface » (ajouter,
    // arbre des éléments) et le panneau « Propriétés » prennent place à côté de Scènes et de l'Inspecteur.
    // Chaque interface a son annuler / refaire et s'enregistre avec Ctrl+S.
    class InterfaceEditor {
        public:
        // Ouvre le fichier dans un onglet (ou affiche son onglet s'il est déjà ouvert) et l'affiche.
        void Open(Flux::Engine& engine, const std::filesystem::path& path);
        // Une interface est affichée (sinon, la scène).
        bool IsActive() const { return m_active >= 0; }
        // Revient à la scène (mode Jouer).
        void ShowScene() { m_active = -1; }

        // Onglets au-dessus de la vue (rien sans interface ouverte) ; retourne la place qui reste pour la vue.
        // À appeler après la disposition (EditorLayout::Update), avant les panneaux.
        Flux::UI::Rect DrawTabs(Flux::Engine& engine, const Flux::UI::Rect& view);
        // Écran de l'interface affichée, et ses panneaux. Sans interface affichée, ne fait que rendre la vue à la scène.
        void Update(Flux::Engine& engine, const Flux::UI::Rect& view);
        // Ctrl+S, Ctrl+Z, Ctrl+Y, Ctrl+D, Suppr, flèches : à appeler à chaque frame, avant les panneaux, quand une
        // interface est affichée (la scène garde ses raccourcis sinon).
        void HandleShortcuts(Flux::Engine& engine);
        // Éléments du menu « Édition » pour l'interface affichée.
        void DrawEditMenuItems(Flux::UI::Context& ui);
        // Élément « Enregistrer l'interface » du menu « Fichier ».
        void DrawFileMenuItems(Flux::UI::Context& ui, Flux::Engine& engine);
        // Fermeture de l'éditeur : propose d'enregistrer chaque interface modifiée ; false si l'utilisateur annule.
        bool ConfirmClose(Flux::Engine& engine);

        // Panneau Contenu : un fichier ou un dossier déplacé ou renommé (onglets et images des interfaces suivent),
        // un fichier supprimé (son onglet devient « sans fichier »).
        void FileMoved(const std::filesystem::path& from, const std::filesystem::path& to, const std::string& oldReference,
                       const std::string& newReference);
        void FileDeleted(const std::filesystem::path& path);

        private:
        struct Snapshot
        {
            std::string json;
            std::uint32_t selection = 0;
        };

        struct OpenInterface
        {
            std::filesystem::path path; // vide : fichier supprimé, à enregistrer sous un autre nom
            Flux::Interface::Document document;
            std::string savedJson;     // contenu du fichier : modifié tant que l'état diffère
            std::string committedJson; // dernier état mis dans l'historique
            std::vector<Snapshot> undo;
            std::vector<Snapshot> redo;
            bool changed = false;      // modifié depuis le dernier pas d'historique
            std::uint32_t selection = 0;
            // Vue : agrandissement par rapport à l'écran ajusté à la vue, et décalage (pixels).
            float zoom = 1.0f;
            Flux::Math::Vector2f pan;
            int resolution = 0; // index dans la liste des résolutions d'aperçu

            bool IsModified() const { return committedJson != savedJson || changed; }
            std::string GetName() const;
        };

        // Geste à la souris sur l'écran.
        struct Drag
        {
            enum class Kind { None, Pending, Move, Resize, Pan } kind = Kind::None;
            int handle = -1; // poignée tenue (0-7, dans le sens des aiguilles d'une montre depuis le coin haut-gauche)
            Flux::Math::Vector2f startMouse{};
            Flux::Interface::Rect startRect{};
            Flux::Interface::Rect parentRect{};
            Flux::Math::Vector2f startPan{};
        };

        OpenInterface* GetActive() { return m_active >= 0 ? &m_open[static_cast<std::size_t>(m_active)] : nullptr; }
        Flux::Interface::Element* GetSelected();
        void Select(std::uint32_t id);
        void MarkChanged();
        // Met l'état modifié dans l'historique quand aucun geste n'est en cours.
        void CommitHistory(bool interacting);
        void Undo(bool redo);
        bool Save(Flux::Engine& engine, OpenInterface& open);
        // Ferme l'onglet après avoir proposé d'enregistrer ; false si l'utilisateur annule.
        bool Close(Flux::Engine& engine, std::size_t index);

        // Écran (InterfaceCanvas.cpp). Résolutions d'aperçu : noms affichés, taille en pixels.
        static std::span<const std::string_view> GetResolutionNames();
        static Flux::Math::Vector2f GetResolution(int index);
        Flux::Interface::Rect GetScreenRect(const OpenInterface& open, const Flux::UI::Rect& view) const;
        void UpdateCanvas(Flux::Engine& engine, const Flux::UI::Rect& view);
        void DrawSelection(Flux::Engine& engine, const std::vector<Flux::Interface::Placed>& placed, const Flux::UI::Rect& view,
                           std::uint32_t hovered);

        // Panneaux (InterfacePanels.cpp).
        void DrawElementsPanel(Flux::Engine& engine);
        // command : action du menu clic droit choisie sur action (appliquée après l'arbre, qui ne doit pas changer
        // pendant qu'on le parcourt).
        void DrawElementNode(Flux::Engine& engine, Flux::Interface::Element& element, std::uint32_t& action, int& command);
        void DrawPropertiesPanel(Flux::Engine& engine);
        // Cible de l'action d'un bouton : une fenêtre du document, une scène, une fonction ; true si changée.
        bool DrawActionTarget(Flux::Engine& engine, const Flux::Interface::Document& document, Flux::Interface::Element& button);
        void Add(Flux::Interface::ElementType type);
        void Duplicate();
        void Delete();
        // Avance (vers le premier plan, +1) ou recule l'élément parmi ses voisins.
        void Reorder(std::uint32_t id, int direction);
        // Range l'élément avant (zone -1), dans (0) ou après (1) la cible, à la même place à l'écran.
        void MoveTo(std::uint32_t id, std::uint32_t target, int zone);
        // Déplace l'élément sélectionné de delta pixels de référence (flèches).
        void Nudge(Flux::Math::Vector2f delta);

        std::vector<OpenInterface> m_open;
        int m_active = -1;
        bool m_wasActive = false;
        bool m_panelsPlaced = false;
        Drag m_drag;
        // Élément glissé dans l'arbre du panneau Interface.
        struct TreeDrag
        {
            std::uint32_t id = 0;
            Flux::Math::Vector2f pressMouse{};
            bool dragging = false;
            std::uint32_t target = 0; // élément survolé cette frame
            int zone = 0;             // -1 : avant lui, 0 : dedans, 1 : après
        };
        TreeDrag m_treeDrag;
        // Rectangle de l'écran et échelle à la dernière frame (pour les gestes au clavier).
        Flux::Interface::Rect m_screen;
        float m_scale = 0.0f;
        int m_hoveredTab = -1;
    };
}
