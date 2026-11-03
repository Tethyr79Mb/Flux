#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Vector2.hpp>
#include <Flux/UI/Context.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class SceneEditor;
    class UndoHistory;

    // Panneau « Placer » (le « Place Actors » d'Unreal) : les objets qu'on peut ajouter à la scène, par catégorie.
    // Un clic crée l'objet (devant la caméra, ou au centre de l'écran pour une image) ; le glisser dans la vue
    // le pose sous la souris. Un modèle 3D ou un personnage demande d'abord son fichier. L'objet créé est sélectionné, et Ctrl+Z le retire.
    // Les prefabs du projet ont leur section : un clic pose un nouvel exemplaire. Le jeu ajoute ses propres objets
    // (Engine::GetEditorExtensions) : chasseur armé, stations...
    class PlacePanel {
        public:
        // view : rectangle de la vue 3D (retourné par EditorLayout::Update). prefabs : ContentBrowser::GetPrefabs.
        // dropToGround : bouton « Poser au sol » de la vue (SceneViewport::IsDroppingToGround), l'objet posé descend
        // sur ce qui est dessous.
        void Update(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const Flux::UI::Rect& view,
                    const std::vector<std::filesystem::path>& prefabs, bool dropToGround);

        // Pour les outils (serveur MCP) : libellés des objets du panneau, ceux du jeu compris (un prefab : « Prefab :
        // nom »), et pose de l'un d'eux comme un clic (devant la caméra, sélectionné, annulable). file : fichier d'un
        // modèle, personnage, interface ou son, relatif aux assets (sans lui, la boîte de dialogue). Rend l'objet posé
        // (ou le ciel, le soleil déjà là).
        static std::vector<std::string> GetItemLabels(Flux::Engine& engine, const std::vector<std::filesystem::path>& prefabs);
        static std::expected<Flux::Uuid, std::string> PlaceByLabel(Flux::Engine& engine, SceneEditor& sceneEditor,
                                                                   UndoHistory& history, std::string_view label,
                                                                   std::optional<std::string> file, const Flux::UI::Rect& view,
                                                                   bool dropToGround,
                                                                   const std::vector<std::filesystem::path>& prefabs);

        private:
        // Extension : objet ajouté par le jeu (Engine::GetEditorExtensions), retrouvé par son libellé.
        enum class Item { Empty, Cube, Model, ScreenImage, SceneChange, Character, Ship, Light, Spot, Sky, Interior, Interface, Sound, Planet, Sun, Prefab, Extension };
        struct Choice
        {
            Item item;
            std::filesystem::path prefab; // Item::Prefab : son fichier
            std::string extension;        // Item::Extension : son libellé (le jeu rechargé à chaud peut l'avoir retiré)
            bool operator==(const Choice&) const = default;
        };

        // drop : point de la vue où l'objet a été lâché ; sans lui, la place par défaut. file : voir PlaceByLabel.
        // Erreur vide : abandon sans message (boîte de dialogue fermée, objet du jeu retiré).
        static std::expected<Flux::Uuid, std::string> Place(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history,
                                                            const Choice& choice, std::optional<Flux::Math::Vector2f> drop,
                                                            const Flux::UI::Rect& view, bool dropToGround,
                                                            std::optional<std::string> file = std::nullopt);
        // Place depuis le panneau : une erreur est montrée dans une boîte de dialogue.
        static void PlaceFromPanel(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const Choice& choice,
                                   std::optional<Flux::Math::Vector2f> drop, const Flux::UI::Rect& view, bool dropToGround);

        std::optional<Choice> m_held; // bouton tenu
        std::string m_filter;         // recherche dans la liste
        Flux::Math::Vector2f m_pressMouse;
        bool m_dragging = false;    // la souris s'est éloignée du bouton tenu : glisser-déposer en cours
    };
}
