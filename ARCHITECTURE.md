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
   ni tuile, ni texture du sol, ni modèle pendant 10 images (`RenderTimings::planetPendingTiles`,
   `groundTexturesLoading`, `modelsLoading`).
6. Rendu (`Renderer::RenderFrame`), puis actions des interfaces et changement de scène en fin d'image.

## Rendu

Couches, de la plus haute à la plus basse :

```
Renderer           ordre des passes, mesures (CPU, attentes, timestamps GPU seulement panneau Moteur ouvert),
                   étapes du diagnostic de plantage GPU
├─ MeshRenderer    modèles, tuiles de planète, ombres, lumières, données par image
├─ PlanetRenderer  choix des tuiles du quadtree ; PlanetTileCompute : relief calculé par le GPU
├─ SkyRenderer     ciel (nébuleuse cuite une fois dans une texture cube), éclairage ambiant
├─ ThrusterRenderer flammes des réacteurs : volume lumineux calculé au pixel dans une boîte, mélange additif
├─ TracerRenderer  traçantes des projectiles : bandes face à la caméra, un seul appel de dessin, mélange additif
├─ OverlayRenderer FluxUI : texte MSDF, formes SDF, images
├─ ModelCache      modèles glTF chargés une fois (GpuModel), lus et rangés (PrepareModel) sur deux fils à part
│                  (ModelLoader), envoyés au GPU un par image ; géométrie dans des pages partagées (GeometryPage,
│                  RangeAllocator), images et matériaux en tables (groupe 2)
├─ OcclusionCulling entités cachées écartées par le GPU (Culling.slang, pyramide de profondeur)
└─ RHI             Device, Buffer, Texture, Sampler, BindGroup, Pipeline, CommandList (interfaces virtuelles)
   └─ Vulkan       seul backend aujourd'hui (volk, vk-bootstrap, VMA)
```

Les envois vers la mémoire vidéo (buffers remplis, textures et leurs mipmaps) n'attendent pas le GPU : ils sont
enregistrés dans un command buffer à part (`Vk::Device::Upload`), soumis avec l'image suivante juste avant ses
commandes, et leurs buffers intermédiaires sont rendus quand le GPU a fini cette image. Chaque envoi commence par une
barrière sur tout ce qui a été soumis avant (une image en vol peut lire la table des matériaux qu'il réécrit). Sans
fenêtre (tests, calculs), ils sont soumis et attendus tout de suite, comme `RHI::Device::Submit`.

Les modèles sont rangés au chargement (`PrepareModel`, `Assets/Model.cpp`) pour se dessiner en peu d'appels quelle
que soit la façon dont ils ont été modélisés : matériaux identiques (tout sauf le nom) réunis, une primitive par
matériau, et des primitives d'ombre qui réunissent tous les opaques sans seuil d'alpha d'une même sorte de faces (les
ombres n'ont pas besoin du matériau). Un modèle fixe de plus de 32 m est découpé en blocs de 16 m (`ModelClusterSize`,
selon le centre de chaque triangle), chacun avec sa boîte : `MeshRenderer` les écarte un par un du champ et des
cascades (`ClusterDraw`), une station n'est dessinée que là où on la voit.

Ordre des passes dans une image (2 images en vol, `RHI::FramesInFlight`) :

1. Préparation sur le CPU : ciel et soleil (direction de la caméra vers le `SunComponent` s'il y en a un, sinon
   hauteur et direction réglées dans le Ciel), objets visibles (élimination hors champ), tuiles de planète choisies,
   transparents triés de loin à près. Un objet ni en vue ni dans une cascade d'ombre est écarté avant tout calcul (pose
   d'un personnage comprise). Les opaques et les ombres sont rangés en lots (même pipeline, même modèle, même
   primitive), et les lots du même pipeline et de la même page de géométrie partent ensemble dans un seul
   `DrawIndexedIndirect` (une commande par lot, le matériau de chaque objet lu par indice dans la table) ; les lots opaques passent dans l'ordre de leur objet
   le plus proche (ce qu'ils cachent échoue au test de profondeur avant d'être éclairé). Les transparents restent un
   appel par objet, dans leur ordre.
