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
- ✅ Élimination hors champ, opaques triés, mesure CPU et GPU (panneau Moteur ; scène détaillée : objets, sol, ciel, océan, nuages, transparents) et de la mémoire (RAM du processus, mémoire vidéo).
- ✅ Diagnostic de plantage GPU : étape bloquée et panne du pilote dans plantage-gpu.txt (repères AMD, VK_EXT_device_fault).
- ✅ Calcul sur GPU (compute shaders dans le RHI).
- ✅ Flammes des réacteurs (composant « Réacteur ») : volume de gaz calculé au pixel, longueur et éclat selon la
  poussée du vaisseau, lumière autour ; à pleine poussée dans l'éditeur (jeu arrêté) pour les régler.

### Planètes
- ✅ Cube-sphère en quadtree, LOD sans popping (CDLOD), tuiles calculées par le GPU.
- ✅ Atterrissage depuis l'espace sans chargement ; collision du sol sur le CPU en double, autour des objets.
- ✅ Atmosphère (ciel vu du sol, brume, halo), océans avec vagues.
- 🔨 Soleil posé dans l'espace (composant « Soleil ») : lumière, ombres et disque viennent de sa position ; jour d'un côté de la planète, nuit de l'autre. Pas de cycle jour / nuit (ni la planète ni le soleil ne bougent). Disque et lumière de couleurs séparées ; côté nuit, la lumière du ciel de l'espace baisse (planète à atmosphère). Soleil fixe au centre de la scène, composant « Orbite » (distance, angle, inclinaison) pour ranger les planètes autour, comme dans EVE Online. Lumière de contour (« Lumière de contour ») aussi sur les planètes, liseré selon la sphère.
- 🔨 Ciel bleu de jour : étoiles et nébuleuse effacées par la lumière de l'air.
- ✅ Biomes par climat et sol texturé (5 matières, plaquage triplanaire).
- ✅ Teinte de chaque texture du sol (herbe verdie par défaut : son image se confondait de loin avec la roche).
- ✅ Plage réglée en mètres au-dessus de la mer (elle suit le niveau de la mer).
- ✅ Sol sans répétition visible : pavage hexagonal (rotation et décalage aléatoires par cellule, mélange des 3 cellules voisines).
- 🔨 Sol vu de haut : texture fondue vers sa couleur moyenne entre 600 m et 2 km (plus de motif régulier de points sombres, moins de lectures au loin).

