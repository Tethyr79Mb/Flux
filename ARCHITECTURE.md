# Architecture de Flux

Vue technique d'ensemble du moteur (FluxEngine), de l'éditeur (FluxEditor) et du jeu Ars Solaris. Pour les règles de
travail, voir `CLAUDE.md` ; pour ce qui reste à faire, `Engine/ROADMAP.md` et `Editor/ROADMAP.md`.

## Les quatre exécutables

```
                 ┌──────────────────────────────┐
                 │        FluxEngine.dll        │  rendu, scène, physique, son, UI, plateforme
                 └──────────────────────────────┘
                   ▲             ▲            ▲
                   │             │            │ lie
   ┌───────────────┴──┐   ┌──────┴──────┐  ┌──┴──────────────┐
   │ FluxEditor.exe   │   │  Game.exe   │  │ FluxTests.exe   │
   │ panneaux, gizmo, │   │ lance le    │  │ tests de chaque │
   │ annuler, Jouer   │   │ projet      │  │ domaine         │
   └────────┬─────────┘   └──────┬──────┘  └─────────────────┘
            │ charge (copie,      │ charge
            │ rechargée à chaud)  │
            ▼                     ▼
          ┌──────────────────────────┐
          │     GameModule.dll       │  code du jeu : FluxRegisterGame / FluxUnregisterGame
          └──────────────────────────┘
```

- **FluxEngine.dll** exporte seulement ce qui porte `FLUX_API`. Vulkan, Jolt, FreeType, miniaudio et stb restent privés :
  aucun en-tête public ne les expose. EnTT est public (le jeu manipule la scène directement).
- **GameModule.dll** enregistre ses composants, ses fonctions d'interface et sa mise à jour. L'éditeur charge une copie
  (`.hotreload/`) pour pouvoir recompiler la DLL pendant qu'il tourne. `GameModuleApiVersion` refuse une DLL compilée
  contre un autre moteur.
