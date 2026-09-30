#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>
#include <functional>
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

    struct Rect
    {
        Math::Vector2f min;
        Math::Vector2f max;

        Math::Vector2f GetSize() const { return max - min; }
    };

    // Zone d'ancrage : index dans la disposition, valable jusqu'à la prochaine SplitDock ou ClearDockLayout.
    using DockId = int;
    enum class DockSide { Left, Right, Top, Bottom };

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

        // Vrai si la souris survole un panneau, une zone ancrée (hors zone centrale) ou manipule un widget :
        // le reste du programme doit l'ignorer.
        bool WantsMouse() const;
        // Vrai pendant la saisie dans un champ : les raccourcis clavier du jeu et de la caméra doivent se taire.
        bool WantsKeyboard() const;
        // Vrai tant qu'un widget est tenu (valeur glissée, bouton enfoncé, panneau déplacé) ou qu'un champ est en
        // saisie : une modification en cours n'est pas finie (annuler / refaire en fait un seul pas).
        bool IsInteracting() const;
        // Vrai tant que le dernier widget décrit (bouton...) est tenu : sert à commencer un glisser-déposer.
        bool IsItemActive() const;
        // Rectangle du dernier widget interactif décrit, et souris au-dessus de lui même si un autre widget est tenu
        // (cible d'un glisser-déposer).
        Rect GetItemRect() const;
        bool IsMouseOverItem() const;
        // Liste de dessin du contenu du panneau en cours (repères de glisser-déposer...), ou nullptr hors panneau.
        DrawList* GetPanelDrawList();

        // Panneau déplaçable (par sa barre de titre) et repliable (clic sur la barre).
        // height 0 : hauteur ajustée au contenu ; sinon hauteur fixe, avec défilement (molette ou barre) si le contenu
        // dépasse. initialPosition ne sert qu'à la première apparition. Retourne false si le panneau est replié :
        // inutile alors de décrire son contenu. EndPanel doit être appelé dans tous les cas.
        bool BeginPanel(std::string_view title, Math::Vector2f initialPosition, float width = 320.0f, float height = 0.0f);
        void EndPanel();

        // Widgets : seulement entre BeginPanel et EndPanel.
        void Text(std::string_view text);
        void TextDisabled(std::string_view text);
        // Entre BeginDisabled et EndDisabled (imbricables), les widgets sont affichés en grisé et ne réagissent pas :
        // valeurs visibles mais non modifiables (la scène en mode Jouer).
        void BeginDisabled(bool disabled = true);
        void EndDisabled();
        // Libellé à gauche, texte à droite, alignés comme les champs.
        void LabelText(std::string_view label, std::string_view text);
        // Vrai au clic (bouton relâché sur le widget).
        bool Button(std::string_view label);
        // Onglet d'une rangée (les suivants avec SameLine) : l'appelant garde l'onglet affiché.
        //     if (ui.Tab("Graphiques", tab == 0)) tab = 0;
        // Vrai au clic.
        bool Tab(std::string_view label, bool selected);
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
        // Champ texte suivi d'un bouton « ... » (choisir un fichier) : browse devient true au clic sur le bouton,
        // à l'appelant d'ouvrir le choix et de modifier value.
        bool InputTextBrowse(std::string_view label, std::string& value, bool& browse);

        // Couleur linéaire éditée en sRGB 0-255 (les valeurs d'un sélecteur de couleur), avec un aperçu ;
        // un clic sur l'aperçu ouvre le sélecteur de couleur (SetColorPicker), s'il y en a un.
        bool ColorEdit3(std::string_view label, Math::LinearColor& color);
        // Idem, plus l'opacité (alpha, 0 : invisible, 255 : opaque), linéaire.
        bool ColorEdit4(std::string_view label, Math::LinearColor& color);

        // Entiers : même principe que les champs numériques, sans décimales.
        bool DragInt(std::string_view label, std::int32_t& value, float speed = 0.2f, double min = NoMin, double max = NoMax);
        // Entiers 64 bits (secteurs).
        bool DragInt3(std::string_view label, Math::Vector3i64& value, float speed = 0.2f);

        // Choix parmi quelques valeurs (énumération) : un clic ouvre la liste sous le champ, un clic sur une valeur
        // la choisit. Retourne true quand index change.
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
        // Un seul menu s'ouvre par clic : celui du premier widget décrit sous la souris (une vignette avant le fond
        // de sa zone).
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

        // Ancrage : une zone de l'écran découpée en rectangles par des séparateurs à glisser. Chaque rectangle
        // accueille un ou plusieurs panneaux (en onglets s'ils sont plusieurs) ; le rectangle central reste libre
        // (la vue 3D de l'éditeur). Construction, une fois :
        //     const DockId root = ui.ClearDockLayout();           // tout l'espace, central
        //     const auto [left, rest] = ui.SplitDock(root, DockSide::Left, 0.2f);
        //     ui.DockPanel("Hiérarchie", left);                   // rest reste central
        // puis à chaque frame, avant les panneaux :
        //     const Rect view = ui.DockSpace(zone);
        // Un panneau ancré garde son BeginPanel / EndPanel : sa position et sa taille viennent de sa zone, et
        // BeginPanel retourne false quand un autre onglet est affiché. Sans DockSpace dans la frame (Game.exe),
        // tous les panneaux flottent.
        struct DockSplit
        {
            DockId side; // la nouvelle zone, du côté demandé
            DockId rest; // le reste, qui garde les panneaux (et le rôle central) de la zone découpée
        };
        // Vide la disposition ; retourne la zone unique, centrale, qui couvre tout l'espace.
        DockId ClearDockLayout();
        // fraction : part de la zone donnée au côté demandé (entre 0 et 1).
        DockSplit SplitDock(DockId zone, DockSide side, float fraction);
        // Ajoute un panneau (par son titre, ##suffixe compris) aux onglets d'une zone non centrale.
        void DockPanel(std::string_view title, DockId zone);
        // Pose la disposition sur min-max (sous la barre de menus, en général), gère séparateurs et onglets.
        // Retourne le rectangle central.
        Rect DockSpace(Math::Vector2f min, Math::Vector2f max);
        // Disposition en JSON (proportions, panneaux et onglet affiché de chaque zone), pour la retrouver
        // au prochain lancement. Load retourne false (disposition inchangée) si le texte n'est pas valide.
        std::string SaveDockLayout() const;
        bool LoadDockLayout(std::string_view json);

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

        // Vignette d'une grille (explorateur de fichiers) : une icône, le nom dessous (raccourci s'il déborde).
        // Folder : un dossier jaune ; File : une page à coin corné, avec une bande de couleur portant un court badge
        // (« 3D », « Image »). Les suivantes avec SameLine tant que GetAvailableWidth le permet.
        enum class TileIcon { Folder, File };
        struct TileResult
        {
            bool clicked = false;
            bool doubleClicked = false;
        };
        TileResult Tile(std::string_view label, TileIcon icon, std::string_view badge, Math::LinearColor badgeColor,
                        bool selected, float width);

        // Zone de taille donnée dans le panneau, avec son propre défilement (molette au-dessus d'elle) : deux
        // colonnes qui défilent chacune de leur côté, par exemple. width ou height <= 0 : tout ce qui reste de la
        // ligne ou du panneau. Décrire son contenu puis appeler EndChild ; elle compte ensuite comme un widget
        // (SameLine, BeginContextMenu pour un clic droit dans son fond).
        void BeginChild(std::string_view id, float width, float height);
        void EndChild();
        // Largeur libre de la ligne en cours (panneau ou zone), pour ranger des vignettes en grille.
        float GetAvailableWidth() const;

        void Separator();
        // Le prochain widget se place à droite du précédent au lieu d'en dessous.
        void SameLine();

        // Portée d'identifiants : pour des widgets répétés dans une boucle (une ligne par entité, par exemple).
        void PushId(std::string_view id);
        void PushId(std::uint64_t id);
        void PopId();

        const Style& GetStyle() const;

        // Sélecteur de couleur ouvert par l'aperçu de ColorEdit3 (Engine y branche celui du système) :
        // modifie la couleur et retourne true, ou retourne false si l'utilisateur annule.
        using ColorPicker = std::function<bool(Math::LinearColor& color)>;
        void SetColorPicker(ColorPicker picker);

        private:
        bool ColorEdit(std::string_view label, Math::LinearColor& color, bool alpha);

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
