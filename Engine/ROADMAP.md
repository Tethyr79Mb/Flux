# Feuille de route du moteur (FluxEngine)

Moteur 3D spatial du jeu **Ars Solaris** : Vulkan, C++23, Windows. Ce fichier dit où on en est et ce qui vient
ensuite. L'ordre des sections « Prochaines étapes » est une proposition : Bruno décide.

Légende : ✅ fait · 🔨 en cours (PR ouverte) · ⬜ à faire

## Où on en est

### Rendu
- ✅ Vulkan (volk, vk-bootstrap, VMA) derrière une couche RHI (`Renderer/RHI`) : un autre backend (DX12) reste possible.
- ✅ Shaders Slang compilés par CMake.
- ✅ Caméra en reverse-Z, plan lointain infini, rendu relatif à la caméra (mondes géants).
- ✅ Modèles glTF : matériaux PBR métal/rugosité, normales, occlusion, émissif, transparence triée.
- ✅ Ombres du soleil en 4 cascades ; lumières ponctuelles et spots (256 max, sans ombre).
- ✅ Ciel spatial généré ou panorama, reflets et ambiante tirés du ciel ; volumes « Intérieur » ; lumière de contour.
- ✅ Élimination hors champ, opaques triés, mesure CPU et GPU (panneau Moteur).
- ✅ Calcul sur GPU (compute shaders dans le RHI).

### Planètes
- ✅ Cube-sphère en quadtree, LOD sans popping (CDLOD), tuiles calculées par le GPU.
- ✅ Atterrissage depuis l'espace sans chargement ; collision du sol sur le CPU en double, autour des objets.
- ✅ Atmosphère (ciel vu du sol, brume, halo), océans avec vagues.
- ✅ Biomes par climat et sol texturé (5 matières, plaquage triplanaire).
- 🔨 Teinte de chaque texture du sol (PR #92).

### Monde, scène, physique
- ✅ Scène EnTT, secteurs int64 + positions double (échelle galaxie), sauvegarde JSON (`flux.scene`).
- ✅ Composants enregistrés une fois : inspecteur et sauvegarde générés.
- ✅ Physique Jolt : objets simulés, personnage (capsule), un monde physique par vaisseau (on marche à bord pendant qu'il vole).
- ✅ Vaisseaux : pilotage 6 axes avec inertie, vol atmosphérique, mode voyage, sièges, atterrissage stabilisé.
- ✅ Animation squelettique (fondus, repos/marche/course/chute/s'asseoir).
- ✅ Son 3D (miniaudio), actions d'entrée réassignables (Touches.json).

### Interface (FluxUI)
- ✅ Mode immédiat partagé éditeur/jeu, texte MSDF, formes SDF, thèmes.
- ✅ Interfaces du jeu en fichiers `.fluxui` : fenêtres, boutons à actions, liaisons de valeurs, onglets, défilement.
- ✅ Textes et images à l'écran, changement de scène.

### Outillage
- ✅ Module du jeu en DLL rechargeable à chaud, API versionnée (`GameModuleApiVersion`).
- ✅ Tests (FluxTests), CI MSVC Debug + Release.
- ✅ Version affichée dans le jeu : numéro + commit Git.

## Prochaines étapes

### Performances au sol (constaté : 150 images/s en Release face à l'océan)
- ⬜ Perspective aérienne précalculée (texture 3D) au lieu de l'air calculé à chaque pixel.
- ⬜ Océan : tuiles plus grossières et moins nombreuses (253 tuiles redessinées aujourd'hui).
- ⬜ Réglage du niveau de détail des tuiles (`LodSplitFactor`).

### Rendu
- ⬜ HDR + bloom (lueurs, soleil, réacteurs).
- ⬜ Anticrénelage (MSAA ou TAA).
- ⬜ Ombres des lumières intérieures.
- ⬜ Instancing (champs d'astéroïdes, végétation).
- ⬜ Rendu vers texture : écrans du cockpit, interfaces dans le monde 3D.

### Planètes
- ⬜ Végétation et rochers posés sur le sol (instancing).
- ⬜ Mélange des matières par la hauteur des textures (displacement) plutôt qu'un fondu.
- ⬜ Nuages.

### Gameplay
- ⬜ Prefabs (vaisseau, station réutilisables).
- ⬜ Menu Échap du jeu, réglages enregistrés pour Game.exe.
- ⬜ Événements d'animation (bruits de pas).
- ⬜ Collision vaisseau contre vaisseau ; vol sous un surplomb (enveloppe convexe aujourd'hui).
- ⬜ Interdire de se lever en vol hors gravité (chute sans fin).

## Plus tard
- ⬜ Launcher (mises à jour, paramètres, bouton Jouer).
- ⬜ Render graph.
- ⬜ Crédits et `LICENCES-TIERCES.txt` générés à côté du jeu (avant toute distribution).
- ⬜ Backend DX12.

## Dettes et limites connues
- Les lumières sont parcourues pixel par pixel, sans tuiles (lent au-delà de quelques dizaines).
- Les reflets viennent seulement du ciel, pas de la scène.
- L'atlas de police est régénéré au démarrage et limité au Latin-1.
- Le mode Jouer copie la scène en JSON (lent sur une grosse scène).
- Pas de DPI : l'interface suppose une mise à l'échelle Windows de 100 %.
- Les textures du sol pèsent 45 Mo dans le dépôt (2K, 5 matières).
