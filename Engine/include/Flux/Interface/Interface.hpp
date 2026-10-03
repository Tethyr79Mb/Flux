#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/UI/DrawList.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Interfaces du jeu (menus, fenêtres, écrans de réglages) : un fichier .fluxui construit dans l'éditeur d'interface,
// affiché à l'écran par InterfaceComponent. Un document est un arbre d'éléments (fenêtre, cadre, texte, image,
// bouton, case à cocher, curseur) placés par une ancre dans leur parent, en pixels d'un écran de 1080 lignes comme
// les textes à l'écran (ScreenUi) : l'interface garde ses proportions dans toutes les résolutions.
namespace Flux::Interface {
    inline constexpr float ReferenceHeight = 1080.0f;

    enum class ElementType : std::uint8_t {
        Window,   // fond, barre de titre, ombre ; se déplace par sa barre en jeu si « Déplaçable »
        Frame,    // cadre : un fond qui regroupe des éléments
        Text,
        Image,
        Button,
        Checkbox,
        Slider,
        Count,
    };

    // Point du parent où l'élément est accroché (le même point de l'élément s'y pose : coin sur coin, centre sur
    // centre), ou Stretch : l'élément suit les quatre bords du parent, à des marges données.
    enum class Anchor : std::uint8_t {
        TopLeft,
        Top,
        TopRight,
        Left,
        Center,
        Right,
        BottomLeft,
        Bottom,
        BottomRight,
        Stretch,
        Count,
    };

    enum class TextAlign : std::uint8_t { Left, Center, Right, Count };

    struct Element
    {
        std::uint32_t id = 0; // unique dans le document, jamais réutilisé
        std::string name;     // pour s'y retrouver dans l'arbre (et, plus tard, le désigner depuis le jeu)
        ElementType type = ElementType::Frame;
        Anchor anchor = Anchor::Center;
        // Ancre ordinaire : décalage depuis l'ancre (vers la droite et le bas) et taille.
        // Stretch : marges gauche et haut (position), droite et bas (size).
        Math::Vector2f position;
        Math::Vector2f size{100.0f, 100.0f};
        bool visible = true;

        Math::LinearColor background{0.0f, 0.0f, 0.0f, 0.0f};
        Math::LinearColor border{0.0f, 0.0f, 0.0f, 0.0f};
        float borderWidth = 0.0f;
        float radius = 0.0f; // coins arrondis

        std::string text; // libellé (bouton, case), titre (fenêtre), texte
        Math::LinearColor textColor{1.0f, 1.0f, 1.0f};
        float fontSize = 24.0f;
        TextAlign align = TextAlign::Left;

        // Bouton : fond survolé et enfoncé. Case et curseur : coche et partie remplie (accent).
        Math::LinearColor hoverColor{1.0f, 1.0f, 1.0f};
        Math::LinearColor pressedColor{1.0f, 1.0f, 1.0f};
        Math::LinearColor accent{1.0f, 1.0f, 1.0f};

        std::string image; // chemin relatif au dossier des assets
        Math::LinearColor tint{1.0f, 1.0f, 1.0f}; // multiplie l'image
        bool keepAspect = true; // image entière dans le rectangle, proportions gardées (sinon étirée)

        // Fenêtre.
        float titleHeight = 40.0f; // 0 : pas de barre de titre
        Math::LinearColor titleColor{0.0f, 0.0f, 0.0f, 0.0f};
        bool shadow = true;
        bool movable = true;

        bool checked = false; // case
        float value = 0.5f;   // curseur, entre minimum et maximum
        float minimum = 0.0f;
        float maximum = 1.0f;

        std::vector<Element> children; // fenêtres et cadres seulement ; dessinés par-dessus leur parent
    };

    struct Document
    {
        std::vector<Element> roots; // dans l'ordre du dessin : le dernier par-dessus
        std::uint32_t nextId = 1;
    };

    // Noms affichés (« Fenêtre », « En haut à gauche »...), indexés par l'énumération.
    FLUX_API std::span<const std::string_view> GetTypeNames();
    FLUX_API std::span<const std::string_view> GetAnchorNames();
    FLUX_API std::span<const std::string_view> GetAlignNames();
    // Fenêtres et cadres reçoivent des enfants.
    FLUX_API bool IsContainer(ElementType type);

    // Réglage d'un élément, propre à certains types (le titre d'une fenêtre, la valeur d'un curseur) : la même table
    // sert à l'enregistrement et au panneau Propriétés de l'éditeur.
    struct Field
    {
        using Member = std::variant<std::string Element::*, Math::LinearColor Element::*, float Element::*, bool Element::*,
                                    TextAlign Element::*>;

        std::string_view key;   // dans le fichier
        std::string_view label; // dans l'éditeur
        Member member;
        std::uint32_t types = 0; // bit 1 << ElementType : types concernés
        bool image = false;      // chemin d'une image (choisi dans l'explorateur)
        float speed = 1.0f;      // nombres : variation par pixel glissé, bornes, décimales
        float min = 0.0f;
        float max = 100000.0f;
        int decimals = 0;

        bool AppliesTo(ElementType type) const { return (types >> static_cast<unsigned>(type)) & 1u; }
    };
    // Dans l'ordre d'affichage ; une clé peut revenir avec un autre libellé pour d'autres types (« Titre », « Libellé »).
    FLUX_API std::span<const Field> GetFields();

