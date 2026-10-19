#pragma once

#include "Flux/Core/Uuid.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"

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
    // lisses, plus le soleil (lumière principale et ses ombres) s'il n'y a pas de SunComponent. Généré (nébuleuse, bande galactique, étoiles) ou
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

    // Réglages du monde de la scène (comme les World Settings d'Unreal) : la gravité et l'air hors des planètes et des
    // vaisseaux, qui ont les leurs. Un seul compte ; sans lui, comme ses valeurs par défaut.
    struct WorldComponent
    {
        float gravity = 9.81f; // m/s², vers le -y du monde ; 0 : apesanteur (l'espace)
        float air = 0.0f;      // densité de l'air : 0 aucun, 1 comme au sol sur Terre (le vaisseau y est porté)
        // Freinage par l'air, ici comme dans l'atmosphère des planètes : la vitesse de chute qu'un objet simulé ou un
        // personnage ne dépasse pas dans un air comme sur Terre, sous 9,81 m/s². Plus d'air ou moins de gravité :
        // il tombe moins vite.
        float terminalSpeed = 55.0f; // m/s (un parachutiste à plat)
    };

    // Soleil : l'étoile de la scène, un objet posé dans l'espace (loin : la Terre est à 150 millions de km du sien). Sa
    // lumière, ses ombres et son disque dans le ciel viennent de sa position : sur une planète, il est haut à midi d'un
    // côté, se lève ou se couche sur le bord, et il fait nuit de l'autre côté. Il remplace le soleil du Ciel (hauteur
    // et direction) ; un seul compte. Ni lui ni la planète ne bougent pendant le jeu : pas de cycle jour / nuit.
    // Reflets du soleil dans l'objectif de la caméra.
    enum class SunRays : std::uint8_t {
        Star,   // branches d'étoile fines autour du disque
        Streak, // long trait horizontal de la couleur du disque, comme dans EVE Online
        None,
    };

    struct SunComponent
    {
        Math::LinearColor color{1.0f, 0.96f, 0.9f};    // couleur du disque vu dans le ciel
        Math::LinearColor lightColor{1.0f, 1.0f, 1.0f}; // couleur de la lumière (blanche, comme celle du Soleil)
        float intensity = 1.45f; // éclairement d'une surface face à lui (1 : le blanc d'une surface blanche)
        float size = 0.53f;      // diamètre apparent du disque, en degrés (le Soleil vu de la Terre)
        SunRays rays = SunRays::Star;
    };

    // Orbite autour du Soleil de la scène (SunComponent), pour ranger un système autour de son étoile comme dans EVE
    // Online : l'objet (une planète, en général) est à `distance` millions de km du soleil, à `angle` degrés autour de
    // lui dans le plan horizontal (0 : vers +x) et `inclination` degrés au-dessus. Rien ne tourne pendant le jeu.
    // Distance nulle : prise de la position actuelle (Orbits::Update). Un objet à la racine seulement ; ce qui est posé
    // près d'une planète sans en être l'enfant ne la suit pas quand l'orbite change.
    struct OrbitComponent
    {
        double distance = 0.0;
        double angle = 0.0;
        double inclination = 0.0;
    };

    // Planète en cube-sphère centrée sur l'entité (qui la fait tourner ; son échelle est ignorée) : une sphère de
    // `radius` mètres déformée par un relief généré (le même pour une même graine), plus détaillée près de la caméra
    // (Flux/Planetology/Planet.hpp). Son sol est solide (personnages, objets simulés, vaisseaux qui se posent) et elle
    // attire vers son centre jusqu'à deux rayons de distance ; au-delà, c'est la gravité du monde (PhysicsWorld). Son
    // atmosphère colore le ciel et voile le lointain.
    struct PlanetComponent
    {
        float radius = 60000.0f; // m, au niveau zéro du relief
        float relief = 800.0f;   // m, hauteur des plus hautes montagnes (et profondeur des creux)
        std::int32_t seed = 1;
        // Sol, choisi au pixel (shaders/Mesh.slang, fragmentPlanet) : roche dans les pentes (color, roughness), plage
        // de sable jusqu'à beachHeight mètres au-dessus du niveau de la mer (elle suit la mer), neige au-dessus de
        // snowLevel (en % du relief, au-dessus du niveau zéro : elle suit quand le relief change). Ailleurs, le climat choisit (biomes) : la température baisse vers les pôles (axe y
        // de la planète) et en altitude (6,5 °C par km) ; l'humidité (bruit à grande échelle, plus forte près de la
        // mer) va de déserts (sable) à prairies (herbe) en passant par la terre nue ; le froid jaunit l'herbe
        // (toundra) puis la couvre de neige.
        // Avec les images du dossier Textures/Sol des assets (roche, herbe, terre, sable, neige), chaque matière est une
        // texture plaquée sur le relief et les couleurs ci-dessous ne servent plus (les teintes, si) ; sans elles,
        // couleurs unies.
        // Sans zones : roche partout (une lune).
        Math::LinearColor color{0.42f, 0.38f, 0.33f};
        float roughness = 0.9f;
        bool zones = true;
        Math::LinearColor grassColor{0.07f, 0.11f, 0.035f};
        Math::LinearColor sandColor{0.5f, 0.42f, 0.3f};
        Math::LinearColor snowColor{0.85f, 0.88f, 0.92f};
        Math::LinearColor soilColor{0.14f, 0.09f, 0.055f};
        // Teintes des textures du sol, multipliées à leur couleur sans changer leur luminosité : blanc (ou un gris) garde
        // l'image telle quelle, un jaune clair la dore. L'herbe est verdie : son image, brun olive en moyenne, se confond
        // de loin avec la roche et la terre.
        Math::LinearColor rockTint{1.0f, 1.0f, 1.0f};
        Math::LinearColor grassTint{0.25f, 1.0f, 0.2f};
        Math::LinearColor soilTint{1.0f, 1.0f, 1.0f};
        Math::LinearColor sandTint{1.0f, 1.0f, 1.0f};
        Math::LinearColor snowTint{1.0f, 1.0f, 1.0f};
        float temperature = 20.0f;      // °C à l'équateur, au niveau zéro
        float poleTemperature = -30.0f; // °C aux pôles
        float humidity = 0.5f;          // 0 : planète désertique, 1 : très humide
        float beachHeight = 10.0f; // m au-dessus du niveau de la mer (seaLevel)
        float snowLevel = 65.0f;
        // Océan : une surface lisse au niveau de la mer (seaLevel, en % du relief comme le sable), au-dessus du fond qui
        // garde son relief. Claire là où il est peu profond (on voit le fond), de la couleur de l'eau au large.
        bool ocean = true;
        float seaLevel = 0.0f;
        Math::LinearColor waterColor{0.01f, 0.045f, 0.07f};
        float gravity = 9.81f; // m/s², au niveau du sol ; décroît comme 1 / distance² au-dessus
        // Atmosphère (Flux/Planetology/Atmosphere.hpp) : ciel coloré vu du sol, brume au loin, halo vu de l'espace.
        float atmosphere = 1.0f;          // densité : 0 aucune, 1 comme sur Terre
        float atmosphereHeight = 30000.0f; // m au-dessus du sol, là où elle s'arrête (presque tout est dans le bas)
        Math::LinearColor skyColor{0.175f, 0.41f, 1.0f}; // ce que l'air diffuse le plus : bleu sur Terre, orangé sur Mars
    };

    // Végétation d'une planète (sur l'entité de son PlanetComponent) : des arbres semés par le GPU autour de la caméra,
    // toujours aux mêmes endroits (Planetology/VegetationRenderer.hpp). Seulement là où pousserait l'herbe du sol : pente
    // douce, assez humide, ni plage, ni neige, ni gel ; en forêts (cover : part de ces plaines couverte) plutôt
    // qu'éparpillés. Trois modèles du même arbre : détaillé de près, simplifié au-delà de lodDistance (il fait aussi les
    // ombres), imposteur (quelques plans) au-delà de farDistance, de plus en plus espacés, jusqu'à range ; plus loin,
    // la couleur des forêts sur le sol. Pas de collision.
    struct VegetationComponent
    {
        std::string model = "Modeles/Arbre/Arbre.glb";
        std::string lodModel = "Modeles/Arbre/Arbre-lod1.glb";
        std::string farModel = "Modeles/Arbre/Arbre-loin.glb";
        float density = 0.4f;     // part des places possibles occupée dans une forêt (une tous les 5,7 m au plus)
        float cover = 0.5f;       // part des plaines couverte de forêts
        float maxSlope = 25.0f;   // degrés : au-delà, pas d'arbre
        float minScale = 0.7f;    // taille de chaque arbre, tirée entre les deux (x le modèle)
        float maxScale = 1.3f;
        float lodDistance = 60.0f; // m
        float farDistance = 300.0f;
        float range = 3000.0f;    // m : plus aucun arbre au-delà (la couleur des forêts reste sur le sol)
    };

    // Nuages d'une planète (sur l'entité de son PlanetComponent) : une couche de `thickness` mètres d'épaisseur à
    // partir de `altitude` mètres au-dessus du niveau zéro, traversée en quelques pas par pixel (shaders/Mesh.slang,
    // fragmentClouds) : vus du sol comme de l'espace, ils font de l'ombre au sol, à l'eau et aux objets. Ils dérivent
    // vers l'est (autour de l'axe y de la planète). Trop peu de pas pour voler dedans de façon crédible.
    struct CloudsComponent
    {
        float altitude = 4000.0f;  // m au-dessus du niveau zéro du relief : la base des nuages
        float thickness = 1500.0f; // m, des plus épais
        float coverage = 0.5f;     // part du ciel couverte, de 0 (aucun) à 1 (couvert)
        float opacity = 0.9f;      // des nuages les plus épais
        float size = 8000.0f;      // m, taille des grandes formations
        float speed = 10.0f;       // m/s à l'équateur ; négatif : vers l'ouest
        Math::LinearColor color{1.0f, 1.0f, 1.0f};
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

    // Réacteur : la flamme qui sort de la tuyère, vers le +z de l'entité (l'arrière d'un vaisseau qui regarde vers
    // son -z), plus la lueur qu'elle jette autour. Son origine est au centre de la sortie de la tuyère ; width et
    // height donnent la taille de la sortie (une tuyère rectangulaire fait une flamme plate). Sur un vaisseau
    // (ShipComponent parent), elle suit la poussée de ses moteurs (ShipComponent::throttle) : au ralenti, courte et
    // pâle ; à fond (boost, mode voyage), longue et blanche au cœur. Un effet de rendu, sans collision.
    struct ThrusterComponent
    {
        Math::LinearColor color{0.25f, 0.5f, 1.0f};
        float width = 2.3f;      // m, sortie de la tuyère
        float height = 0.8f;     // m
        float length = 7.0f;     // m, longueur de la flamme à pleine poussée
        float intensity = 1.0f;  // éclat de la flamme
        // Poussée montrée quand les moteurs sont coupés (dans l'éditeur, vaisseau sans pilote) ; 0 : flamme éteinte.
        float idle = 0.15f;
        float light = 8.0f;      // intensité de la lumière jetée autour à pleine poussée (0 : aucune)
    };

    // Ce qu'un emport accepte, et ce qu'est une arme.
    enum class WeaponKind : std::uint8_t {
        Gun,     // canon : tire des projectiles en rafale
        Missile, // lance-missiles
    };

    // Point d'emport d'un vaisseau (« hardpoint ») : l'endroit du modèle où se fixe une arme, son -z vers l'avant du
    // tir. L'arme est une entité enfant (WeaponComponent) : changer d'arme, c'est remplacer cet enfant. Une arme d'un
    // autre genre, ou plus grande que l'emport, n'y tire pas.
    struct HardpointComponent
    {
        WeaponKind kind = WeaponKind::Gun;
        std::int32_t size = 1; // de 1 à 3 : la plus grande arme acceptée
    };

    // Arme posée sur un emport (HardpointComponent parent) d'un vaisseau. Pendant le jeu, aux commandes de ce
    // vaisseau, elle pivote vers le point que regarde le pilote (sans sortir de son cône autour de l'avant de
    // l'emport) et tire tant que la touche de son groupe est tenue (1 à 0 au-dessus des lettres). Son -z : le canon ; ses projectiles partent de
    // muzzle mètres devant son origine.
    struct WeaponComponent
    {
        WeaponKind kind = WeaponKind::Gun;
        std::int32_t size = 1;
        std::int32_t group = 1;         // groupe de tir : touche 1 à 9, 10 pour la touche 0 ; 0 : ne tire pas
        float fireRate = 8.0f;          // tirs par seconde
        float projectileSpeed = 1500.0f; // m/s, en plus de la vitesse du vaisseau
        float range = 2500.0f;          // m : au-delà, le projectile disparaît
        float damage = 10.0f;           // par projectile
        float cone = 20.0f;             // °, débattement autour de l'avant de l'emport (0 : tir fixe)
        float turnRate = 120.0f;        // °/s, vitesse de pivotement
        float muzzle = 1.0f;            // m, de l'origine de l'arme à la bouche du canon
        Math::LinearColor color{1.0f, 0.55f, 0.2f}; // couleur de la traçante
        // Missiles : nombre emporté (0 : illimité) et virage le plus serré en vol (°/s). Verrouillage : écart autour
        // du centre de la vue dans lequel une cible se verrouille à l'appui sur une touche de tir (°). Un missile part par appui sur la touche du groupe, du
        // lance-missiles de ce groupe qui en a tiré le moins ; la cadence est alors son temps de rechargement.
        std::int32_t ammo = 0;
        float guidance = 90.0f;
        float lockAngle = 10.0f;
        // Pendant le jeu (non enregistrés) : temps avant le prochain tir (s), missiles déjà tirés.
        float cooldown = 0.0f;
        std::int32_t fired = 0;
    };

    // Points de vie d'une cible (vaisseau, décor) : les projectiles qui touchent ses corps de collision, ou ceux de
    // ses enfants, les retirent ; à 0, elle est détruite avec ses enfants (sauf si elle porte le joueur : elle reste
    // à 0, la fin de partie viendra plus tard). Pendant le jeu, sur la copie de la scène : le fichier garde la valeur
    // de départ.
    struct HealthComponent
    {
        float health = 100.0f;
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

    // Étape du pilote automatique d'un vaisseau (Docking) : vers un quai (par-dessus la station si on arrive par
    // l'arrière ou le côté, ou par-dessous si on est plus bas qu'elle ; puis devant l'entrée, au-dessus du quai,
    // demi-tour, descente), ou en repartant (remonter, sortir).
    enum class AutopilotStage : std::uint8_t { Off, Climb, Over, Under, Approach, Enter, Turn, Descend, Rise, Exit };

    // Warp d'un vaisseau (Warp) : il se tourne vers sa destination en chargeant, puis y file sans collision et en sort
    // de lui-même à son arrivée.
    enum class WarpStage : std::uint8_t { Off, Charging, Cruising };

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
        // Aide au pilotage là où il y a une gravité (planète, sol) : sans roulis commandé, les ailes reviennent à plat
        // (ShipComponent::levelRate), et le lacet tourne autour de la verticale du lieu. Touche H en vol.
        bool levelAssist = true;
        float levelRate = 1.5f;        // 1/s : vitesse de retour à plat (1,5 : l'inclinaison fond de 78 % par seconde)
        // Dans l'atmosphère d'une planète : vitesse à laquelle l'air range sa vitesse derrière son nez (1/s, au sol,
        // moins haut où l'air est moins dense ; comme un avion porté par ses ailes). Dans l'espace, rien : il garde
        // son inertie.
        float airGrip = 2.0f;
        // Boost (Maj tenue) : vitesse max et accélération multipliées d'autant, le vaisseau fonce vers l'avant ;
        // relâché, il revient à sa vitesse max.
        float boost = 2.0f;
        // Mode voyage (Tab : allumer, éteindre) : monte en régime en travelSpoolTime secondes jusqu'à travelFactor fois
        // sa vitesse max et son accélération ; il vire alors plus lentement (le tiers) et sa vitesse suit son nez.
        // Dans l'espace et la haute atmosphère, de plus en plus bridé à mesure que l'air s'épaissit ; l'air épais (vers
        // 6 km d'altitude sur Terre), le frein ou un atterrissage le coupent.
        float travelFactor = 10.0f;
        float travelSpoolTime = 3.0f;
        // Warp (J, vers la destination visée) : pour les grandes distances, jusqu'à warpSpeed km/s ; 0 : pas de warp
        // (une station).
        float warpSpeed = 1.0e7f;
        // Sans pilote, il tombe avec la gravité du lieu (planète, monde) jusqu'à se poser ; décoché : il reste où il
        // est (station, vaisseau amiral en orbite). Seulement à la racine de la scène et avec une collision.
        bool fallsWithoutPilot = true;
        // Pendant le jeu (non enregistrés) : posé sur le décor (ni rotation ni déplacement à plat), et vitesse de
        // croisière vers l'avant que le pilote a choisie (m/s ; négative : en arrière).
        bool grounded = false;
        float cruiseSpeed = 0.0f;
        bool travel = false;     // mode voyage demandé
        float travelLevel = 0.0f; // régime du mode voyage, de 0 (éteint) à 1
        // Poussée des moteurs vers l'avant, de 0 (coupés) à 1 (boost, mode voyage) : flammes des réacteurs
        // (ThrusterComponent). Retombe à 0 sans pilote.
        float throttle = 0.0f;
        // Pilote automatique (Docking) : son étape, et le quai (DockComponent) réservé, ou occupé une fois amarré
        // (étape Off, le vaisseau enfant de la station).
        AutopilotStage autopilot = AutopilotStage::Off;
        Entity dock = NullEntity;
        // Warp en cours : son étape, sa destination, le rayon autour d'elle où il en sort (le haut de l'atmosphère d'une
        // planète, quelques km d'une station) et le temps déjà chargé (s).
        WarpStage warp = WarpStage::Off;
        Math::WorldPosition warpTarget;
        double warpExit = 0.0;
        float warpCharge = 0.0f;
    };

    // Quai d'une station (entité enfant d'un vaisseau immobile) : là où un vaisseau s'amarre. Son origine est celle
    // du vaisseau posé (pour le Chasseur, son plancher de cockpit, 2,3 m au-dessus de la dalle), son -z la direction
    // de la sortie : le vaisseau amarré a son nez tourné vers elle, prêt à repartir. Le pilote automatique arrive par
    // l'avant du quai (approach mètres devant, à hover mètres de haut), s'arrête au-dessus, fait demi-tour et descend.
    struct DockComponent
    {
        float approach = 100.0f;  // m devant le quai : point d'approche, hors de la station
        float hover = 6.0f;       // m au-dessus du quai : hauteur de l'approche et du demi-tour
        float clearance = 150.0f; // m au-dessus du quai : on passe par-dessus la station quand on arrive de derrière
        // Pendant le jeu : vaisseau qui l'a réservé ou qui y est amarré.
        Entity ship = NullEntity;
    };

    // État d'un vaisseau mené par l'IA de combat (CombatAI).
    enum class CombatState : std::uint8_t {
        Patrol, // tourne autour de son point de départ, en attendant de voir la cible
        Attack, // fonce sur elle et tire quand elle est devant lui
        Break,  // rompt le combat un instant (passé trop près, ou touché s'il est prudent), puis revient
        Flee,   // presque détruit : s'enfuit à toute vitesse
    };

    // IA de combat d'un vaisseau ennemi (CombatAI) : il attaque le vaisseau du joueur avec ses armes (emports). Les
    // réglages font des pirates prudents ou kamikazes sans code.
    struct CombatAIComponent
    {
        float detectionRange = 6000.0f; // m : il voit la cible et l'attaque ; à une fois et demie, il la perd
        float preferredRange = 800.0f;  // m : distance de combat ; plus loin il fonce, plus près il ralentit
        // 0 : prudent (rompt tôt, rompt quand il est touché, tire peu de missiles) ; 1 : kamikaze (colle à la cible).
        float aggression = 0.5f;
        float inaccuracy = 1.0f;    // °, erreur de visée qui va et vient (1° : 17 m à 1 km)
        float fleeHealth = 20.0f;   // % de ses points de vie de départ : en dessous, il fuit
        float patrolRadius = 300.0f; // m, cercle autour de son point de départ
        // Pendant le jeu (non enregistrés).
        CombatState state = CombatState::Patrol;
        float stateTime = 0.0f;      // s dans l'état
        float clock = 0.0f;          // s depuis le début : fait varier l'erreur de visée
        float startHealth = -1.0f;   // points de vie au premier passage (-1 : pas encore lus)
        float lastHealth = 0.0f;     // à l'image d'avant : touché s'ils baissent
        float missileCooldown = 0.0f; // s avant de pouvoir lancer un missile
        bool firing = false;         // la cible est devant lui : il tire (CombatAI::Fire)
        bool hasHome = false;
        Math::WorldPosition home;
        Math::Vector3f breakDirection; // monde : où il file en rompant
    };

    // Donneur de missions (un personnage non joueur) : à moins de reach mètres, F ouvre sa fenêtre de mission. Une
    // seule sorte de mission pour l'instant : détruire un groupe d'ennemis qui apparaît à distance mètres devant la
    // station où il se trouve (le -z de son vaisseau ; sans vaisseau, le sien), répartis sur un cercle de spread mètres.
    // Chaque ennemi est un exemplaire de enemyPrefab (un vaisseau qui n'a pas de points de vie en reçoit) ; sans prefab,
    // un cube rouge à abattre.
    struct MissionGiverComponent
    {
        std::string title = "Chasse aux pirates";
        std::string briefing = "Des pirates rôdent près de la station. Détruisez tout le groupe.";
        std::string enemyPrefab;     // relatif aux assets (.prefab.json)
        std::int32_t enemyCount = 3;
        float distance = 500000.0f;  // m, de la station au centre du groupe : loin, on y va en warp
        float spread = 60.0f;        // m, rayon du cercle des ennemis
        float reach = 2.5f;          // m, du joueur au donneur pour lui parler
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
        // Police du projet (.ttf, .otf), chemin relatif au dossier des assets (« Polices/Orbitron.ttf ») ; vide, ou
        // fichier illisible : Arimo, la police de l'interface.
        std::string font;
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

    // Exemplaire d'un prefab (fichier .prefab.json des assets : un objet tout monté, un vaisseau avec ses pièces), sur
    // sa racine. Ses pièces (PrefabLinkComponent) viennent du fichier à chaque chargement de la scène : modifier le
    // prefab modifie tous ses exemplaires. La scène n'enregistre que ce qui diffère du prefab (position, réglages
    // changés sur cet exemplaire, entités et composants ajoutés).
    struct PrefabInstanceComponent
    {
        std::string prefab; // relatif aux assets
    };

    // Pièce d'un exemplaire de prefab (sa racine comprise) : l'entité du prefab dont elle vient. Son identifiant dans
    // la scène est tiré de celui de l'exemplaire et de `source` : le même à chaque chargement.
    struct PrefabLinkComponent
    {
        Entity instance = NullEntity; // racine de l'exemplaire (PrefabInstanceComponent)
        Uuid source;                  // identifiant de l'entité dans le fichier du prefab
    };

    // Composants d'un fichier de scène dont le type n'est pas enregistré (module de jeu absent ou ancien) :
    // gardés tels quels (clé, texte JSON) et réécrits à la sauvegarde, pour ne jamais perdre de données.
    struct UnknownComponentsComponent
    {
        std::vector<std::pair<std::string, std::string>> components;
    };
}
