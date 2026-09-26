#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Flux {
    class Input;
}

namespace Flux::UI {
    class DrawList;
    class Font;

    using WidgetId = std::uint64_t;

    struct FLUX_API Style
    {
        float fontSize = 15.0f;
        float panelPadding = 8.0f;           // marge intérieure des panneaux
        Math::Vector2f framePadding{6.0f, 4.0f}; // autour du texte dans les boutons et les champs
        float itemSpacing = 4.0f;            // entre deux widgets
        float labelFraction = 0.38f;         // part de la largeur réservée aux libellés des champs

        Math::LinearColor text;
        Math::LinearColor textDisabled;
        Math::LinearColor panelBackground;
        Math::LinearColor panelBorder;
        Math::LinearColor titleBackground;
        Math::LinearColor titleBackgroundActive;
        Math::LinearColor frame;
        Math::LinearColor frameHovered;
        Math::LinearColor frameActive;
        Math::LinearColor button;
        Math::LinearColor buttonHovered;
        Math::LinearColor buttonActive;
        Math::LinearColor checkMark;
        Math::LinearColor selection;
        Math::LinearColor separator;
        Math::LinearColor axisX;
        Math::LinearColor axisY;
        Math::LinearColor axisZ;

        static Style Dark();
    };

    // Interface en mode immédiat : les widgets sont décrits à chaque frame par du code ordinaire,
    //     if (ui.Button("Sauver")) { ... }
    //     ui.Checkbox("V-Sync", vsync);
    // et lisent / modifient directement les variables du programme. Aucun arbre d'objets à synchroniser :
    // FluxUI ne garde que l'état qui ne peut pas vivre dans le programme (position des panneaux, champ en édition...).
    //
    // Chaque widget est identifié par son libellé, dans la portée du panneau et des PushId en cours.
    // Deux widgets de même libellé dans un panneau : ajouter un suffixe caché, "Position##soleil".
    class FLUX_API Context {
        public:
        explicit Context(const Font& font, const Style& style = Style::Dark());
        ~Context();

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        // mouseEnabled false : la souris sert à autre chose (caméra en vol libre), l'UI l'ignore.
        void BeginFrame(const Input& input, Math::Vector2f screenSize, double deltaSeconds, bool mouseEnabled = true);
        // Ajoute les panneaux de la frame à output, du plus lointain au plus proche.
        void EndFrame(DrawList& output);

        // Vrai si la souris survole un panneau ou manipule un widget : le reste du programme doit l'ignorer.
        bool WantsMouse() const;
        // Vrai pendant la saisie dans un champ : les raccourcis clavier du jeu et de la caméra doivent se taire.
        bool WantsKeyboard() const;

        // Panneau déplaçable (par sa barre de titre) et repliable (clic sur la barre).
        // height 0 : hauteur ajustée au contenu ; sinon hauteur fixe, avec défilement (molette ou barre) si le contenu
        // dépasse. initialPosition ne sert qu'à la première apparition. Retourne false si le panneau est replié :
        // inutile alors de décrire son contenu. EndPanel doit être appelé dans tous les cas.
        bool BeginPanel(std::string_view title, Math::Vector2f initialPosition, float width = 320.0f, float height = 0.0f);
        void EndPanel();

        // Widgets : seulement entre BeginPanel et EndPanel.
        void Text(std::string_view text);
        void TextDisabled(std::string_view text);
        // Libellé à gauche, texte à droite, alignés comme les champs.
        void LabelText(std::string_view label, std::string_view text);
        // Vrai au clic (bouton relâché sur le widget).
        bool Button(std::string_view label);
        // Retourne true quand la valeur vient de changer.
        bool Checkbox(std::string_view label, bool& value);

        // Champs numériques : glisser horizontalement pour changer la valeur (Maj : x10, Ctrl : /10),
        // ou cliquer pour la taper (Entrée valide, Échap annule ; la virgule est acceptée).
        // speed : variation par pixel ; decimals : chiffres affichés. Retournent true quand la valeur change.
        static constexpr double NoMin = std::numeric_limits<double>::lowest();
        static constexpr double NoMax = std::numeric_limits<double>::max();
        bool DragFloat(std::string_view label, float& value, float speed = 0.01f, double min = NoMin, double max = NoMax,
                       int decimals = 3);
        bool DragDouble(std::string_view label, double& value, double speed = 0.01, double min = NoMin, double max = NoMax,
                        int decimals = 3);
        bool DragFloat3(std::string_view label, Math::Vector3f& value, float speed = 0.01f, double min = NoMin,
                        double max = NoMax, int decimals = 3);
        bool DragDouble3(std::string_view label, Math::Vector3d& value, double speed = 0.01, double min = NoMin,
                         double max = NoMax, int decimals = 3);