### Monde, scène, physique
- ✅ Scène EnTT, secteurs int64 + positions double (échelle galaxie), sauvegarde JSON (`flux.scene`).
- ✅ Composants enregistrés une fois : inspecteur et sauvegarde générés.
- ✅ Physique Jolt : objets simulés, personnage (capsule), un monde physique par vaisseau (on marche à bord pendant qu'il vole).
- ✅ Vaisseaux : pilotage 6 axes avec inertie, vol atmosphérique, assiette qui suit la courbure de la planète (aide à plat), mode voyage (ralenti à l'approche d'une planète, vitesse rendue à sa sortie), sièges, atterrissage stabilisé.
- ✅ Animation squelettique (fondus, repos/marche/course/chute/s'asseoir).
- ✅ Son 3D (miniaudio), actions d'entrée réassignables (Touches.json).
- 🔨 Composant « Monde » (comme les World Settings d'Unreal) : gravité et air hors planètes, freinage par l'air des objets et des personnages (aussi dans l'atmosphère des planètes).

### Interface (FluxUI)
- ✅ Mode immédiat partagé éditeur/jeu, texte MSDF, formes SDF, thèmes.
- ✅ Interfaces du jeu en fichiers `.fluxui` : fenêtres, boutons à actions, liaisons de valeurs, onglets, défilement.
- ✅ Textes et images à l'écran, changement de scène.

### Outillage
- ✅ Module du jeu en DLL rechargeable à chaud, API versionnée (`GameModuleApiVersion`).
- ✅ Tests (FluxTests), CI MSVC Debug + Release.
- 🔨 Debug utilisable : Jolt optimisé en Debug, préréglage « Release + débogage », petits mondes physiques sans fils.
- ✅ Version affichée dans le jeu : numéro + commit Git.

## Prochaines étapes

### Performances au sol (constaté : 150 images/s en Release face à l'océan)
- ✅ Démarrage : textures du sol décodées en parallèle sans bloquer le jeu, police et images optimisées en Debug.
- 🔨 Air, climat et reflet du ciel sur l'océan calculés par sommet pour le sol et l'eau, plus au pixel.
- ✅ Tuiles des planètes dessinées par appels indirects : un appel par page de 256 tuiles et par passe (sol, eau,
  chaque cascade d'ombres) au lieu d'un appel par tuile.
- ⬜ Perspective aérienne précalculée (texture 3D) pour l'air devant les modèles, si l'air par sommet ne suffit pas.
- ⬜ Océan : tuiles plus grossières et moins nombreuses (253 tuiles redessinées aujourd'hui).
- ⬜ Réglage du niveau de détail des tuiles (`LodSplitFactor`).

### Rendu
- ✅ HDR + bloom (lueurs, soleil, réacteurs) ; soleil dans son propre shader (disque assombri au bord, halo, rayons en étoile ou long trait horizontal façon EVE Online, au choix du composant Soleil). Force réglable dans Paramètres > Graphiques de l'éditeur ; le jeu garde la force par défaut (réglage du jeu à venir).
- ⬜ Particules sur le GPU : armes, explosions, étincelles, fumée (et poussière sous les réacteurs au sol).
- ⬜ Anticrénelage (MSAA ou TAA).
- ⬜ Ombres des lumières intérieures.
- ⬜ Instancing (champs d'astéroïdes, végétation).
- ⬜ Rendu vers texture : écrans du cockpit, interfaces dans le monde 3D.

### Planètes
- ⬜ Végétation et rochers posés sur le sol (instancing).
- ⬜ Mélange des matières par la hauteur des textures (displacement) plutôt qu'un fondu.
- ✅ Nuages : une couche épaisse par planète (`CloudsComponent`), en volume près de la caméra (8 pas), en colonne au loin et vu de l'espace, avec son ombre.
- ⬜ Nuages : plus de pas quand on vole dedans, reprojection d'une image à l'autre, formes par type (cumulus, stratus).
- ⬜ Remplacer la texture de sable (aerial_beach_01), trop marron.
- ⬜ 2 ou 3 textures par matière, pour varier davantage le sol de près.

### Gameplay
- ✅ Prefabs : exemplaires qui suivent leur prefab, différences gardées champ par champ.
- ⬜ Prefab dans un prefab (station avec ses vaisseaux).
- ✅ Réglages enregistrés pour Game.exe (`GameStartup`, `Reglages.json` : fenêtre, plein écran sans bordure (Alt+Entrée, F11), V-Sync, volume, panneau Moteur).
- ⬜ Menu Échap du jeu ; plein écran et choix du GPU dans les réglages.
- ⬜ Événements d'animation (bruits de pas).
- ⬜ Collision vaisseau contre vaisseau ; vol sous un surplomb (enveloppe convexe aujourd'hui).
- ⬜ Interdire de se lever en vol hors gravité (chute sans fin).
- ⬜ GameMode (règles de partie : personnage du joueur, point d'apparition, interface, victoire), quand il y aura des règles à y mettre.

## Plus tard
- ⬜ Réseau, géré par le moteur : domaine `Network` du moteur. Transport UDP avec messages fiables et non fiables (bibliothèque à choisir :
  GameNetworkingSockets ou ENet), serveur qui fait autorité, composants à répliquer marqués dans `ComponentRegistry`
  (comme l'inspecteur et la sauvegarde), positions en secteurs + double, prédiction du joueur et interpolation des
  autres. Les mondes physiques par vaisseau tournent sur le serveur.
- ⬜ Mises à jour, gérées par le moteur : manifeste de version (liste des fichiers et de leurs empreintes) publié en HTTPS, seuls les fichiers
  changés téléchargés puis vérifiés, retour à la version précédente en cas d'échec. Le Launcher s'en sert ; Game.exe
  vérifie au démarrage qu'il est à jour.
- ⬜ Launcher (paramètres, bouton Jouer), par-dessus les mises à jour du moteur.
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
