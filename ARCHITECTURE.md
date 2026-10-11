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
- **Game.exe** n'a qu'un `main` d'une ligne : `Flux::RunGame` (`Core/GameClient.hpp`) lit le projet, charge
  GameModule.dll, ouvre la scène de démarrage et lance `Engine::Run`. Comme dans Unreal, le moteur possède la boucle et
  appelle le jeu (même boucle que le mode Jouer de l'éditeur, condition du rechargement à chaud).
- **Game.fluxproject** décrit le projet : nom, dossier `assets/`, scène de démarrage. Les assets sont copiés à côté
  des exécutables (`bin/assets/`).

## Une image (`Engine::Run`)

1. Fenêtre : événements Win32, entrées clavier et souris.
2. FluxUI commence l'image (`UI::Context::BeginFrame`) ; la caméra suit le joueur ou vole librement (éditeur).
3. Si le jeu tourne (`UpdateGameSystems`) :
   - changement de scène demandé, actions d'entrée lues (`InputActions`) ;
   - sièges et pilotage (`Seats::Update`) ;
   - physique (`PhysicsWorld::Update`, pas fixe de 1/60 s) : personnages, objets simulés, vaisseaux, sol des planètes ;
   - animation (`Animator::Update`), après la physique ;
   - puis la mise à jour du module du jeu.
4. Caméra suiveuse placée, son 3D (`AudioSystem::Update`), mise à jour de l'éditeur s'il y en a un.
5. Interface à l'écran : textes, images, interfaces `.fluxui`, affichage de pilotage, version, panneau Moteur.
6. Rendu (`Renderer::RenderFrame`), puis actions des interfaces et changement de scène en fin d'image.

## Rendu

Couches, de la plus haute à la plus basse :

```
Renderer           ordre des passes, mesures (CPU, attentes, timestamps GPU), étapes du diagnostic de plantage GPU
├─ MeshRenderer    modèles, tuiles de planète, ombres, lumières, données par image
├─ PlanetRenderer  choix des tuiles du quadtree ; PlanetTileCompute : relief calculé par le GPU
├─ SkyRenderer     ciel (nébuleuse cuite une fois dans une texture cube), éclairage ambiant
├─ OverlayRenderer FluxUI : texte MSDF, formes SDF, images
├─ ModelCache      modèles glTF chargés une fois (GpuModel), matériaux
└─ RHI             Device, Buffer, Texture, Sampler, BindGroup, Pipeline, CommandList (interfaces virtuelles)
   └─ Vulkan       seul backend aujourd'hui (volk, vk-bootstrap, VMA)
```

Ordre des passes dans une image (2 images en vol, `RHI::FramesInFlight`) :

1. Préparation sur le CPU : ciel et soleil, objets visibles (élimination hors champ), tuiles de planète choisies, tri
   des opaques de près à loin et des transparents de loin à près.
2. Calculs GPU : tuiles de planète manquantes (compute), ciel recuit s'il a changé.
3. Ombres du soleil : 4 cascades dans une carte 4096² D32.
4. Scène : opaques, ciel, transparents. PBR métal/rugosité, lumières ponctuelles et spots, volumes intérieurs,
   atmosphère et océans dans `Mesh.slang`. Tonemapping ACES en sortie du fragment (pas de HDR intermédiaire).
5. Interface (overlay) par-dessus.

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

## Physique (Jolt)

- `PhysicsContext` (créé avec le moteur) : système Jolt, fils de travail, maillages de collision en cache. Un monde
  de moins de 128 corps actifs est calculé sur le fil principal (réveiller les fils coûtait plus que le calcul).
- `PhysicsWorld` (par scène lancée) : un monde Jolt pour la scène, plus un par vaisseau dans son propre repère. On
  marche dans un vaisseau qui vole parce que son intérieur est immobile dans son monde. Des copies cinématiques de
  son décor dans le monde extérieur permettent d'y monter.
- Vaisseaux à la racine : déplacés par `MoveShips` (lancers de leur enveloppe convexe contre le décor), avec
  stabilisateur d'atterrissage ; pas de corps dynamique.
- Personnages : `CharacterVirtual` (capsule), gravité de la planète ou du vaisseau.
- Sol des planètes (`PlanetGround`) : tuiles de maillage au niveau de détail le plus fin, calculées sur le CPU en
  `double` autour des objets (sondes), sur un fil à part.

## Planètes

- Cube-sphère : 6 faces en quadtree, tuiles de 33×33 avec jupes, morphing CDLOD entre niveaux.
- Relief = une formule (`Terrain::GetHeight`), dupliquée dans `shaders/PlanetTile.slang` pour le GPU. Les deux
  versions doivent rester égales (`PlanetGpuTests`). Pas de terrain stocké ni creusable.
- Sol choisi au pixel dans `Mesh.slang` : climat (température, humidité), pentes, plages, neige ; 5 textures en
  tableaux (`GroundTextures`) plaquées en triplanaire ; couleurs unies si les textures manquent ou pendant leur
  décodage (sur d'autres fils, seulement si la scène a une planète).
- Atmosphère (diffusion calculée par sommet pour le sol et l'océan, au pixel pour les modèles et le ciel) et océan (tuiles sous le niveau de la mer redessinées en surface d'eau).
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
- `SceneEditor` regroupe les panneaux : Scènes (hiérarchie), Inspecteur, Placer, Contenu, Caméra, Console, Paramètres.
- `SceneViewport` : vue 3D, sélection (`Picking`), gizmo, glisser-déposer.
- `SceneDocument` : fichier ouvert, annuler / refaire (état avant/après par objet), titre de la fenêtre.
- `Interface/` : éditeur des `.fluxui`.

## Fichiers

| Fichier | Rôle |
|---|---|
| `*.scene.json` | scène (`flux.scene` v1) |
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
