#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/UI/DrawList.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    class Scene;

    // Éléments d'interface de la scène (ScreenTextComponent, ScreenImageComponent, InterfaceComponent) : dessinés à plat par-dessus la 3D, dans l'écran du jeu
    // ou dans la vue de l'éditeur.
    namespace ScreenUi {
        // Hauteur d'écran de référence : tailles et décalages sont exprimés pour 1080 lignes, puis mis à l'échelle
        // de la vue (un texte de 64 fait 64 pixels en 1080p, 85 en 1440p, et reste proportionné dans l'éditeur).
        inline constexpr float ReferenceHeight = 1080.0f;

        // Rectangle d'un élément dessiné, pour le sélectionner à la souris.
        struct ItemRect
        {
            Entity entity = NullEntity;
            Math::Vector2f min;
            Math::Vector2f max;
        };

        // Texture d'une image de la scène (chemin tel qu'écrit dans le composant) : Engine::LoadAssetTexture.
        using TextureResolver = std::function<std::optional<UI::TextureInfo>(std::string_view imagePath)>;

        // Police d'un texte de la scène (chemin tel qu'écrit dans le composant) : Engine::LoadAssetFont.
        using FontResolver = std::function<std::optional<UI::FontRef>(std::string_view fontPath)>;

        // Interface d'une entité (InterfaceComponent) à dessiner dans la vue : Engine::DrawInterface. Retourne le
        // rectangle qu'elle occupe (pour la sélection), nullopt si rien n'est dessiné.
        struct InterfaceRequest
        {
            Entity entity;
            const InterfaceComponent& component;
            UI::DrawList& list;
            Math::Vector2f viewMin;
            Math::Vector2f viewMax;
            bool placeholders;
            bool needsRect; // l'appelant veut le rectangle (sélection dans l'éditeur) : sinon, inutile de le calculer
        };
        using InterfaceDrawer = std::function<std::optional<std::pair<Math::Vector2f, Math::Vector2f>>(const InterfaceRequest&)>;

        struct DrawOptions
        {
            TextureResolver textures{}; // sans : les images ne sont pas dessinées
            InterfaceDrawer interfaces{}; // sans : les interfaces ne sont pas dessinées
            // Reçoit les rectangles des éléments dessinés (découpés à la vue), dans l'ordre du dessin.
            std::vector<ItemRect>* rects = nullptr;
            // Éditeur : un cadre gris remplace une image sans fichier ou introuvable, pour la voir et la sélectionner.
            bool placeholders = false;
            FontResolver fonts{}; // sans : les textes sont tous dans la police de la liste
        };

        // Dessine les éléments de la scène dans la vue min-max, dans l'ordre de la hiérarchie (le dernier par-dessus) ;
        // sur une même entité, l'image passe sous le texte.
        FLUX_API void Draw(const Scene& scene, UI::DrawList& list, Math::Vector2f viewMin, Math::Vector2f viewMax,
                           const DrawOptions& options = {});

        // L'entité porte un élément d'interface (pas d'objet dans le monde : pas de gizmo).
        FLUX_API bool IsScreenElement(const Scene& scene, Entity entity);

        // Décalage de l'élément qu'on peut glisser dans la vue (texte, image de taille donnée), en pixels de référence ;
        // nullopt si l'entité n'en a pas (image étirée ou couvrant l'écran : sa place ne dépend pas du décalage).
        struct Offset
        {
            float* x;
            float* y;
        };
        FLUX_API std::optional<Offset> FindOffset(Scene& scene, Entity entity);

        // Point de la vue où se trouve l'ancre (coin, milieu d'un bord ou centre).
        FLUX_API Math::Vector2f AnchorPoint(ScreenAnchor anchor, Math::Vector2f viewMin, Math::Vector2f viewMax);
        // Échelle de la vue : pixels de la vue par pixel de référence.
        FLUX_API float GetScale(Math::Vector2f viewMin, Math::Vector2f viewMax);
    }
}
