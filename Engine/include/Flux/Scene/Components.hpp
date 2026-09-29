#pragma once

#include "Flux/Core/Uuid.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Flux {
    using Entity = entt::entity;
    inline constexpr Entity NullEntity = entt::null;

    // Identifiant stable, sauvegardé : sert aux références entre entités (parent, cibles, undo).
    struct IdComponent
    {
        Uuid id;
    };

    struct NameComponent
    {
        std::string name;
    };

    // Transformation locale : relative au parent, ou au secteur pour une racine.
    // La position est en double pour rester précise jusqu'aux bords d'un secteur (1 million de km).
    struct TransformComponent
    {
        Math::Vector3d position;
        Math::Quaternionf rotation;
        Math::Vector3f scale{1.0f, 1.0f, 1.0f};
    };

    // Secteur d'une entité racine (absent : secteur 0). Voir Math::WorldPosition.
    struct SectorComponent
    {
        Math::Vector3i64 sector;
    };

    // Liens de la hiérarchie, en liste chaînée : aucune allocation par enfant. Les racines sont aussi chaînées entre elles.
    // Géré par Scene uniquement (CreateEntity, SetParent, DestroyEntity).
    struct HierarchyComponent
    {
        Entity parent = NullEntity;
        Entity firstChild = NullEntity;
        Entity lastChild = NullEntity; // ajout en fin de liste sans la parcourir
        Entity nextSibling = NullEntity;
        Entity previousSibling = NullEntity;
    };

    enum class MeshType : std::uint8_t {
        Cube, // cube unité centré
    };

    struct MeshRendererComponent
    {
        MeshType mesh = MeshType::Cube;
        Math::LinearColor color{1.0f, 1.0f, 1.0f};
    };

    // Modèle 3D importé (glTF : .gltf ou .glb), placé par la transformation de l'entité. 1 unité du fichier = 1 mètre.
    struct ModelComponent
    {
        std::string model; // relatif au dossier des assets du projet, avec des « / » (models/Personnage.glb)
        Math::LinearColor tint{1.0f, 1.0f, 1.0f}; // multiplie les couleurs du modèle (blanc : inchangées)
    };

    enum class ColliderShape : std::uint8_t {
        Box,     // size
        Sphere,  // radius
        Capsule, // radius et height, debout selon y
    };

    // Collision et physique (Jolt) d'une entité, pendant le jeu seulement. La forme est dans le repère de l'entité :
    // elle suit sa transformation, échelle comprise. Comme « Simulate Physics » d'Unreal : sans simulation, l'entité
    // est un décor immobile (sol, mur) ; avec, elle tombe et se cogne. Un objet simulé doit être une racine de la
    // hiérarchie (sans parent) ou un enfant direct d'un vaisseau (ShipComponent) : la physique réécrit sa position et
    // sa rotation.
    struct ColliderComponent
    {
        ColliderShape shape = ColliderShape::Box;
        Math::Vector3f size{1.0f, 1.0f, 1.0f}; // boîte : le cube unité par défaut, comme le maillage Cube
        float radius = 0.5f;
        float height = 2.0f; // capsule : hauteur totale, extrémités arrondies comprises
        Math::Vector3f center; // décalage du centre de la forme

        bool simulatePhysics = false;
        float mass = 0.0f;          // kg, objet simulé ; 0 : d'après le volume (densité de 1000 kg/m³, celle de l'eau)
        float friction = 0.5f;
        float restitution = 0.0f;   // rebond : 0 aucun, 1 parfait
        float gravityFactor = 1.0f; // 0 : flotte (dans l'espace)
    };

    // Personnage qui marche (Jolt CharacterVirtual) : une capsule debout dont le bas est l'origine de l'entité,
    // comme les pieds d'un modèle glTF. Doit être une racine ou un enfant direct d'un vaisseau (ShipComponent).
    // Le joueur le dirige au clavier pendant le jeu
    // (ZQSD, Maj pour courir, Espace pour sauter) et la caméra le suit.
    struct CharacterComponent
    {
        float radius = 0.3f;
        float height = 1.8f;     // taille totale
        float walkSpeed = 2.0f;  // m/s
        float runSpeed = 5.0f;
        float jumpSpeed = 4.5f;  // m/s vers le haut au départ du saut (environ 1 m de haut)
        bool player = true;      // dirigé par le joueur ; sinon, immobile (soumis à la gravité seulement)
        float groundSpeed = 0.0f; // vitesse horizontale mesurée pendant le jeu, m/s ; 0 en l'air (non enregistrée)
        float airTime = 0.0f;     // secondes passées en l'air (saut, chute) pendant le jeu ; 0 au sol (non enregistrée)
    };

    // Vaisseau ou station : un repère physique à lui. Ses descendants (sol, murs, caisses, personnages) sont simulés
    // dans son repère, avec une gravité artificielle vers son -y (le sol du pont) : on y marche pendant qu'il vole ou
    // tourne, sans glisser. Sa propre collision, s'il en a une, reste dans le repère de son parent (sa coque).
    // Pour l'essayer avant le pilotage : un mouvement constant pendant le jeu.
    struct ShipComponent
    {
        float gravity = 9.81f;         // m/s², gravité artificielle ; 0 : apesanteur à bord
        Math::Vector3f velocity;       // m/s, dans les axes du vaisseau (-z : vers l'avant)
        Math::Vector3f angularVelocity; // °/s autour de ses axes (y : virer, x : tangage, z : roulis)
    };

    // Joue les animations du modèle de l'entité (ModelComponent) pendant le jeu ; dans l'éditeur, le modèle montre
    // la première image de son animation. Sur un personnage (CharacterComponent), le choix suit ses pas : repos
    // quand il est immobile, marche, puis course au-delà de la moitié entre ses vitesses de marche et de course,
    // chacune jouée d'autant plus vite qu'il va vite, et « en l'air » pendant un saut ou une chute ; fondu entre elles.
    struct AnimatorComponent
    {
        std::string animation{};     // nom dans le fichier (marche d'un personnage) ; vide : la première
        std::string idleAnimation{}; // personnage immobile ; vide : l'animation principale se fige
        std::string runAnimation{};  // personnage qui court ; vide : l'animation principale, accélérée
        std::string airAnimation{};  // personnage en l'air (saut, chute) ; vide : celle du sol continue
        float speed = 1.0f;          // vitesse de lecture (2 : deux fois plus vite)
        bool loop = true;            // sinon, s'arrête sur sa dernière image

        // Lecture en cours (non enregistrée) : animation jouée et son instant, animation qui s'efface, fondu.
        std::string playing{};
        double time = 0.0;
        std::string previous{};
        double previousTime = 0.0;
        float fade = 1.0f; // part de playing dans la pose, de 0 à 1
    };

    // Point de l'écran (ou de la vue de l'éditeur) auquel un élément d'interface est accroché.
    enum class ScreenAnchor : std::uint8_t {
        TopLeft,
        Top,
        TopRight,
        Left,
        Center,
        Right,
        BottomLeft,
        Bottom,
        BottomRight,
    };

    // Texte collé à l'écran (titre, menu), dessiné par-dessus la 3D. La transformation de l'entité est ignorée.
    // Taille et décalage sont en pixels d'un écran de 1080 lignes (ScreenUi::ReferenceHeight) : le texte garde
    // la même place dans toutes les résolutions, et dans la vue de l'éditeur.
    struct ScreenTextComponent
    {
        std::string text = "Texte";
        float size = 64.0f;
        Math::LinearColor color{1.0f, 1.0f, 1.0f};
        ScreenAnchor anchor = ScreenAnchor::Center; // aussi le point du texte placé sur l'ancre (centre sur centre...)
        float offsetX = 0.0f; // vers la droite
        float offsetY = 0.0f; // vers le bas
    };

    // Manière dont une image à l'écran occupe la vue.
    enum class ScreenImageFit : std::uint8_t {
        Size,    // largeur et hauteur données (ou celles du fichier), placée par l'ancre et le décalage
        Stretch, // étirée sur toute la vue, proportions perdues
        Cover,   // couvre toute la vue en gardant ses proportions, le surplus est rogné (fond d'écran)
    };

    // Image collée à l'écran (logo, fond d'écran), dessinée par-dessus la 3D comme ScreenTextComponent, avec les
    // mêmes unités (pixels d'un écran de 1080 lignes).
    struct ScreenImageComponent
    {
        std::string image; // chemin relatif au dossier des assets du projet, avec des « / » (textures/logo.png)
        Math::LinearColor tint{1.0f, 1.0f, 1.0f}; // multiplie les couleurs de l'image (blanc : inchangées)
        float opacity = 1.0f;
        ScreenImageFit fit = ScreenImageFit::Size;
        // 0 : taille du fichier (1 pixel de l'image = 1 pixel de référence) ; une seule à 0 : proportions gardées.
        float width = 0.0f;
        float height = 0.0f;
        ScreenAnchor anchor = ScreenAnchor::Center;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
    };

    // Passe à une autre scène après un délai ou sur confirmation du joueur (écran de démarrage) : voir SceneChange.
    // Seulement pendant le jeu (Game.exe, ou le mode Jouer de l'éditeur).
    struct SceneChangeComponent
    {
        std::string scene;     // scène à charger, relative au dossier des assets (scenes/Demo.scene.json)
        float delay = 3.0f;    // secondes ; 0 : pas de délai
        bool onConfirm = true; // aussi au clic gauche, sur Entrée ou Espace
        float elapsed = 0.0f;  // temps écoulé depuis le chargement de la scène (non enregistré)
    };

    // Composants d'un fichier de scène dont le type n'est pas enregistré (module de jeu absent ou ancien) :
    // gardés tels quels (clé, texte JSON) et réécrits à la sauvegarde, pour ne jamais perdre de données.
    struct UnknownComponentsComponent
    {
        std::vector<std::pair<std::string, std::string>> components;
    };
}