- **Game.exe** n'a qu'un `main` d'une ligne : `Flux::GameStartup` (`Core/GameStartup.hpp`) lit le projet et les
  réglages du joueur (`GameSettings`, `Reglages.json` à côté de l'exe : fenêtre, plein écran, V-Sync, volume, panneau Moteur), crée
  le moteur avec, charge GameModule.dll, ouvre la scène de démarrage et lance `Engine::Run` ; à la fermeture, il relit
  ces réglages sur le moteur et réécrit le fichier (créé dès le premier lancement ; fermer la console arrête aussi la
  boucle proprement, `Platform::CatchConsoleClose`). Comme dans Unreal, le moteur possède la boucle et
  appelle le jeu (même boucle que le mode Jouer de l'éditeur, condition du rechargement à chaud).
- **Game.fluxproject** décrit le projet : nom, dossier `assets/`, scène de démarrage. Les assets sont copiés à côté
  des exécutables (`bin/assets/`).
- Avec les préréglages de Visual Studio, exe, DLL, shaders et assets vont dans `Flux/bin/<préréglage>/` (le jeu prêt à
  lancer) ; `out/build/` garde les fichiers intermédiaires et `out/deps/` les dépendances téléchargées
  (`FLUX_OUTPUT_DIRECTORY`).

## Une image (`Engine::Run`)

1. Fenêtre : événements Win32, entrées clavier et souris.
2. FluxUI commence l'image (`UI::Context::BeginFrame`) ; la caméra suit le joueur ou vole librement (éditeur).
3. Si le jeu tourne et que la scène est prête (`UpdateGameSystems`) :
   - changement de scène demandé, actions d'entrée lues (`InputActions`) ;
   - sièges (`Seats::Update` : s'asseoir, se relever), puis vol des vaisseaux (`Ships::Update` : pilotage du
     vaisseau aux commandes, moteurs des autres coupés) ;
   - physique (`PhysicsWorld::Update`, pas fixe de 1/60 s) : personnages, objets simulés, vaisseaux, sol des planètes ;
   - armes, après la physique (le vaisseau est à sa place de l'image) : les projectiles avancent et touchent
     (`Weapons::GuideMissiles`, `UpdateProjectiles`, `ApplyHits`), puis les armes du vaisseau piloté visent et tirent
     (`Weapons::Update`) ;
   - animation (`Animator::Update`), après la physique ;
   - puis la mise à jour du module du jeu.
4. Caméra suiveuse placée, son 3D (`AudioSystem::Update`), mise à jour de l'éditeur s'il y en a un, puis les orbites
   (`Orbits::Update` : objets placés autour du Soleil, aussi quand le jeu est arrêté).
5. Interface à l'écran : textes, images, interfaces `.fluxui`, affichage de pilotage, version, panneau Moteur.
   Après une scène chargée en jeu, l'écran de chargement recouvre tout et le jeu attend (`Engine::IsLoading`) : la
   scène est quand même rendue, pour que les tuiles des planètes se calculent, jusqu'à ce que le rendu n'attende plus
   ni tuile ni texture du sol pendant 10 images (`RenderTimings::planetPendingTiles`, `groundTexturesLoading`).
6. Rendu (`Renderer::RenderFrame`), puis actions des interfaces et changement de scène en fin d'image.

## Rendu

Couches, de la plus haute à la plus basse :

```
Renderer           ordre des passes, mesures (CPU, attentes, timestamps GPU), étapes du diagnostic de plantage GPU
├─ MeshRenderer    modèles, tuiles de planète, ombres, lumières, données par image
├─ PlanetRenderer  choix des tuiles du quadtree ; PlanetTileCompute : relief calculé par le GPU
├─ SkyRenderer     ciel (nébuleuse cuite une fois dans une texture cube), éclairage ambiant
├─ ThrusterRenderer flammes des réacteurs : volume lumineux calculé au pixel dans une boîte, mélange additif
├─ TracerRenderer  traçantes des projectiles : bandes face à la caméra, un seul appel de dessin, mélange additif
├─ OverlayRenderer FluxUI : texte MSDF, formes SDF, images
├─ ModelCache      modèles glTF chargés une fois (GpuModel), matériaux
└─ RHI             Device, Buffer, Texture, Sampler, BindGroup, Pipeline, CommandList (interfaces virtuelles)
   └─ Vulkan       seul backend aujourd'hui (volk, vk-bootstrap, VMA)
```

Ordre des passes dans une image (2 images en vol, `RHI::FramesInFlight`) :

1. Préparation sur le CPU : ciel et soleil (direction de la caméra vers le `SunComponent` s'il y en a un, sinon
   hauteur et direction réglées dans le Ciel), objets visibles (élimination hors champ), tuiles de planète choisies, tri
   des opaques de près à loin et des transparents de loin à près.
2. Calculs GPU : tuiles de planète manquantes (compute), ciel recuit s'il a changé.
3. Ombres du soleil : 4 cascades dans une carte 4096² D32.
4. Scène, dans une image HDR (RGBA16Float, lumière linéaire) à la taille de la vue 3D avec sa propre profondeur
   (`PostRenderer`) : opaques, fond du ciel puis soleil (`Sun.slang` : disque assombri au bord, halo, rayons en étoile
   ou trait horizontal selon `SunComponent::rays`, bien plus lumineux que le blanc), océans, nuages, transparents,
   flammes des réacteurs (additives, sans tri). PBR métal/rugosité, lumières ponctuelles et spots, volumes intérieurs, atmosphère et océans dans `Mesh.slang`.
   Avec l'anticrénelage, la projection est décalée d'une fraction de pixel différente à chaque image (8 points de
   Halton, `TemporalView`), sauf le fond du ciel (étoiles d'un pixel) ; les opaques écrivent aussi leur mouvement
   propre (seconde cible RG16Float) : où le point était à l'image précédente moins où il serait resté immobile, nul
   pour le décor et pour un cockpit qui suit la caméra.
5. Anticrénelage temporel (`Temporal.slang`, Paramètres > Graphiques, activé par défaut) : chaque pixel est retrouvé
   dans le résultat de l'image précédente (profondeur + caméra d'avant, plus le mouvement propre), ramené dans la boîte
   de couleurs de son voisinage 3 x 3 (YCoCg, moyenne ± écart type) puis mélangé à 10 % de l'image actuelle. Deux
   historiques en alternance ; rien de repris après un saut de caméra de plus de 100 km ou un changement de taille.
6. Bloom (`Post.slang`, méthode de Jimenez) : l'image anticrénelée réduite de moitié jusqu'à 6 fois (13 lectures,
   moyenne de Karis à la première), puis remontée en additionnant chaque niveau flou au niveau au-dessus ; sans seuil.
7. Passe principale vers la fenêtre : la scène mélangée à son bloom (Paramètres > Graphiques, 4 % par défaut), puis
   l'exposition et la courbe de tons ACES ; l'interface (overlay) par-dessus. Le fond d'une scène sans ciel est
   effacé avec la couleur qui redonne exactement la couleur choisie après la courbe (`InverseToneMap`).

Shaders : `Engine/shaders/*.slang`, compilés en SPIR-V par CMake (`slangc`). Les données par image et par objet passent
par des buffers de stockage (`FrameData`, `DrawData`) ; leurs structures C++ et Slang doivent rester identiques
(`static_assert` sur les tailles). Un dessin donne l'indice de son `DrawData` comme indice d'instance.

## Coordonnées et grands mondes

- Les entités racines portent un `SectorComponent` (cube de 1e9 m, indices `int64`) et une position `double` dans
  leur secteur ; leurs enfants ont une position `double` relative au parent.
- `Math::WorldPosition` = secteur + décalage ; `Math::Delta` donne l'écart entre deux positions sans perte.
- Le rendu se fait relatif à la caméra : le GPU ne voit que des `float` proches de zéro. Profondeur en reverse-Z avec
  un plan lointain infini.
- Le monde physique se recentre sur le joueur au-delà de 1 km (Jolt travaille en `float` autour de son ancre).

## Scène et composants

- `Scene` enveloppe un registre EnTT, avec hiérarchie (parent, enfants, ordre), noms et `Uuid` stables.
- Chaque composant est déclaré une fois dans `ComponentRegistry.cpp` : nom de sauvegarde, libellé, champs et bornes.
  L'inspecteur de l'éditeur et la sauvegarde JSON (`flux.scene`) en sont générés. Le jeu enregistre les siens de la
  même façon.
- Le mode Jouer de l'éditeur copie la scène (aller-retour JSON) et la remet à la fin.
- Prefabs (`SceneSerializer`) : un `.prefab.json` est une racine et ses descendants. Dans une scène, un exemplaire
  (`PrefabInstanceComponent` sur sa racine, `PrefabLinkComponent` sur chaque pièce) est écrit comme sa racine, le
  chemin du prefab et, par pièce, les seuls champs qui en diffèrent (`overrides`, composants ajoutés ou retirés).
  Au chargement, les pièces sont refaites depuis le prefab puis les différences appliquées : ce qui change dans le
  prefab arrive partout, sauf là où un exemplaire l'a changé. Identifiant d'une pièce = celui du prefab mélangé à
  celui de l'exemplaire (`MemberId`, réversible). Lecture des prefabs par `Engine::GetPrefabReader` (textes gardés
  tant que le fichier ne change pas). Pas encore de prefab dans un prefab.

## Physique (Jolt)

- `PhysicsContext` (créé avec le moteur) : système Jolt, fils de travail, maillages de collision en cache. Un monde
  de moins de 128 corps actifs est calculé sur le fil principal (réveiller les fils coûtait plus que le calcul).
- `PhysicsWorld` (par scène lancée) : un monde Jolt pour la scène, plus un par vaisseau dans son propre repère. On
  marche dans un vaisseau qui vole parce que son intérieur est immobile dans son monde. Des copies cinématiques de
  son décor dans le monde extérieur permettent d'y monter.
- Vaisseaux à la racine : déplacés par `MoveShips` (lancers de leur enveloppe convexe contre le décor immobile du
  monde et les autres vaisseaux : leur coque et les copies de leur décor, donc les murs d'une station), avec
  stabilisateur d'atterrissage ; pas de corps dynamique. Sans pilote (« Tombe sans pilote », coché par défaut), un
  vaisseau qui a une collision tombe avec la gravité des planètes (rien dans l'espace, au-delà de deux rayons) ou,
  dans une scène sans planète, celle du monde ; freiné par l'air ; posé, il s'immobilise (freins serrés).
- Station : un vaisseau immobile (`ShipComponent` sans moteurs ni chute) ; sa gravité tient les personnages sur ses
  ponts. Ses quais (`DockComponent`, origine d'un vaisseau posé, sortie vers leur -z) servent au pilote automatique
  (`Docking`, après `Ships::Update`) : il règle la vitesse et la rotation du vaisseau le long d'un trajet calculé dans
  le repère du quai (par-dessus la station si on arrive de derrière, par-dessous si on est plus bas qu'elle ; point d'approche devant l'entrée, au-dessus du
  quai, demi-tour, descente), puis en fait un enfant de la station. La station devient son monde extérieur
  (`FindOuterGrid`) : les copies de sa coque y passent, et le personnage qui en descend passe dans la station
  (`TransferCharacters`). Un vaisseau garé sans pilote dans son hangar (dans la boîte de son enveloppe) tombe avec
  la gravité de son pont et se pose (`FindCarrierGravity`), même loin de toute planète.
- Missions : un PNJ porte `MissionGiverComponent` (« Donneur de missions ») ; F à sa portée ouvre sa fenêtre
  (`Engine::UpdateMissions`, `EngineMissions.cpp`). Une seule sorte : détruire un groupe d'ennemis, que
  `Missions::SpawnEnemies` fait apparaître à la racine, à `distance` devant la station (prefab du donneur, ou cubes
  rouges), avec des points de vie s'ils n'en ont pas. Le moteur garde une mission en cours (`m_mission`), finie quand
  plus aucun ennemi n'existe ; elle n'est pas enregistrée et s'efface au chargement d'une scène.
- Personnages : `CharacterVirtual` (capsule), gravité de la planète ou du vaisseau.
- Gravité et air hors planètes et vaisseaux : `WorldComponent` (« Monde », un par scène, comme les World Settings
  d'Unreal). L'air freine les objets simulés et la chute des personnages (dv/dt = -k·densité·|v|·v, k tiré de la
  vitesse de chute limite) et porte les vaisseaux ; dans l'atmosphère d'une planète, sa densité remplace celle du monde.
- Sol des planètes (`PlanetGround`) : tuiles de maillage au niveau de détail le plus fin, calculées sur le CPU en
  `double` autour des objets (sondes), sur un fil à part.

## Armes (`Weapons`)

- Un emport (`HardpointComponent`) est une entité du vaisseau ; l'arme (`WeaponComponent`) est son enfant direct. Une
  arme d'un autre genre ou plus grande que l'emport ne tire pas (`Weapons::Fits`).
- Tir par groupes : chaque arme a un groupe (`WeaponComponent::group`). Les actions « Groupe d'armes 1 » à « 10 »
  (touches 1 à 0) verrouillent la cible visée et activent ou arrêtent le groupe (`Engine::m_activeGroups`) ; les
  canons d'un groupe actif tirent seuls dès que la cible est à leur portée, un lance-missiles lance un missile puis
  son groupe s'arrête (`Weapons::Update` retourne le groupe). Perdre la cible arrête tout.
- Visée : la cible verrouillée, en avance (sa vitesse moins celle du vaisseau, sur le temps de vol des obus) ; sans
  verrou, le point regardé à 700 m devant la caméra. Ramenée dans le cône de l'arme, à sa vitesse de pivotement ;
  seule la rotation locale de l'arme change (pas de roulis). Une arme ne tire que si la cible verrouillée est à sa
  portée : sans verrou, rien ne part.
- Projectiles : un `std::vector<Projectile>` gardé par le moteur (des centaines par seconde : pas des entités),
  réservé d'avance ; un projectile part de la bouche du canon avec la vitesse du vaisseau en plus de la sienne. Les
  traçantes (`TracerRenderer`) en sont tirées à chaque image, positions relatives à la caméra.
- Impacts : chaque projectile lance le segment qu'il parcourt dans l'image contre tous les mondes physiques
  (`PhysicsWorld::CastRay` ; corps du tireur ignorés). La cible est l'entité touchée ou son ancêtre le plus proche
  qui a un `HealthComponent` ; à 0, elle est détruite avec ses enfants, et la physique retire ses corps à sa
  prochaine mise à jour. Le personnage du joueur protège ce qui le porte (fin de partie à venir).
- Missiles : des `Projectile` marqués `missile`, avec une cible. Le verrouillage (`Weapons::FindLockTarget`) se
  fait à l'appui sur une touche de tir : la cible à points de vie la plus proche du centre de la vue, dans l'angle et
  la portée des armes montées (parcours de tous les `HealthComponent` : linéaire, peu nombreux). Il tient ensuite hors
  du viseur tant que la cible vit et reste à portée (`KeepsLock`, à chaque image). Un groupe actif en
  lance un, du lance-missiles de ce groupe qui a le moins tiré. En vol, `GuideMissiles` tourne
  la vitesse vers la cible (en avance sur un vaisseau qui vole, d'après la vitesse de son `ShipComponent`), sans
  dépasser le virage du missile ni changer sa vitesse. Le nombre tiré (`fired`) n'est pas enregistré : la copie de la scène du mode Jouer
  repart pleine.

## Planètes

- Cube-sphère : 6 faces en quadtree, tuiles de 33×33 avec jupes, morphing CDLOD entre niveaux.
- Relief = une formule (`Terrain::GetHeight`), dupliquée dans `shaders/PlanetTile.slang` pour le GPU. Les deux
  versions doivent rester égales (`PlanetGpuTests`). Pas de terrain stocké ni creusable.
- Sol choisi au pixel dans `Mesh.slang` : climat (température, humidité), pentes, plages, neige ; 5 textures en
  tableaux (`GroundTextures`) plaquées en triplanaire, chaque projection en pavage hexagonal (rotation et décalage
  aléatoires par cellule, périodique sur `GroundPeriod` pour ne pas voir de raccord entre tuiles) ; couleurs unies si les textures manquent ou pendant leur
  décodage (sur d'autres fils, seulement si la scène a une planète).
- Atmosphère (diffusion calculée par sommet pour le sol et l'océan, au pixel pour les modèles et le ciel) et océan (tuiles sous le niveau de la mer redessinées en surface d'eau).
- Nuages (`CloudsComponent`) : une couche épaisse entre deux sphères (base et sommet), sans maillage. Un triangle
  plein écran (`fragmentClouds`) cherche l'entrée et la sortie de la couche sur le rayon de chaque pixel. Près de la
  caméra, il marche en 8 pas dans le volume (forme en cumulus, érosion, lumière du soleil traversée vers le haut) ;
  au loin et vu de l'espace, il lit une seule colonne de densité, moins d'octaves et moins d'érosion selon la
  taille d'un pixel, pour éviter le grain. Il écrit la profondeur du premier nuage touché, jamais plus près que la
  couche (`SV_DepthLessEqual`, le triangle posé à cette distance) : testée contre la scène sans être enregistrée, et
  les pixels cachés par le relief sont écartés avant le shader. La couverture s'arrête de lire ses octaves dès
  qu'elles ne peuvent plus donner de nuage. Leur ombre (`CloudShadow`) multiplie le soleil de tout ce qui est éclairé
  (objets, sol, eau).
- Sommets des tuiles rangés dans des pages de 256 cases (`PlanetTileCompute`) ; chaque passe (sol, eau, cascade
  d'ombres) dessine une page en un seul `DrawIndexedIndirect`. L'indice de la tuile dans les `DrawData` est celui de
  l'instance (`SV_VulkanInstanceID`), comme pour tous les maillages de `Mesh.slang`.

## Interface (FluxUI)

- Mode immédiat : `UI::Context` (panneaux ancrés, onglets, widgets, menus, arbres) produit une `UI::DrawList`
  dessinée par `OverlayRenderer`. Texte MSDF (police Arimo, atlas généré au démarrage), formes SDF.
- Interfaces du jeu : fichiers `.fluxui` (`Flux::Interface`, mode retenu) éditées dans l'éditeur, chargées par
  `InterfaceLibrary`, liées aux valeurs du jeu (`BindInterfaceValue`) et aux fonctions C++ du module.

## Éditeur

- `main.cpp` crée le moteur sans panneau Moteur ni interface plein écran, puis branche sa propre mise à jour.
- En haut : barre de menus puis barre d'outils (`BeginToolBar`, boutons à pictogramme et info-bulles) ; en bas, la
  barre d'état. `GetMenuBarHeight` compte les deux barres du haut.
- `SceneEditor` regroupe les panneaux : Scènes (hiérarchie), Inspecteur, Placer, Contenu, Caméra, Console, Paramètres.
  L'inspecteur d'une pièce de prefab marque « » » les champs qui diffèrent du prefab (FluxUI : `"libellé###clé"`
  garde le même widget quand le texte visible change).
- `SceneViewport` : vue 3D, sélection (`Picking`), gizmo, glisser-déposer.
- `SceneDocument` : fichier ouvert, annuler / refaire (état avant/après par objet), titre de la fenêtre.
- `Interface/` : éditeur des `.fluxui`.

## Fichiers

| Fichier | Rôle |
|---|---|
| `*.scene.json` | scène (`flux.scene` v2 : exemplaires de prefabs) |
| `*.prefab.json` | prefab (`flux.prefab`, même forme qu'une scène, une seule racine) |
| `*.fluxui` | interface du jeu (`flux.ui` v1) |
| `Game.fluxproject` | projet : nom, assets, scène de démarrage |
| `Touches.json` | touches réassignées (à côté de l'exe) |
| `FluxEditor.layout.v3.json`, `FluxEditor.settings.json` | disposition et réglages de l'éditeur |

## Compilation

- CMake + Ninja, préréglages MSVC (`CMakePresets.json`) : Debug, Release, et « Release + débogage » (RelWithDebInfo)
  pour tester vite en gardant les points d'arrêt. Dépendances par FetchContent dans `out/deps/<préréglage>`. Jolt est
  compilé optimisé même en Debug.
- `cmake/FluxSources.cmake` trouve les sources tout seul ; `FluxPrecompiled.cmake` les en-têtes précompilés ;
  `FluxResources.cmake` les shaders ; `FluxVersion.cmake` la version affichée (numéro de `project()` + commit Git).
- CI : `.github/workflows/msvc.yml`, Debug + Release, FluxTests.
- FreeType, msdfgen et stb_image (`cmake/stb`) sont compilés optimisés même en Debug : ils tournent au démarrage.