2. Calculs GPU : tuiles de planète manquantes (compute), ciel recuit s'il a changé, entités cachées écartées
   (`OcclusionCulling`, `Culling.slang`) : chaque entité des lots opaques et transparents (ou chaque bloc d'un grand
   modèle) est comparée, là où elle était à l'image précédente (`previousWorld`), à la pyramide de la profondeur de
   cette image ; ses commandes partent vides du CPU et le GPU n'y ajoute que les visibles. Pas de test au premier
   affichage ni après un saut de la caméra (`TemporalView::continuous`).
3. Ombres du soleil : 4 cascades dans une carte 4096² D32, profondeur seule sans programme de pixel (sauf seuil
   d'alpha) ; un objet fermé à une face n'y dessine que ses faces tournées vers le soleil, et un objet de moins de
   1,5 texel de rayon n'est pas dessiné dans la cascade.
4. Scène, dans une image HDR (RGBA16Float, lumière linéaire) à la taille de la vue 3D avec sa propre profondeur
   (`PostRenderer`) : opaques, fond du ciel puis soleil (`Sun.slang` : disque assombri au bord, halo, rayons en étoile
   ou trait horizontal selon `SunComponent::rays`, bien plus lumineux que le blanc), océans, nuages, transparents,
   flammes des réacteurs (additives, sans tri). PBR métal/rugosité, lumières ponctuelles et spots, volumes intérieurs, atmosphère et océans dans `Mesh.slang`.
   Les matériaux opaques sans seuil d'alpha passent par `fragmentOpaque`, qui ne jette aucun pixel : le GPU teste la
   profondeur avant le programme et n'éclaire pas ce qu'un objet plus proche cache.
   Avec l'anticrénelage, la projection est décalée d'une fraction de pixel différente à chaque image (8 points de
   Halton, `TemporalView`), sauf le fond du ciel (étoiles d'un pixel) ; les opaques écrivent aussi leur mouvement
   propre (seconde cible RG16Float) : où le point était à l'image précédente moins où il serait resté immobile, nul
   pour le décor et pour un cockpit qui suit la caméra.
   Après la passe, sa profondeur est réduite en pyramide (chaque niveau garde la plus lointaine de 2 x 2 texels) pour
   le test des entités cachées de l'image suivante.
5. Anticrénelage temporel (`Temporal.slang`, Paramètres > Graphiques, activé par défaut) : chaque pixel est retrouvé
   dans le résultat de l'image précédente (profondeur + caméra d'avant, plus le mouvement propre), ramené dans la boîte
   de couleurs de son voisinage 3 x 3 (YCoCg, moyenne ± écart type) puis mélangé à 10 % de l'image actuelle. Deux
   historiques en alternance ; rien de repris après un saut de caméra de plus de 100 km ou un changement de taille.
6. Bloom (`Post.slang`, méthode de Jimenez) : l'image anticrénelée réduite de moitié jusqu'à 6 fois (13 lectures,
   moyenne de Karis à la première), puis remontée en additionnant chaque niveau flou au niveau au-dessus ; sans seuil.
7. Passe principale vers la fenêtre : la scène mélangée à son bloom (Paramètres > Graphiques, 4 % par défaut), puis
   l'exposition et la courbe de tons ACES ; dans l'éditeur, la grille au sol (`Grid.slang`, plan y = 0 : chaque pixel
   suit son rayon jusqu'au plan et relit la profondeur de la scène pour se cacher derrière les objets) ; l'interface
   (overlay) par-dessus. Le fond d'une scène sans ciel est
   effacé avec la couleur qui redonne exactement la couleur choisie après la courbe (`InverseToneMap`).

Shaders : `Engine/shaders/*.slang`, compilés en SPIR-V par CMake (`slangc`). Les données par image et par objet passent
par des buffers de stockage (`FrameData`, `DrawData`) ; leurs structures C++ et Slang doivent rester identiques
(`static_assert` sur les tailles). Teinte, émissif, métal et rugosité propres à l'objet sont dans son `DrawData`, le
matériau dans les push constants (`DrawParams`) : un lot dessine des objets de réglages différents. L'instance `i`
d'un lot de modèles lit l'indice de son `DrawData` dans le buffer `instances` (groupe 1, emplacement 6) ; une tuile de
planète donne directement le sien comme indice d'instance.

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
- Chaque scène et chaque prefab notent la version du moteur qui les a écrits (`"engine"` = `GameModuleApiVersion`). Un
  fichier plus récent que le moteur, d'un format plus récent ou avec un champ inconnu (autre branche) est refusé
  avec « Scène non chargée car non compatible avec la version actuelle du moteur » et le détail, la scène en cours
  intacte ; dans le jeu, le message s'affiche dans une fenêtre. Un fichier plus ancien (ou sans ce numéro) est lu et
  converti (`.RemovedField`, `.ConvertedField`).
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
  Un vaisseau immobile, sans pilote ni chute (une station), ne teste pas sa collision : son enveloppe englobe le
  hangar, et chaque test contre le maillage d'un chasseur racine garé dedans coûtait 26 ms par image.
- Warp (`Warp`, après `Docking::Update`) : pour les grandes distances. Destinations : stations (vaisseaux à quais),
  planètes, objectif de la mission en cours ; J vise celle à moins de 30° du nez. Le vaisseau charge 3 s en se
  tournant vers elle, puis sa vitesse est multipliée par e chaque seconde, bornée par la distance restante en une seconde : il ralentit en arrivant et sort à 4 km d'une station, 10 km au-dessus de
  l'atmosphère d'une planète. En vol (`WarpStage::Cruising`), `MoveShips` le déplace sans collision et
  `CollectProbes` ne demande pas de sol pour lui. Il ne s'arrête qu'à destination : une planète sur la ligne droite est
  contournée à 20 km au-dessus de son atmosphère, et près d'elle une image ne le porte pas plus loin que le quart de
  la distance qui l'en sépare.
- IA de combat (`CombatAI`, `CombatAIComponent`) : machine à états en C++ (choix de Bruno, pas de langage de script)
  sur les vaisseaux ennemis à la racine : patrouille autour du point de départ, attaque dans `detectionRange`
  (vise en avance, ralentit à `preferredRange`), rupture de côté quand il passe trop près ou, prudent, quand il est
  touché, fuite sous `fleeHealth`. `Steer` (avant la physique) règle vitesse et rotation comme le pilote automatique
  (`Steering.hpp`) ; `Fire` (après la physique) appelle `Weapons::Update` avec la cible verrouillée, une erreur de
  visée (`WeaponInput::aimOffset`) et des missiles espacés (`WeaponInput::missiles`). Cible : le vaisseau du joueur.
- Missions : un PNJ porte `MissionGiverComponent` (« Donneur de missions ») ; F à sa portée ouvre sa fenêtre
  (`Engine::UpdateMissions`, `EngineMissions.cpp`). Une seule sorte : détruire un groupe d'ennemis, que
  `Missions::SpawnEnemies` fait apparaître à la racine, à `distance` devant la station (prefab du donneur, ou drones
  cubes armés), avec des points de vie et une IA de combat s'ils n'en ont pas. Le moteur garde une mission en cours (`m_mission`), finie quand
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

## Planètes (Planetology)

Tout ce qui concerne les planètes est réuni dans le domaine Planetology, espace de noms `Flux::Planetology` :
`include/Flux/Planetology/` (`Planet.hpp` : relief, tuiles ; `Atmosphere.hpp`) et `src/Planetology/` (choix des tuiles
`PlanetRenderer`, relief sur le GPU `PlanetTileCompute`, textures du sol `GroundTextures`, données de l'atmosphère pour
le GPU `AtmosphereGpu`, sol physique `PlanetGround`, arbres `VegetationRenderer`). Restent ailleurs : les composants
(`PlanetComponent`, `CloudsComponent`, `VegetationComponent` dans Scene, pour l'inspecteur et la sauvegarde), le dessin
du sol, de l'océan, des nuages et des arbres (`MeshRenderer` et `shaders/Mesh.slang`) et les shaders `PlanetTile.slang`,
`PlanetClimate.slang`, `Vegetation.slang` et `Atmosphere.slang`.

- Cube-sphère : 6 faces en quadtree, tuiles de 33×33 avec jupes, morphing CDLOD entre niveaux.
- Relief = une formule (`Terrain::GetHeight`), dupliquée dans `shaders/PlanetTile.slang` pour le GPU. Les deux
  versions doivent rester égales (`PlanetGpuTests`). Pas de terrain stocké ni creusable. Couches de bruit de Perlin
  avec leur pente exacte : continents, zones de collines, chaînes de montagnes (bandes où un bruit lent passe par
  zéro), crêtes, collines, détail. Les couches sont lues à un point déformé par la pente d'un bruit lent (côtes et
  chaînes tordues), et chaque octave est atténuée selon la pente réelle déjà sommée de sa couche (érosion analytique).
  Une octave ne dépend que des octaves plus grandes : une tuile grossière garde la forme de ses enfants.
- Sol choisi au pixel dans `Mesh.slang` : climat (température, humidité), pentes, plages, neige ; 5 textures en
  tableaux (`GroundTextures`) plaquées en triplanaire, chaque projection en pavage hexagonal (rotation et décalage
  aléatoires par cellule, périodique sur `GroundPeriod` pour ne pas voir de raccord entre tuiles) ; couleurs unies si les textures manquent ou pendant leur
  décodage (sur d'autres fils, seulement si la scène a une planète). Coût borné par pixel : 2 matières, 2 projections
  et 2 cellules au plus (la plus faible des trois est retirée des autres, sans saut), 2 lectures par point (couleur +
  rugosité en alpha ; normales en rouge-vert + occlusion en bleu).
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
  l'instance (`SV_VulkanInstanceID`) ; les modèles passent, eux, par le buffer `instances`.
- Arbres (`VegetationComponent`, `VegetationRenderer`), tout sur le GPU, le CPU ne voit aucun arbre. Les plaines sont
  les tuiles d'un niveau fixe (bord de 200 à 400 m) que `PlanetRenderer` visite en vue. Chacune est semée une fois
  (`computeTrees` dans `PlanetTile.slang`, avec le même `Height` que le sol au détail le plus fin) : 64×64 places, le
  hasard tiré de la place sur la face (mêmes arbres à chaque passage), gardées selon les forêts, la pente, l'altitude
  et le climat du sol (`PlanetClimate.slang`, partagé avec `fragmentPlanet` : les arbres poussent sur l'herbe). 256
  plaines restent semées en mémoire vidéo. À chaque image, `Vegetation.slang` range les arbres des plaines en vue en
  listes (détaillé, simplifié, imposteur selon la distance ; ombres avec le modèle simplifié, une liste par cascade
  touchée), puis écrit leurs nombres dans les commandes : un `DrawIndexedIndirect` par primitive de modèle et par liste.
  `vertexTree` lit la place et la rotation de chaque arbre et le balance au vent ; `fragmentTree` ne retourne pas les
  normales des feuilles (tournées vers l'extérieur de la couronne des deux côtés). Modèles : `assets/Modeles/Arbre`,
  générés par `/mnt/project-files/Modeles/Arbre/tree.py` (Blender). Les plaines vont jusqu'à la portée du composant
  (3 km par défaut) : sous une tuile dessinée plus grossière, `PlanetRenderer::AddPlains` ajoute celles qu'elle couvre.
  Au loin, les imposteurs s'espacent (une part inverse de la distance, un peu agrandis) et rapetissent près de la
  portée ; `fragmentPlanet` peint la cime des forêts sur le sol (mêmes taches et climat, `ForestAt`, calculés par
  sommet) à partir de la distance des imposteurs. L'air des arbres est calculé par sommet (`TreeVertexOutput`).

## Interface (FluxUI)

- Mode immédiat : `UI::Context` (panneaux ancrés, onglets, widgets, menus, arbres) produit une `UI::DrawList`
  dessinée par `OverlayRenderer`. Texte MSDF (police Arimo, atlas généré au démarrage), formes SDF.
- Polices du projet (`ScreenTextComponent::font`) : `Renderer::LoadFont` construit l'atlas sur un autre fil
  (`std::async`), puis lui donne sa texture et son groupe comme une image ; chaque texture garde ses constantes du
  shader (taille d'atlas, étendue de distance, mode). `DrawList::AddText(FontRef...)` : le texte passe dans une
  commande à part (un appel de dessin de plus par changement de police).
- Interfaces du jeu : fichiers `.fluxui` (`Flux::Interface`, mode retenu) éditées dans l'éditeur, chargées par
  `InterfaceLibrary`, liées aux valeurs du jeu (`BindInterfaceValue`) et aux fonctions C++ du module.

## Éditeur

- `main.cpp` crée le moteur sans panneau Moteur ni interface plein écran, puis branche sa propre mise à jour.
- En haut : barre de menus puis barre d'outils (`BeginToolBar`, boutons à pictogramme et info-bulles, groupe Texte de
  `TextTools` avec ses listes `OpenPopupMenu` et `MenuBarColorButton`) ; en bas, la barre d'état. `GetMenuBarHeight` compte les deux barres du haut.
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
