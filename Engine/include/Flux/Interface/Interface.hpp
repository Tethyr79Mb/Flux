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
// bouton, case à cocher, curseur, barre de progression, onglets) placés par une ancre dans leur parent, en pixels d'un écran de 1080 lignes comme
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
        Tabs,     // barre d'onglets : chaque enfant est une page, nommée sur son onglet ; une seule page affichée
        ProgressBar, // remplie selon sa valeur (vie, chargement) ; le joueur ne la change pas
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

    // Ce que fait un bouton cliqué en jeu. Les fenêtres sont désignées par leur nom (target).
    enum class ButtonAction : std::uint8_t {
        None,
        OpenWindow,   // l'affiche et la met devant les autres
        CloseWindow,
        ToggleWindow, // l'ouvre si elle est fermée, la ferme sinon
        CloseParent,  // ferme la fenêtre qui contient le bouton (« Retour », « Annuler »)
        ChangeScene,  // target : chemin de la scène, relatif au dossier des assets
        Quit,         // quitte le jeu (dans l'éditeur : arrête le mode Jouer)
        CallFunction, // target : nom d'une fonction C++ enregistrée par le jeu (Engine::RegisterInterfaceFunction)
        ShowTab,      // target : nom d'une page d'onglets, affichée à la place des autres pages (« Suivant »)
        RebindKey,    // target : nom d'une action d'entrée ; la prochaine touche enfoncée devient la sienne
        ResetKeys,    // remet toutes les touches par défaut
        Count,
    };

    struct Element
    {
        std::uint32_t id = 0; // unique dans le document, jamais réutilisé
        std::string name;     // pour s'y retrouver dans l'arbre, et le désigner (actions des boutons, code du jeu)
        ElementType type = ElementType::Frame;
        Anchor anchor = Anchor::Center;
        // Ancre ordinaire : décalage depuis l'ancre (vers la droite et le bas) et taille.
        // Stretch : marges gauche et haut (position), droite et bas (size).
        Math::Vector2f position;
        Math::Vector2f size{100.0f, 100.0f};
        bool visible = true; // fenêtre : ouverte au démarrage ; page d'onglets : la page affichée (la première visible)

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

        // Fenêtre (et onglets : hauteur de la barre, fond d'un onglet).
        float titleHeight = 40.0f; // 0 : pas de barre de titre
        Math::LinearColor titleColor{0.0f, 0.0f, 0.0f, 0.0f};
        bool shadow = true;
        bool movable = true;
        bool closeButton = false;   // croix dans la barre de titre
        bool closeOnEscape = true;  // Échap ferme la fenêtre ouverte la plus en avant qui l'accepte
        bool openOnEscape = false;  // ... et sinon ouvre celle-ci (menu pause)
        bool pausesGame = false;    // le jeu est en pause tant qu'elle est ouverte

        bool checked = false; // case
        float value = 0.5f;   // curseur, entre minimum et maximum
        float minimum = 0.0f;
        float maximum = 1.0f;

        ButtonAction action = ButtonAction::None; // bouton
        std::string target;                       // fenêtre, scène ou fonction visée par l'action

        // Liaison : nom d'une valeur du moteur ou du jeu (Engine::BindInterfaceValue). En jeu, la case, le curseur,
        // la barre ou le texte l'affichent ; la case et le curseur la changent quand le joueur les touche.
        std::string binding;
        // En jeu seulement (pas enregistré) : la valeur liée d'un texte, mise à la place de « {} » dans son texte
        // (ou à la place du texte entier s'il n'en a pas).
        std::optional<std::string> boundText;

        std::vector<Element> children; // fenêtres, cadres et onglets seulement ; dessinés par-dessus leur parent
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
    FLUX_API std::span<const std::string_view> GetActionNames();
    // L'action vise une fenêtre par son nom (ouvrir, fermer, basculer).
    FLUX_API bool TargetsWindow(ButtonAction action);
    // Fenêtres, cadres et onglets reçoivent des enfants.
    FLUX_API bool IsContainer(ElementType type);
    // L'action vise une page d'onglets par son nom.
    FLUX_API bool TargetsTab(ButtonAction action);

    // Réglage d'un élément, propre à certains types (le titre d'une fenêtre, la valeur d'un curseur) : la même table
    // sert à l'enregistrement et au panneau Propriétés de l'éditeur.
    struct Field
    {
        using Member = std::variant<std::string Element::*, Math::LinearColor Element::*, float Element::*, bool Element::*,
                                    TextAlign Element::*, ButtonAction Element::*>;
        // Texte libre, chemin d'une image (choisi dans l'explorateur), ou cible d'une action (fenêtre, scène,
        // fonction : l'éditeur propose selon l'action).
        // Liaison : nom d'une valeur (l'éditeur propose celles du moteur et du jeu).
        enum class Text : std::uint8_t { Plain, Image, ActionTarget, Binding };

        std::string_view key;   // dans le fichier
        std::string_view label; // dans l'éditeur
        Member member;
        std::uint32_t types = 0; // bit 1 << ElementType : types concernés
        Text text = Text::Plain; // chaînes seulement
        float speed = 1.0f;      // nombres : variation par pixel glissé, bornes, décimales
        float min = 0.0f;
        float max = 100000.0f;
        int decimals = 0;

        bool AppliesTo(ElementType type) const { return (types >> static_cast<unsigned>(type)) & 1u; }
    };
    // Dans l'ordre d'affichage ; une clé peut revenir avec un autre libellé pour d'autres types (« Titre », « Libellé »).
    FLUX_API std::span<const Field> GetFields();

    // Texte affiché par l'élément : son texte, ou la valeur liée (boundText) à la place de « {} ».
    FLUX_API std::string GetShownText(const Element& element);

    // Réglages par défaut d'un type (taille, couleurs, texte), sans identifiant ni nom.
    FLUX_API Element MakeDefault(ElementType type);
    // Nouvel élément avec les réglages par défaut de son type (taille, couleurs, texte) ; prend l'identifiant suivant.
    FLUX_API Element MakeElement(Document& document, ElementType type);

    FLUX_API Element* Find(Document& document, std::uint32_t id);
    FLUX_API const Element* Find(const Document& document, std::uint32_t id);
    // Parent de l'élément (nullptr : à la racine, ou absent).
    FLUX_API Element* FindParent(Document& document, std::uint32_t id);
    // Premier élément de ce nom (parcours en profondeur, dans l'ordre du fichier) ; nullptr si aucun.
    FLUX_API Element* FindByName(Document& document, std::string_view name);
    FLUX_API const Element* FindByName(const Document& document, std::string_view name);
    // Ouvre (la met aussi devant les autres fenêtres) ou ferme la fenêtre de ce nom ; false si aucune fenêtre ne
    // porte ce nom.
    FLUX_API bool SetWindowOpen(Document& document, std::string_view name, bool open);
    // Affiche la page d'onglets de ce nom (les autres pages de ses onglets sont masquées) ; false si aucune page
    // d'onglets ne porte ce nom.
    FLUX_API bool ShowTab(Document& document, std::string_view pageName);
    // Page affichée par des onglets : la première visible, sinon la première ; -1 sans page.
    FLUX_API int GetShownTab(const Element& tabs);
    // Une fenêtre ouverte (et ses parents visibles) met le jeu en pause.
    FLUX_API bool IsPausingGame(const Document& document);
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
    // Onglets : la barre (en haut de rect), l'onglet index (largeurs égales), la zone des pages (sous la barre), et
    // l'onglet sous le point (-1 : aucun).
    FLUX_API Rect GetTabBar(const Element& tabs, const Rect& rect, float scale);
    FLUX_API Rect GetTabRect(const Element& tabs, const Rect& rect, float scale, int index);
    FLUX_API Rect GetTabContent(const Element& tabs, const Rect& rect, float scale);
    FLUX_API int FindTabAt(const Element& tabs, const Rect& rect, float scale, Math::Vector2f point);
    // Point du parent où se pose l'ancre (le coin haut-gauche du parent pour Stretch).
    FLUX_API Math::Vector2f AnchorPoint(Anchor anchor, const Rect& parent);
    // Tous les éléments visibles, dans l'ordre du dessin (parent avant ses enfants). clip : partie de la vue où
    // ils peuvent se voir (l'éditeur montre l'écran dans une vue plus petite) ; sans, toute la vue. reveal : un
    // élément montré même s'il est masqué, avec les parents qui le contiennent (l'éditeur montre la fenêtre fermée
    // qu'on a sélectionnée).
    FLUX_API std::vector<Placed> Arrange(const Document& document, Math::Vector2f viewMin, Math::Vector2f viewMax,
                                         const std::optional<Rect>& clip = std::nullopt, std::uint32_t reveal = 0);
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
        bool escape = false; // touche Échap appuyée à cette frame
    };

    // État de l'interface en jeu, d'une frame à l'autre.
    struct Interaction
    {
        std::uint32_t hovered = 0; // bouton, case ou curseur survolé
        std::uint32_t pressed = 0; // élément tenu depuis le clic
        std::uint32_t clicked = 0; // bouton cliqué à cette frame
        std::uint32_t changed = 0; // case ou curseur changé par le joueur à cette frame
        bool wantsMouse = false;   // souris au-dessus de l'interface : le jeu doit ignorer ses clics
        std::uint32_t closeHovered = 0; // fenêtre dont la croix est survolée
        bool closePressed = false;      // pressed est une fenêtre tenue par sa croix (pas par sa barre de titre)
        int hoveredTab = -1;            // onglet survolé des onglets hovered
        int pressedTab = -1;            // onglet tenu des onglets pressed
        Math::Vector2f dragStart;
        Math::Vector2f dragOrigin;
        // Résultats de la frame : action d'un bouton cliqué que l'interface ne fait pas elle-même (changer de
        // scène, quitter, fonction C++), Échap utilisée (une fenêtre fermée ou ouverte), jeu en pause.
        ButtonAction action = ButtonAction::None;
        std::string target;
        bool escapeUsed = false;
        bool pausesGame = false;
    };

    // Fait réagir l'interface à la souris et à Échap (cases, curseurs, fenêtres déplacées, ouvertes, fermées, mises
    // devant les autres) : modifie le document. Les fenêtres à la racine forment la pile : la dernière est devant.
    FLUX_API void Update(Document& document, Interaction& interaction, Math::Vector2f viewMin, Math::Vector2f viewMax,
                         const Pointer& pointer);

    using TextureResolver = std::function<std::optional<UI::TextureInfo>(std::string_view imagePath)>;

    struct DrawOptions
    {
        TextureResolver textures{};
        const Interaction* interaction = nullptr; // survol et appui en jeu
        bool placeholders = false;                // éditeur : un cadre gris pour une image sans fichier
        std::optional<Rect> clip{};               // voir Arrange
        std::uint32_t reveal = 0;                 // voir Arrange
    };

    // Dessine le document dans la vue min-max (découpé à elle).
    FLUX_API void Draw(const Document& document, UI::DrawList& list, Math::Vector2f viewMin, Math::Vector2f viewMax,
                       const DrawOptions& options = {});
}