    // Réglages par défaut d'un type (taille, couleurs, texte), sans identifiant ni nom.
    FLUX_API Element MakeDefault(ElementType type);
    // Nouvel élément avec les réglages par défaut de son type (taille, couleurs, texte) ; prend l'identifiant suivant.
    FLUX_API Element MakeElement(Document& document, ElementType type);

    FLUX_API Element* Find(Document& document, std::uint32_t id);
    FLUX_API const Element* Find(const Document& document, std::uint32_t id);
    // Parent de l'élément (nullptr : à la racine, ou absent).
    FLUX_API Element* FindParent(Document& document, std::uint32_t id);
    // Retire l'élément (avec ses enfants) et le retourne.
    FLUX_API std::optional<Element> Remove(Document& document, std::uint32_t id);
    // Liste qui contient l'élément (racines ou enfants de son parent), et sa place dedans.
    FLUX_API std::vector<Element>* FindSiblings(Document& document, std::uint32_t id, std::size_t& index);
    // Copie profonde avec de nouveaux identifiants (dupliquer).
    FLUX_API Element CloneWithNewIds(Document& document, const Element& element);

    // Fichier .fluxui (JSON « flux.ui », couleurs linéaires [r, g, b, a]).
    FLUX_API std::string SaveToString(const Document& document);
    FLUX_API std::expected<Document, std::string> LoadFromString(std::string_view text);
    FLUX_API std::expected<void, std::string> SaveFile(const Document& document, const std::filesystem::path& path);
    FLUX_API std::expected<Document, std::string> LoadFile(const std::filesystem::path& path);

    struct Rect
    {
        Math::Vector2f min;
        Math::Vector2f max;

        bool Contains(Math::Vector2f point) const {
            return point.x >= min.x && point.y >= min.y && point.x < max.x && point.y < max.y;
        }
    };

    // Élément placé dans la vue, pour le dessin, la souris et l'éditeur.
    struct Placed
    {
        const Element* element = nullptr;
        Rect rect;   // pixels de la vue
        Rect parent; // rectangle du parent (la vue pour une racine)
        Rect clip;   // partie visible : découpée par les parents
        std::uint32_t parentId = 0;
        int depth = 0;
    };

    // Échelle de la vue : pixels de la vue par pixel de référence.
    FLUX_API float GetScale(Math::Vector2f viewMin, Math::Vector2f viewMax);
    // Rectangle de l'élément dans son parent.
    FLUX_API Rect Arrange(const Element& element, const Rect& parent, float scale);
    // Change ancre-position-taille pour que l'élément occupe rect (pixels de la vue) dans son parent : l'éditeur
    // déplace et redimensionne ainsi quelle que soit l'ancre.
    FLUX_API void SetRect(Element& element, const Rect& parent, float scale, const Rect& rect);
    // Point du parent où se pose l'ancre (le coin haut-gauche du parent pour Stretch).
    FLUX_API Math::Vector2f AnchorPoint(Anchor anchor, const Rect& parent);
    // Tous les éléments visibles, dans l'ordre du dessin (parent avant ses enfants). clip : partie de la vue où
    // ils peuvent se voir (l'éditeur montre l'écran dans une vue plus petite) ; sans, toute la vue.
    FLUX_API std::vector<Placed> Arrange(const Document& document, Math::Vector2f viewMin, Math::Vector2f viewMax,
                                         const std::optional<Rect>& clip = std::nullopt);
    // Élément visible sous le point, le plus en avant (0 : aucun).
    FLUX_API std::uint32_t FindAt(std::span<const Placed> placed, Math::Vector2f point);

    // Souris en jeu : ce que l'interface en fait (survol, clic, glisser).
    struct Pointer
    {
        Math::Vector2f position;
        bool down = false;
        bool pressed = false;
        bool released = false;
        bool enabled = true; // false : la souris est ailleurs (panneau de l'éditeur), rien n'est survolé
    };

    // État de l'interface en jeu, d'une frame à l'autre.
    struct Interaction
    {
        std::uint32_t hovered = 0; // bouton, case ou curseur survolé
        std::uint32_t pressed = 0; // élément tenu depuis le clic
        std::uint32_t clicked = 0; // bouton cliqué à cette frame
        bool wantsMouse = false;   // souris au-dessus de l'interface : le jeu doit ignorer ses clics
        Math::Vector2f dragStart;
        Math::Vector2f dragOrigin;
    };

    // Fait réagir l'interface à la souris (cases, curseurs, fenêtres déplacées) : modifie le document.
    FLUX_API void Update(Document& document, Interaction& interaction, Math::Vector2f viewMin, Math::Vector2f viewMax,
                         const Pointer& pointer);

    using TextureResolver = std::function<std::optional<UI::TextureInfo>(std::string_view imagePath)>;

    struct DrawOptions
    {
        TextureResolver textures{};
        const Interaction* interaction = nullptr; // survol et appui en jeu
        bool placeholders = false;                // éditeur : un cadre gris pour une image sans fichier
        std::optional<Rect> clip{};               // voir Arrange
    };

    // Dessine le document dans la vue min-max (découpé à elle).
    FLUX_API void Draw(const Document& document, UI::DrawList& list, Math::Vector2f viewMin, Math::Vector2f viewMax,
                       const DrawOptions& options = {});
}