        // Champ texte : cliquer pour éditer, Entrée ou clic ailleurs valide, Échap annule.
        // Retourne true quand une nouvelle valeur vient d'être validée (pas à chaque touche).
        bool InputText(std::string_view label, std::string& value);

        // Couleur linéaire éditée en sRGB 0-255 (les valeurs d'un sélecteur de couleur), avec un aperçu.
        bool ColorEdit3(std::string_view label, Math::LinearColor& color);

        // Entiers : même principe que les champs numériques, sans décimales.
        bool DragInt(std::string_view label, std::int32_t& value, float speed = 0.2f, double min = NoMin, double max = NoMax);
        // Entiers 64 bits (secteurs).
        bool DragInt3(std::string_view label, Math::Vector3i64& value, float speed = 0.2f);

        // Choix parmi quelques valeurs (énumération) : un clic passe à la suivante, clic droit à la précédente.
        // Retourne true quand index change.
        bool Choice(std::string_view label, int& index, std::span<const std::string_view> options);

        // Titre de section repliable (un composant dans l'inspecteur). Retourne true si la section est dépliée ;
        // pas de TreePop. defaultOpen ne sert qu'à la première apparition.
        bool CollapsingHeader(std::string_view label, bool defaultOpen = true);

        // Barre de menus en haut de l'écran, au-dessus de tous les panneaux (qui ne peuvent plus passer dessous) :
        //     if (ui.BeginMainMenuBar()) {
        //         if (ui.BeginMenu("Fichier")) {
        //             if (ui.MenuItem("Enregistrer", "Ctrl+S")) { ... }
        //             ui.EndMenu();
        //         }
        //         ui.EndMainMenuBar();
        //     }
        // Hors de tout panneau. Un clic sur un titre ouvre son menu ; un clic ailleurs ou Échap le referme.
        bool BeginMainMenuBar();
        void EndMainMenuBar();
        // Vrai si ce menu est ouvert : décrire ses éléments puis appeler EndMenu.
        bool BeginMenu(std::string_view label);
        void EndMenu();
        // Menu contextuel du dernier widget décrit, ouvert par un clic droit sur lui ; id le distingue des autres.
        // Vrai s'il est ouvert : décrire ses éléments (MenuItem) puis appeler EndMenu.
        bool BeginContextMenu(std::string_view id = "contexte");
        // Vrai au clic, qui referme le menu. shortcut n'est qu'affiché : le raccourci reste à gérer par l'appelant.
        bool MenuItem(std::string_view label, std::string_view shortcut = {}, bool enabled = true);
        // Bouton dans la barre de menus (après les menus, ou à la position donnée par SetMenuBarCursor) ;
        // highlighted : dessiné comme enfoncé (mode en cours). Vrai au clic.
        bool MenuBarButton(std::string_view label, bool highlighted = false);
        // Position horizontale du prochain élément de la barre (pour centrer des boutons, par exemple).
        void SetMenuBarCursor(float x);
        // Taille d'un texte dans la police de l'interface.
        Math::Vector2f MeasureText(std::string_view text) const;
        // Hauteur de la barre de menus (0 sans barre) : la place à laisser en haut de l'écran.
        float GetMenuBarHeight() const;

        // Nœud d'arbre (hiérarchie) : flèche pour déplier, clic sur le libellé pour sélectionner,
        // double-clic pour déplier aussi. Quand open est vrai, décrire les enfants puis appeler TreePop.
        struct TreeNodeResult
        {
            bool open = false;
            bool clicked = false;
            bool doubleClicked = false;
        };
        // defaultOpen ne sert qu'à la première apparition du nœud.
        TreeNodeResult TreeNode(std::string_view label, bool selected, bool leaf = false, bool defaultOpen = false);
        void TreePop();
        // Force l'état déplié du prochain TreeNode (pour montrer une sélection faite ailleurs, par exemple).
        void SetNextTreeNodeOpen(bool open);

        void Separator();
        // Le prochain widget se place à droite du précédent au lieu d'en dessous.
        void SameLine();

        // Portée d'identifiants : pour des widgets répétés dans une boucle (une ligne par entité, par exemple).
        void PushId(std::string_view id);
        void PushId(std::uint64_t id);
        void PopId();

        const Style& GetStyle() const;

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
