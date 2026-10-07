# Flux : moteur, éditeur et jeu Ars Solaris

Moteur de jeu 3D spatial (FluxEngine), son éditeur (FluxEditor) et le jeu **Ars Solaris** qui les utilise. Gameplay
visé : piloter un vaisseau depuis le cockpit, se poser sur des planètes sans chargement, marcher à bord et en sortir.
Ce qui est fait et ce qui vient : `Engine/ROADMAP.md` et `Editor/ROADMAP.md` (à tenir à jour à chaque PR).

## Langue
- Tout en français : commentaires, messages de commit, descriptions de PR, textes de l'interface, journaux.
- Identifiants du code en anglais (`ShipComponent`, `GetWorldPosition`).
- Le propriétaire du projet, Bruno, débute avec Git : vocabulaire minimal, étapes de Visual Studio en français.

## Organisation du dépôt
- `Engine/` : FluxEngine.dll. En-têtes publics dans `include/Flux/<Domaine>/`, code dans `src/<Domaine>/`, shaders Slang
  dans `shaders/`. Domaines : Animation, Assets, Audio, Core, Graphics, Input, Interface, Math, Physics, Platform,
  Renderer (RHI + backend Vulkan), Scene, UI (FluxUI).
- `Editor/` : FluxEditor.exe (panneaux dans `src/Panels`, documents dans `src/Document`, éditeur d'interfaces dans
  `src/Interface`). Recharge à chaud la DLL du jeu.
- `Game/Module/` : GameModule.dll (code du jeu, `FluxRegisterGame`) et le projet `Game.fluxproject` avec ses `assets/`.
- `Game/Client/` : Game.exe.
- `Tests/` : FluxTests.exe (un fichier `*Tests.cpp` par domaine, fonctions `Run…Tests` appelées depuis `main.cpp`).
- `cmake/` : sources trouvées automatiquement (`flux_add_sources`, pas de liste de fichiers à tenir), dépendances
  (FetchContent), shaders, en-têtes précompilés, avertissements, version.

## Compiler
- Windows seulement. Visual Studio + MSVC avec les préréglages `msvc-debug` et `msvc-release` (Ninja). Le SDK Vulkan
  fournit `slangc`.
- Après une mise à jour : Tirer puis Générer suffit. Ne jamais conseiller « Tout régénérer » ni « Supprimer le cache et
  reconfigurer » (tout recompiler, Jolt compris). Si une mise à jour casse la compilation incrémentale, chercher la
  dépendance CMake manquante.
- La CI GitHub (`.github/workflows/msvc.yml`) compile Debug + Release avec MSVC et lance FluxTests sur chaque PR.
- Juger les performances en Release seulement, jamais en Debug.

## Choix d'architecture (décidés, ne pas remettre en cause sans le demander)
- Vulkan derrière une couche RHI (`Renderer/RHI`) ; C++23 ; volk, vk-bootstrap, VMA ; shaders Slang.
- Mondes géants : secteurs `int64` (1e9 m) + positions `double`, rendu relatif à la caméra, reverse-Z infini.
- Scène EnTT. Chaque composant s'enregistre une fois dans `ComponentRegistry.cpp` : inspecteur et sauvegarde JSON en
  sont générés. Un champ retiré passe par `.RemovedField`.
- Interface maison FluxUI en mode immédiat (`if (ui.Button(...))`), partagée éditeur/jeu. Jamais d'ImGui.
- Physique Jolt, privée au moteur ; un monde physique par vaisseau.
- Ce qui concerne l'affichage est calculé par le GPU, pas par le CPU (tuiles de planète en compute shader). La
  collision des planètes reste sur le CPU en double.

## Règles à respecter
- Changer la disposition d'un composant ou une fonction de l'API du jeu : incrémenter `GameModuleApiVersion`
  (`Engine/include/Flux/Core/GameModule.hpp`), une seule fois par PR.
- Relief des planètes en deux exemplaires : `Terrain::GetHeight` (C++) et `Height` dans `shaders/PlanetTile.slang`.
  Toute modification va dans les deux ; `PlanetGpuTests` vérifie qu'ils restent égaux.
- Bruit du ciel en deux exemplaires : `Graphics/Sky.cpp` et `shaders/Sky.slang`, à modifier ensemble.
- Performances : pas d'allocation ni de copie par image dans le code chaud ; dire dans la PR ce qui coûte (appels de
  dessin un par un, parcours quadratiques) ; mesurer (panneau Moteur, timestamps GPU) avant d'optimiser.
- Commentaires : dire pourquoi, pas répéter le code ; garder la densité du code voisin.
- Ne jamais désactiver ni sauter un test pour obtenir une CI verte.

## Façon de travailler avec Bruno
- Une PR par étape, depuis une branche `claude/<sujet>` partant de master à jour.
- Description de PR : un paragraphe « Avant : », un paragraphe « Après : », puis « Comment : ».
- Bruno teste dans Visual Studio (Git > Récupérer, choisir la branche, Git > Tirer, Générer). Quand il écrit
  « fusionne », Claude fusionne la PR sur GitHub (méthode merge). Ne pas lui demander de fusionner ni de pousser.
- Il veut un avis franc sur la qualité et les performances du code, et une question quand plusieurs choix existent.
- Avant de toucher à un fichier de scène ou d'asset de Bruno, regarder ce qu'il contient : ce sont ses réglages.
