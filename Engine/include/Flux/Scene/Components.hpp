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
        // Lumière propre de la surface (écran, voyant, néon), ajoutée à l'éclairage : couleur x émission. 0 : aucune.
        // N'éclaire pas les objets voisins (il faut une lumière pour cela).
        float emission = 0.0f;
        // Surface : 0 plastique, peinture, bois ; 1 métal nu (sa couleur teinte ses reflets).
        float metallic = 0.0f;
        // 0 : lisse, reflets nets comme un miroir ; 1 : mate, reflets étalés.
        float roughness = 0.5f;
    };

    // Modèle 3D importé (glTF : .gltf ou .glb), placé par la transformation de l'entité. 1 unité du fichier = 1 mètre.
    struct ModelComponent
    {
        std::string model; // relatif au dossier des assets du projet, avec des « / » (models/Personnage.glb)
        Math::LinearColor tint{1.0f, 1.0f, 1.0f}; // multiplie les couleurs du modèle (blanc : inchangées)
        // Coché : métal et rugosité ci-dessous pour tout le modèle, à la place de ceux de ses matériaux (couleur,
        // relief et occlusion du fichier gardés).
        bool overrideMaterial = false;
        float metallic = 1.0f;
        float roughness = 0.3f;
    };

    // Lumière de contour (« rim light ») : les bords de l'objet vus de profil s'éclairent de cette couleur, comme en
    // contre-jour, ce qui détache sa silhouette du fond (un vaisseau sur le noir de l'espace). Agit sur l'entité et
    // sur tous ses enfants (sur la racine d'un vaisseau : tout le vaisseau). Un effet de rendu, pas une lumière :
    // n'éclaire rien autour, ne fait pas d'ombre et n'est pas arrêtée par elles.
    struct RimLightComponent
    {
        Math::LinearColor color{0.45f, 0.7f, 1.0f};
        float intensity = 1.0f;
        float sharpness = 4.0f; // 1 : large dégradé vers le centre ; 8 et plus : fin liseré sur les bords
    };

    enum class LightType : std::uint8_t {
        Point, // dans toutes les directions (ampoule)
        Spot,  // dans un cône, vers le -z de l'entité (projecteur, lampe de plafond)
    };

    // Lumière placée dans la scène : éclaire les maillages à moins de `range` mètres, en s'affaiblissant avec la
    // distance jusqu'à s'éteindre à `range`. Ne fait pas d'ombre : elle traverse les murs (réglez la portée).
    struct LightComponent
    {
        LightType type = LightType::Point;
        Math::LinearColor color{1.0f, 1.0f, 1.0f};
        // Éclairement d'une surface face à la lumière à 1 m : intensity / 2 (1 : le plein soleil) ; à 2 m, intensity / 5.
        float intensity = 5.0f;
        float range = 10.0f;     // mètres
        float angle = 60.0f;     // spot : ouverture totale du cône, en degrés
        float softness = 0.25f;  // spot : part du bord du cône qui s'estompe (0 : bord net, 1 : fondu depuis l'axe)
    };

    // Son joué pendant le jeu (muet dans l'éditeur à l'arrêt) : bruit, ambiance ou musique. Fichier WAV, OGG, MP3 ou
    // FLAC. En 3D, il vient de l'entité : plein volume jusqu'à minDistance, puis de plus en plus faible jusqu'au silence
    // à maxDistance, à gauche ou à droite selon la caméra. Sinon, même volume partout (musique, ambiance d'une scène).
    // Le code du jeu peut aussi le lancer et l'arrêter : voir AudioSystem.
    struct SoundComponent
    {
        std::string file;          // relatif aux assets
        float volume = 1.0f;       // 1 : celui du fichier
        float pitch = 1.0f;        // vitesse de lecture : 2 joue une octave plus haut et deux fois plus vite
        bool loop = false;
        bool playOnStart = true;   // dès le lancement du jeu ; sinon, quand le code du jeu le demande
        bool spatial = true;       // son 3D, placé sur l'entité
        float minDistance = 2.0f;  // mètres
        float maxDistance = 60.0f; // mètres : silence au-delà
        // Sur un vaisseau (ou un de ses enfants) : volume et hauteur montent avec sa vitesse (bruit des moteurs).
        bool followShipSpeed = false;
    };

    // Ciel de la scène, autour de tout et à l'infini : un fond, la lumière ambiante et ce que reflètent les surfaces
    // lisses, plus le soleil (lumière principale et ses ombres). Généré (nébuleuse, bande galactique, étoiles) ou
    // tiré d'une image panoramique. Un seul ciel compte ; sans ciel, pas de fond (couleur unie), une lumière ambiante
    // grise et le soleil par défaut.
    struct SkyComponent
    {
        // Panorama (image 2:1, projection équirectangulaire) relatif aux assets ; vide : ciel généré.
        std::string image;
        float brightness = 1.0f; // du fond comme de la lumière qu'il donne
        // Lumière ambiante et reflets : multiplie ce que le ciel éclaire (un ciel sombre éclaire peu).
        float ambient = 3.0f;
        // Ciel généré : le même pour une même graine.
        std::int32_t seed = 1;
        Math::LinearColor nebulaColor{0.30f, 0.10f, 0.45f};
        Math::LinearColor nebulaColor2{0.05f, 0.25f, 0.45f};
        float nebula = 0.6f;   // 0 : aucune nébuleuse, 1 : très dense
        float galaxy = 0.5f;   // bande de la galaxie (une traînée laiteuse sur un grand cercle)
        float stars = 0.5f;    // 0 : aucune étoile, 1 : beaucoup
        float starBrightness = 1.0f;
        // Soleil : d'où vient sa lumière, en degrés (hauteur au-dessus de l'horizon, direction autour de la verticale).
        float sunElevation = 63.0f;
        float sunAzimuth = 53.0f;
        Math::LinearColor sunColor{1.0f, 0.96f, 0.9f};
        float sunIntensity = 1.45f; // éclairement d'une surface face au soleil (1 : le blanc d'une surface blanche)
        float sunSize = 1.5f;       // diamètre apparent du disque, en degrés
    };

    // Planète en cube-sphère centrée sur l'entité (qui la fait tourner ; son échelle est ignorée) : une sphère de
    // `radius` mètres déformée par un relief généré (le même pour une même graine), plus détaillée près de la caméra
    // (Flux/Graphics/Planet.hpp). Pas encore de collision.
    struct PlanetComponent
    {
        float radius = 60000.0f; // m, au niveau zéro du relief
        float relief = 800.0f;   // m, hauteur des plus hautes montagnes (et profondeur des creux)
        std::int32_t seed = 1;
        Math::LinearColor color{0.42f, 0.38f, 0.33f};
        float roughness = 0.9f;
    };

    // Volume intérieur (cabine, couloir, station) : une boîte centrée sur l'entité, qui tourne et se déplace avec
    // elle. Dedans, la lumière ambiante et les reflets ne viennent plus du ciel (qui n'y entre pas) mais de `ambient`,
    // avec un fondu de `fade` mètres depuis les bords. Le soleil, lui, est arrêté par les ombres.
    struct InteriorComponent
    {
        Math::Vector3f size{2.0f, 2.5f, 4.0f}; // mètres, multipliés par l'échelle de l'entité
        Math::LinearColor ambient{0.25f, 0.25f, 0.27f};
        float intensity = 1.0f;
        float fade = 0.5f;
    };

    enum class ColliderShape : std::uint8_t {
        Box,     // size
        Sphere,  // radius
        Capsule, // radius et height, debout selon y
        Mesh,    // triangles d'un modèle glTF (mesh, sinon le modèle de l'entité) : décor ou coque, jamais simulé
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
        // Maillage : fichier .glb/.gltf relatif aux assets du projet, souvent une version simplifiée du modèle
        // (sols, parois, marches). Vide : le modèle affiché (ModelComponent) de l'entité.
        std::string mesh;

        bool simulatePhysics = false;
        // N'arrête que la caméra qui suit le joueur (verrière d'un cockpit) : personnages et objets la traversent.
        bool cameraOnly = false;
        float mass = 0.0f;          // kg, objet simulé ; 0 : d'après le volume (densité de 1000 kg/m³, celle de l'eau)
        float friction = 0.5f;
        float restitution = 0.0f;   // rebond : 0 aucun, 1 parfait
        float gravityFactor = 1.0f; // 0 : flotte (dans l'espace)
    };

    // Étape d'un personnage qui s'assoit (SeatComponent) : les animations « s'asseoir » et « se lever » se jouent
    // en entier avant qu'il soit assis ou debout.
    enum class SeatState : std::uint8_t {
        Standing,
        SittingDown,
        Seated,
        StandingUp,
    };

    // Personnage qui marche (Jolt CharacterVirtual) : une capsule debout dont le bas est l'origine de l'entité,
    // comme les pieds d'un modèle glTF. Doit être une racine ou un enfant direct d'un vaisseau (ShipComponent).
    // Le joueur le dirige au clavier pendant le jeu
    // (ZQSD, Maj pour courir, Espace pour sauter, F pour s'asseoir) et la caméra le suit.
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
        // Siège occupé pendant le jeu (non enregistrés) : tant qu'il en a un, le personnage y est posé, sans physique.
        Entity seat = NullEntity;
        SeatState seatState = SeatState::Standing;
        float seatTime = 0.0f; // secondes depuis le début de l'étape en cours
    };

    // Siège : le joueur s'y assoit (touche F, à moins de `reach` mètres de lui) et s'en relève (F). Son origine est
    // au sol, là où se posent les pieds du personnage, et il regarde vers son -z : le personnage assis aussi. Debout,
    // on en sort à un demi-mètre devant. Un siège d'un autre vaisseau que celui du joueur (ou du dehors) le fait
    // monter à bord : avec une grande portée, F près du vaisseau met directement au poste de pilotage. Le poste de pilotage dirige le vaisseau au-dessus de lui (ShipComponent) :
    // souris pour virer et cabrer, Z/S pour pousser en avant ou en arrière, Q/D pour glisser sur les côtés, Maj/Ctrl
    // pour monter et descendre, A/E pour rouler, Espace pour s'arrêter ; vue depuis l'œil du pilote (V : vue extérieure).
    struct SeatComponent
    {
        bool pilot = false;
        Math::Vector3f eye{0.0f, 1.3f, -0.35f}; // œil du personnage assis dans le repère du siège (vue cockpit)
        // Où le personnage se retrouve debout en se relevant, dans le repère du siège : un demi-mètre devant par
        // défaut (là où finit l'animation « se lever »), derrière dans un cockpit trop étroit pour passer devant.
        Math::Vector3f exit{0.0f, 0.0f, -0.5f};
        float reach = 1.5f; // m, distance du personnage au siège pour s'y asseoir
    };

    // Vaisseau ou station : un repère physique à lui. Ses descendants (sol, murs, caisses, personnages) sont simulés
    // dans son repère, avec une gravité artificielle vers son -y (le sol du pont) : on y marche pendant qu'il vole ou
    // tourne, sans glisser. Sa propre collision, s'il en a une, reste dans le repère de son parent (sa coque).
    // Pendant le jeu, il avance et tourne selon sa vitesse, que le pilote change depuis un poste de pilotage
    // (SeatComponent::pilot) ; la vitesse enregistrée est celle du départ.
    struct ShipComponent
    {
        float gravity = 9.81f;         // m/s², gravité artificielle ; 0 : apesanteur à bord
        Math::Vector3f velocity;       // m/s, dans les axes du vaisseau (-z : vers l'avant)
        Math::Vector3f angularVelocity; // °/s autour de ses axes (y : virer, x : tangage, z : roulis)
        float maxSpeed = 100.0f;       // m/s, en marche avant (le quart en arrière)
        float acceleration = 10.0f;    // m/s², moteurs à fond
        float turnRate = 30.0f;        // °/s, lacet, tangage et roulis commandés par le pilote
        float turnAcceleration = 360.0f; // °/s², la vitesse de rotation commandée est atteinte progressivement
        float strafeSpeed = 10.0f;     // m/s, glissement sur les côtés, montée et descente (décoller, se poser)
        // Pendant le jeu (non enregistrés) : posé sur le décor (ni rotation ni déplacement à plat), et vitesse de
        // croisière vers l'avant que le pilote a choisie (m/s ; négative : en arrière).
        bool grounded = false;
        float cruiseSpeed = 0.0f;
    };

    // Joue les animations du modèle de l'entité (ModelComponent) pendant le jeu ; dans l'éditeur, le modèle montre
    // la première image de son animation. Sur un personnage (CharacterComponent), le choix suit ses pas : repos
    // quand il est immobile, marche, puis course au-delà de la moitié entre ses vitesses de marche et de course,
    // chacune jouée d'autant plus vite qu'il va vite, « en l'air » pendant un saut ou une chute, et sur un siège :
    // s'asseoir (une fois), assis, se lever (une fois) ; fondu entre elles.
    struct AnimatorComponent
    {
        std::string animation{};     // nom dans le fichier (marche d'un personnage) ; vide : la première
        std::string idleAnimation{}; // personnage immobile ; vide : l'animation principale se fige
        std::string runAnimation{};  // personnage qui court ; vide : l'animation principale, accélérée
        std::string airAnimation{};  // personnage en l'air (saut, chute) ; vide : celle du sol continue
        std::string sitDownAnimation{}; // personnage qui s'assoit, jouée une fois ; vide : assis d'un coup
        std::string seatedAnimation{};  // personnage assis ; vide : celle du repos
        std::string standUpAnimation{}; // personnage qui se lève, jouée une fois ; vide : debout d'un coup
        float speed = 1.0f;          // vitesse de lecture (2 : deux fois plus vite)
        bool loop = true;            // sinon, s'arrête sur sa dernière image

        // Lecture en cours (non enregistrée) : animation jouée et son instant, animation qui s'efface, fondu.
        std::string playing{};
        double time = 0.0;
        bool playingOnce = false; // jouée une fois (s'asseoir, se lever), même si loop
        std::string previous{};
        double previousTime = 0.0;
        bool previousOnce = false;
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

    // Interface construite dans l'éditeur d'interface (fichier .fluxui : fenêtres, boutons, cases...), affichée à
    // l'écran par-dessus la 3D comme les textes et les images. Pendant le jeu, elle réagit à la souris.
    struct InterfaceComponent
    {
        std::string file; // chemin relatif au dossier des assets, avec des « / » (interfaces/Menu.fluxui)
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
