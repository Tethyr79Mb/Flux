# Feuille de route du moteur (FluxEngine)

Moteur 3D spatial du jeu **Ars Solaris** : Vulkan, C++23, Windows. Ce fichier dit où on en est et ce qui vient
ensuite. L'ordre des sections « Prochaines étapes » est une proposition : Bruno décide.

Légende : ✅ atteint · 🔧 à améliorer (fait, avec des limites connues, dites sur la ligne) · 🔨 commencé (en cours ou PR ouverte) · ⬜ à faire

## Où on en est

### Performances du rendu (demandé par Bruno le 2026-10-17)
- ✅ Ombres sans programme de pixel ni faces arrière, petits objets hors des cascades lointaines ; opaques sans
  `discard` (test de profondeur avant éclairage) ; objets ni en vue ni dans une ombre écartés (pose comprise) ; chaque
  carte ne reçoit que les objets, tuiles et arbres dont l'ombre peut tomber dans sa tranche du champ (derrière la
  caméra ou ombre hors champ : écartés ; 416 ombres d'arbres -> 158 sur une vue d'essai) ; heures GPU seulement panneau
  Moteur ouvert (fermé : le compteur d'images/s seul).
- 🔧 Cascades d'ombres lointaines (60 et 200 m) redessinées une image sur deux, à tour de rôle, la carte gardée recalée
  sur la caméra, et seulement si elle couvre encore la même tranche (2026-11-10 : tourner la caméra ne la gardait pas
  moins, des ombres apparaissaient et disparaissaient selon le regard) ; un point hors de sa carte lit la suivante.
  Limites : l'ombre lointaine d'un objet qui bouge a une image de retard ; en tournant, les deux sont refaites.
- 🔧 Objets opaques et ombres rangés en lots (pipeline, modèle, primitive) : les objets identiques en un seul appel
  indirect (instancing), teinte et réglages de chaque objet gardés. Les transparents restent un appel par objet.
- ⬜ Lumières par tuiles d'écran (aujourd'hui chaque pixel parcourt toutes les lumières visibles).
- 🔧 Enregistrement des commandes sur plusieurs fils (demande de Bruno, 2026-11-04), en 3 étapes :
  - ✅ 1. `JobSystem` : fils de travail (cœurs moins un), vol de tâches, attente qui aide, aucune allocation par tâche.
  - ✅ 2. Plusieurs listes primaires par image (choix de Bruno) : `CommandList::Fork` insère des listes à remplir sur d'autres fils, jouées dans l'ordre en une seule soumission ; un pool par command buffer, créé une fois. Aussi dans `Device::Submit`.
  - 🔧 3. Graphe de rendu enregistré en parallèle : préparation et barrières des textures prévues d'abord sur un fil, puis passes réparties en groupes de temps CPU égal (moyenne mesurée par passe), un groupe par liste de Fork, 8 listes au plus. Case « Enregistrement parallèle » et temps CPU par passe au panneau Moteur. Une passe ne se coupe qu'en étapes (3b) : la plus lourde (objets ou sol) fixe le temps.
  - ✅ 3b. Passes lourdes en command buffers secondaires : `CommandList::BeginParallelRenderPass` et `RenderGraph::PassBuilder::BeginParallel`, une liste secondaire par étape remplie par un fil. Scène (objets, arbres, sol, ciel, océan) et ombres (une étape par cascade).
  - 🔧 4. Préparation du rendu en parallèle : chaque entité dessinée (sphère, champ, matrices, contour) calculée par les fils en tranches de 64, poses et listes gardées sur un fil dans l'ordre ; traçantes préparées à côté. Temps par partie au panneau Moteur (« Préparation (CPU) ») et dans les mesures. Reste sur un fil : le parcours des tuiles du sol, les plaines d'arbres, les envois.
  - Plus tard : Jolt sur le `JobSystem` (il a aujourd'hui ses propres fils, cœurs moins un aussi).

### Chargement et rendu communs à tous les modèles (demandé par Bruno le 2026-10-17)
Station, vaisseau ou décor : le moteur s'adapte au modèle, sans fusion des pièces à la main.
- ✅ Préparation au chargement : matériaux identiques réunis, une primitive par matériau, ombres en un appel par
  modèle, grands modèles découpés en blocs de 16 m écartés un par un (champ, cascades).
- 🔨 Gros modèles par paquets de triangles (Bruno, 2026-10-23 : vaisseaux S ~150 000 triangles, M ~400 000, plus gros
  ensuite, tourelles comprises), en 4 étapes. Fait (étape 1) : chaque primitive d'un modèle fixe découpée au chargement
  en paquets de 64 sommets / 124 triangles (meshoptimizer), avec sphère et cône des normales, sur le fil de chargement
  (Chasseur : 50 ms en Release pour 68 000 triangles, contre 600 ms pour lire ses images ; pas de cache disque tant que
  ça reste petit, à revoir avec les paquets .pak). Fait (étape 2, mesh shaders) : sur une carte qui les a, les
  primitives opaques des modèles fixes sont dessinées paquet par paquet, le GPU écartant ceux hors du champ, tournés
  vers l'arrière ou cachés (pyramide de profondeur) ; compteur « Paquets des modèles » au panneau Moteur et dans le
  relevé. À faire : 2b) même tri en compute + sommets pour une carte sans mesh shaders (aujourd'hui modèle par modèle) ;
  2c) scène persistante sur le GPU (n'envoyer que ce qui bouge ; aujourd'hui les tâches sont refaites par le CPU à chaque
  image, une pour 32 paquets) ; 3) ombres par le même tri ; 4) détail continu façon Nanite (groupes de paquets
  simplifiés). Modèles animés et transparents pas découpés. Gain à mesurer en Release chez Bruno.
- 🔨 Rendu piloté par le GPU, commencé le 2026-10-17. Fait : géométrie de tous les modèles dans des pages partagées
  (modèles animés à part), images et matériaux dans deux tables lues par indice, lots du même pipeline et de la même
  page dessinés en un seul `DrawIndexedIndirect` (plusieurs commandes) ; entités et blocs cachés derrière d'autres
  écartés par un compute shader (pyramide de la profondeur de l'image précédente, case « Objets cachés écartés » et
  compteur au panneau Moteur). L'élimination hors champ reste sur le CPU (par entité et par bloc, peu coûteuse ; elle
  évite aussi de calculer la pose des personnages hors champ). Reste : ce qu'un mur découvre apparaît une image plus
  tard (seconde passe à faire), les ombres ne profitent pas du test.
- 🔧 Chargement en arrière-plan : modèles lus et décodés sur deux fils, qui rangent aussi leurs images dans une
  mémoire d'envoi ; copies par une file de transfert dédiée quand la carte en a une (repli : la file graphique), depuis
  un anneau d'envoi de 64 Mo pour les petits envois ; mipmaps calculés par la file graphique. Les gros envois (modèles,
  textures du sol) sont asynchrones : aucune image ne les attend, le modèle apparaît quand ses copies sont finies. Un
  modèle ou un sol remplacé est détruit quand le GPU a fini de s'en servir (`Retire`), sans attendre le GPU. Limites :
  les textures du sol (45 Mo) sont encore recopiées dans la mémoire d'envoi par le fil principal, la file de transfert
  traite les lots dans l'ordre (un petit envoi attendu peut passer derrière un gros lot asynchrone de 32 Mo au plus),
  images à l'écran et ciel en panorama lus sur le fil principal, l'écran des images attend encore le GPU quand une
  image change.
- ⬜ Niveaux de détail par bloc générés au chargement, chargement selon la distance.
- ⬜ Physique : mondes des vaisseaux en parallèle (0,18 ms mesurées avec 30 ennemis : pas utile pour l'instant).
- ⬜ Calcul asynchrone (accord de Bruno du 2026-10-26) : une seconde file du GPU pour les calculs (tuiles du sol,
  semis des arbres, bruit et historique des nuages) qui tournent pendant le dessin, dans les trous qu'il laisse (gain
  habituel de 5 à 15 %, surtout sur AMD). Le render graph décide quelles passes y partent et place les sémaphores et
  les transferts de propriété. À mesurer avant (temps du GPU par passe) ; facultatif, le chemin à une file reste.

### Rendu
- ⬜ Limiteur d'images par seconde réglable par le joueur et réduction de la latence (image préparée au dernier moment
  avant l'affichage, VK_KHR_present_wait), après les ombres.
- 🔨 Vulkan (volk, vk-bootstrap, VMA) derrière une couche RHI (`include/Flux/Renderer/RHI`) : un autre backend (DX12) reste
  possible. Manquent : une liste de commandes par fil (une seule aujourd'hui, sur le fil principal), plusieurs files
  exposées au renderer (la file de transfert reste cachée dans le backend, pas de file de calcul), un cache de
  pipelines (`VkPipelineCache` enregistré sur disque), d'autres copies que `CopyBuffer` (buffer vers texture, texture
  vers texture), plus de deux cibles de couleur par passe (G-buffer du rendu différé), le ray tracing (facultatif).
  Fichiers rangés : un en-tête par interface, types communs dans `RHITypes.hpp`, un fichier par bloc Vulkan
  (instance, surface, choix du GPU, device, swapchain, images en vol, heures du GPU, diagnostic, envois).
- ✅ Shaders Slang compilés par CMake.
- ✅ Caméra en reverse-Z, plan lointain infini, rendu relatif à la caméra (mondes géants).
- 🔧 Modèles glTF : matériaux PBR métal/rugosité, normales, occlusion, émissif, transparence triée ; fichiers compressés par gltfpack (EXT_meshopt_compression : la station passe de 34 à 5,1 Mo). À améliorer : transparents triés par objet (deux objets qui s'entrecroisent peuvent mal se superposer), `.gltf` non accepté, pas de morph targets, coordonnées de texture quantifiées (KHR_texture_transform) ignorées : compresser avec `-vtf`.
- 🔧 Niveau de détail des modèles : une version lointaine par modèle (« Version lointaine » du Modèle 3D), affichée au-delà d'une distance à sa sphère englobante. À améliorer : un seul palier, bascule sans fondu ; les deux versions restent chargées.
- 🔧 Ombres du soleil en 4 cascades, pénombre réaliste (PCSS, 2026-11-10 : nette au contact, élargie selon la distance de
  l'obstacle et la taille du soleil ; lectures tournées à chaque pixel, lissées par l'anticrénelage temporel ; 16 lectures
  sur la configuration recommandée, 8 sur le minimum ; plus d'escaliers sous un soleil rasant) ; lumières ponctuelles et spots (256 max, sans ombre). À améliorer : les lumières traversent les murs (pas d'ombre).
- 🔧 Ombres du relief au loin : 2 cascades de plus (jusqu'à 3 km, mêlée aux cascades proches dans les 200 premiers mètres, puis 3 à 40 km) dans leur propre carte, où seul le sol des planètes est dessiné (4096 texels chacune sur la configuration recommandée, 2048 sur le minimum) ; gardées tant que la caméra et le soleil ont peu bougé, refaites à tour de rôle toutes les 30 images (60 au minimum), jamais en tournant la caméra (obstacles retenus tout autour d'elle) ; tuiles dessinées avec un sommet sur 2 à 16 dans ces cartes ; tuiles hors du champ dessinées dans les cartes quand leur ombre y tombe (montagne derrière la caméra, soleil dans le dos), cascades proches comprises. Limites : rien au-delà de 40 km ; une montagne cachée derrière l'horizon ne fait pas d'ombre ; un texel de 21 m de 3 à 40 km.
- ⬜ Carte d'horizon du relief (choix de Bruno du 2026-11-01, après la carte lointaine) : ombre des montagnes jusqu'à l'horizon, vue de l'orbite, calculée par sommet dans le calcul des tuiles.
- ⬜ Ombres de contact (rayon court dans la profondeur de l'écran), puis cartes d'ombre proches en 4096 en qualité Élevé.
- 🔨 Scènes de test du rendu (`assets/scenes/Rendu/`, 2026-11-10) et vue de débogage « Cartes d'ombre en couleur »
  (panneau Moteur). Fait : scène des ombres (poteaux fins, contact au sol, plans inclinés, abri, barreaux, repères de
  cascades, tour à 350 m). À faire : sphères de matériaux, intérieur sombre, lumières locales. Défauts relevés à
  corriger : pas de fondu entre cascades proches, 23 cm par texel de 60 à 200 m, décalage normal jusqu'à 80 cm (ombres
  décollées). Corrigé : pénombre selon la distance (PCSS) ; un objet à plus de 300 m vers le soleil
  (tour, station) est aplati dans les cartes jusqu'à 3 km au lieu d'être coupé (son ombre dépendait du regard).
- 🔧 Ciel spatial généré ou panorama ; volumes « Intérieur » ; lumière de contour. Reflets et ambiante tirés du ciel par le GPU, en HDR, refaits seulement quand le ciel change (`EnvironmentRenderer`, 2026-11-02) : cube de 128 pixels préfiltré GGX (un niveau de mip par rugosité), lumière diffuse dans un cube d'irradiance ; plus de cube 8 bits calculé par le CPU ni de copie C++ du bruit du ciel. À améliorer : ciel seulement (ni la scène, ni l'air de la planète), pas de reflet net (128 pixels), intérieurs en couleur unie.
- 🔧 Exposition automatique et courbe de tons AgX (2026-11-02, à la place d'ACES ; contraste et saturation à mi-chemin entre AgX de base, qui délavait la planète, et « Punchy », trop proche d'ACES) : histogramme de luminance pondéré au centre calculé par le GPU, adaptation de l'œil réglable dans le Ciel (décalage, bornes, vitesse), exposition affichée dans le panneau Moteur ; pré-exposition (2026-11-03 : la scène écrite déjà exposée, prête pour des lumières physiques en 16 bits). À améliorer : réglages par défaut à ajuster à l'œil ; pas de mesure locale (zones claires et sombres à la fois, comme un cockpit face au soleil).
- 🔧 Unités physiques (2026-11-03) : soleil en lux, lampes et réacteurs en lumens (affaiblies en 1/d²), fond du ciel, émissifs et ambiance des intérieurs en cd/m², bornes de l'exposition en EV100 ; anciennes scènes converties (soleil, fond, émissifs, réacteurs, lampes x 2500 vers des lumens réalistes) ou remises aux valeurs réelles par défaut (intérieurs, bornes : choix de Bruno) ; lumière ambiante du ciel à l'échelle d'avant (débouche les ombres) ; étoiles et nébuleuses du fond avec un éclat minimum à l'écran (choix de Bruno : visibles près d'une station éclairée), effacées par la clarté de l'air à l'écran (ciel de jour, halo) quelle que soit la densité de l'atmosphère ; verres (transparents lisses) sans diffus, reflets seuls (diélectriques à 4 %, sans le ciel vus de l'intérieur), ombres des objets reçues. À améliorer : le verre assombrit le fond sans le teinter (teinte colorée : deux passes ou mélange à deux sources) ; reflet de l'habitacle sur la verrière absent (sonde de reflets du vaisseau) ; valeurs de la station et des modèles à ajuster à l'œil ; le soleil ne dépend pas encore de la distance de l'Orbite ; côté ombre d'un vaisseau presque noir dans l'espace (pas encore de lumière renvoyée par la planète).
- 🔧 Exposition propre aux intérieurs (2026-11-03, d'après une image de Star Citizen) : compensation (-1 stop par défaut) et bornes de l'œil (EV100 3 à 7, choix de Bruno : l'image bouge peu en regardant les lampes ou une porte au soleil) réglées dans l'Intérieur, pour garder une station sombre et contrastée au lieu de la remonter au gris moyen. La boîte d'un Intérieur compte en entier (sol et murs posés sur ses faces), le fondu se fait dehors. Dans l'espace, le ciel n'entre plus dans la mesure de la lumière (ses étoiles, à éclat minimum à l'écran, fermaient l'œil jusqu'à la borne). À améliorer : valeur à régler à l'œil pièce par pièce.
- ⬜ Étalonnage couleur par table (LUT 3D après la courbe de tons), réglable par scène.
- ⬜ Éclairage réaliste, étapes suivantes (plan du 2026-11-02, choix de Bruno : fondations d'abord) : rendu différé, occlusion ambiante GTAO, sondes de reflets dans les vaisseaux, reflets de l'écran, ombres des lampes, brouillard volumique, lumière indirecte.
- ⬜ Lumière renvoyée par la planète proche (remarque de Bruno du 2026-10-26) : dans l'espace, la face d'un objet à l'ombre du soleil ne reçoit que l'ambiante d'un ciel noir et paraît grise ; une lueur bleutée venue de la planète (selon sa taille apparente et sa face éclairée) éclairerait les dessous comme dans les rendus de la station.
- ⬜ Matériaux multicouches (remarque de Bruno du 2026-10-26) : une matière de base (peinture, métal nu) et des couches mélangées par masque (usure, saleté, bandes), comme les « layer blends » de Star Citizen. Aujourd'hui : un seul jeu de textures par matériau.
- ⬜ Décalcomanies (idée de Bruno du 2026-10-26) : images projetées sur les modèles (numéros, logos, bandes), posées dans l'éditeur. En attendant, plaques texturées dans le modèle.
- ⬜ Ciel plus fin (nébuleuse en 2048 ou 4096, compressée BC6H) réglable dans les détails des options graphiques (idée de Bruno).
- ✅ Élimination hors champ, opaques triés, mesure CPU et GPU (panneau Moteur en sections repliables : temps par passe du graphe de rendu, étapes, scène, mémoire) et de la mémoire (RAM du processus, mémoire vidéo).
- ✅ Relevé des mesures dans un fichier (demande de Bruno du 2026-10-22) : bouton « Enregistrer les mesures (10 s) » du panneau Moteur, ou `FLUX_MESURES=secondes` dès la fin du chargement ; `Mesures/mesures-<date>.txt` à côté de l'exécutable avec réglages, caméra, durées d'image (médiane, 1 % les plus longues), CPU, passes et étapes du GPU (calculs détaillés : tuiles de planète, semis et tri des arbres, objets cachés) et compteurs de la scène (moyenne, min, max ; tuiles calculées et plaines semées à chaque image).
- ✅ Feuillage des arbres sans éclairage caché : pré-passe de profondeur (seuil d'alpha), puis éclairage de la seule feuille visible ; ombre des nuages par sommet (mesuré au profiler : 40 % de l'image avant). Feuilles sans reflet du soleil (blanches à contre-jour avant), lumière enveloppante et traversante réglables dans Végétation.
- 🔧 Sol des planètes allégé : tuiles de près à loin (par bandes de distance), ombre des nuages par sommet, lectures sous 5 % de poids et couleur moyenne de près évitées. Mesh shaders quand la carte les a (voir « Fonctions selon le GPU »). À améliorer : coût des pixels encore à mesurer chez Bruno.
- ✅ Diagnostic de plantage GPU : étape bloquée et panne du pilote dans plantage-gpu.txt (repères AMD, VK_EXT_device_fault).
- ✅ Étapes de l'image nommées pour les outils de mesure (Radeon GPU Profiler, Nsight, RenderDoc), en Debug comme en Release (VK_EXT_debug_utils si présente).
- ✅ Calcul sur GPU (compute shaders dans le RHI).
- ✅ Flammes des réacteurs (composant « Réacteur ») : volume de gaz calculé au pixel, longueur et éclat selon la
  poussée du vaisseau, lumière autour ; à pleine poussée dans l'éditeur (jeu arrêté) pour les régler.

### Planètes (Planetology)
- ✅ Code des planètes réuni dans le domaine Planetology (`Flux::Planetology`, dossiers `Planetology/`).
- ✅ Cube-sphère en quadtree, LOD sans popping (CDLOD), tuiles calculées par le GPU, tout en float (déformation au centre de la tuile calculée en double par le CPU, le GPU n'en calcule que le changement : sol affiché et collision au millimètre près du sol sur une planète comme la Terre, plus de doubles lents sur le GPU) ; une tuile pas encore calculée ne remplace que son quart de la parente (le sol ne clignote plus quand la caméra tourne). Horizon calculé pour chaque tuile (son point le plus haut) ; sol calculé en avance là où va la caméra (2 s de sa vitesse : rien ne change à pied, tuiles prêtes en vaisseau), sans être dessiné avant d'être assez près : le détail ne retombe plus à l'arrêt de la caméra.
- ✅ Atterrissage depuis l'espace sans chargement ; collision du sol sur le CPU en double, autour des objets. Tuiles de collision construites sur 1 à 4 fils (un quart des cœurs), les plus proches d'abord, sans refaire une tuile en cours (environ 2,5 ms par tuile mesurées sur un Xeon à 2,1 GHz).
- ✅ Atmosphère (ciel vu du sol, brume, halo), océans avec vagues.
- 🔧 Soleil posé dans l'espace (composant « Soleil ») : lumière, ombres et disque viennent de sa position ; jour d'un côté de la planète, nuit de l'autre. Pas de cycle jour / nuit (ni la planète ni le soleil ne bougent). Disque et lumière de couleurs séparées ; dans l'air d'une planète, le ciel de l'espace n'éclaire plus de jour (l'atmosphère éclaire à sa place) et peu la nuit. Soleil fixe au centre de la scène, composant « Orbite » (distance, angle, inclinaison) pour ranger les planètes autour, comme dans EVE Online. Lumière de contour (« Lumière de contour ») aussi sur les planètes, liseré selon la sphère, vue de l'espace seulement (effacée sous deux hauteurs d'atmosphère : au sol elle voilait tout le terrain). À améliorer : pas de cycle jour / nuit.
- ✅ Ciel bleu de jour : étoiles et nébuleuse effacées par la lumière de l'air.
- ✅ Biomes par climat et sol texturé (5 matières, plaquage triplanaire).
- ✅ Teinte de chaque texture du sol (herbe verdie par défaut : son image se confondait de loin avec la roche).
- ✅ Plage réglée en mètres au-dessus de la mer (elle suit le niveau de la mer).
- ✅ Sol sans répétition visible : pavage hexagonal (rotation et décalage aléatoires par cellule, mélange des 3 cellules voisines).
- 🔧 Sol moins cher à dessiner : 2 matières, 2 projections et 2 cellules au plus par pixel, 2 textures au lieu de 3. Textures compressées en BC7 (bc7enc) quand le GPU lit les formats BC : 4 fois moins de mémoire vidéo (220 -> 56 Mo), sans gain de vitesse mesuré sur une RX 7700 XT (les pixels du sol, 0,5 ms, coûtent par leurs calculs) ; compressées au premier chargement (quelques secondes, sur d'autres fils), puis relues dans `bin/Cache/sol-bc7.bin` tant que les images ne changent pas. Reste : au-delà de 2 km, couleur moyenne de la matière (le relief ne se lit plus de haut) ; la compression devrait se faire à la compilation avec les qualités de texture (1K, 2K, 4K), pas sur la machine du joueur.
- 🔧 Rotation du sol bornée par matière (`Textures/Sol/matieres.json` ; images rangées dans un sous-dossier par matière, `Textures/Sol/roche/`…) : sable et neige ±10° (rides dans le même sens), roche, herbe et terre libres. À améliorer : à la jonction de deux axes de projection, la direction des rides change un peu.
- ✅ Sol vu de haut : texture fondue vers sa couleur moyenne entre 600 m et 2 km (plus de motif régulier de points sombres, moins de lectures au loin).

### Monde, scène, physique
- ✅ Scène EnTT, secteurs int64 + positions double (échelle galaxie), sauvegarde JSON (`flux.scene`).
- ✅ Composants enregistrés une fois : inspecteur et sauvegarde générés.
- ✅ Chargement qui ne bloque jamais sur un fichier abîmé (Bruno, 2026-10-27) : composant illisible (champ inconnu
  d'une version plus récente, valeur invalide) pas chargé mais gardé tel quel et réécrit ; transformation lue champ par
  champ ; exemplaire dont le prefab est introuvable gardé vide avec ses différences ; entité sans parent mise à la
  racine ; un avertissement au journal pour chacun. Seul un fichier inutilisable (pas du JSON, format plus récent,
  identifiants en double, boucle) est refusé. Scène de démarrage absente ou refusée : éditeur et jeu démarrent sur une
  scène vide. Version du moteur notée dans chaque fichier enregistré.
- ✅ Physique Jolt : objets simulés, personnage (capsule, immobile sur les pentes jusqu'à 50°, glisse au-delà, accélère et freine selon son « Accélération » : un appui bref fait un petit pas, sans glisser), un monde physique par vaisseau (on marche à bord pendant qu'il vole).
- ✅ Vaisseaux : pilotage 6 axes avec inertie, souris en pointeur virtuel (manche : vitesse de rotation demandée, prise à l'accélération de chaque axe, réglable par vaisseau), tête du pilote en retard sur les virages, champ de vision élargi en accélérant, vol atmosphérique, assiette qui suit la courbure de la planète (aide à plat), mode voyage (ralenti à l'approche d'une planète, permis en haute atmosphère et bridé à mesure que l'air s'épaissit, vitesse rendue à sa sortie), sièges, atterrissage stabilisé.
- ✅ Vue extérieure du vaisseau piloté (V, molette) : suit son cap sans son roulis (on le voit s'incliner), horizon redressé sur la verticale d'une planète ; dans l'espace, la caméra garde son dernier haut.
- 🔧 Animation squelettique (fondus, repos/marche/course/chute/s'asseoir ; les pas suivent la vitesse du personnage, aussi pendant le fondu). À améliorer : pas d'accès aux os pour le gameplay, pas d'événements d'animation, la course repart du début de son cycle quand on passe de la marche à la course (pas de calage des pas).
- ✅ Son 3D (miniaudio), actions d'entrée réassignables (Touches.json).
- 🔧 Sauvegarde rapide (F8 enregistre la partie, F9 la recharge, F7 l'efface ; reprise au lancement du jeu ; Jouer dans l'éditeur part toujours de la scène éditée) : toute la scène en cours, vitesses des vaisseaux et siège du joueur compris. Les systèmes du jeu y ajoutent leur état (`Engine::AddGameSaveData`) : la mission en cours est gardée. Scène modifiée dans l'éditeur depuis la sauvegarde : sa version actuelle est chargée, avec ce que la partie y avait changé (fusion à trois versions champ par champ, `SceneMerge` ; l'éditeur l'emporte sur un même champ). F8 dans le mode Jouer de l'éditeur garde aussi la scène éditée de départ : le jeu y fusionne ensuite les modifications. À améliorer : un seul emplacement, ni le pilote automatique ni le warp en cours ; une sauvegarde d'avant la fusion, ou d'une scène jamais enregistrée, est reprise telle quelle (le jeu le dit, F7 l'efface).
- ✅ Composant « Monde » (comme les World Settings d'Unreal) : gravité et air hors planètes, freinage par l'air des objets et des personnages (aussi dans l'atmosphère des planètes).

### Interface (FluxUI)
- 🔧 Mode immédiat partagé éditeur/jeu, texte MSDF, formes SDF, thèmes. À améliorer : pas de DPI (mise à l'échelle 100 % seulement), police Latin-1.
- ✅ Interfaces du jeu en fichiers `.fluxui` : fenêtres, boutons à actions, liaisons de valeurs (rangées par catégorie), onglets (pages qui peuvent défiler), défilement, listes déroulantes (choix tapés ou donnés par la valeur liée, comme « Détail du sol »).
- ✅ Textes et images à l'écran, changement de scène.
- 🔧 Polices du projet pour les textes à l'écran (`.ttf`, `.otf` du dossier `Polices/`), atlas MSDF construit sur un
  autre fil au premier usage, attendu par l'écran de chargement. À améliorer : 0,3 à 0,5 s de processeur par police
  à chaque lancement (pas de cache disque), Latin-1 seulement, pas relue si le fichier change, pas encore dans les
  interfaces `.fluxui`.

### Outillage
- ✅ Module du jeu en DLL rechargeable à chaud, API versionnée (`GameModuleApiVersion`).
- ✅ Tests (FluxTests), CI MSVC Debug + Release, tests GPU sur GPU logiciel (lavapipe).
- ✅ Debug utilisable : Jolt optimisé en Debug, préréglage « Release + débogage », petits mondes physiques sans fils.
- ⬜ Validation Vulkan à la demande dans les versions optimisées (Bruno, 2026-11-01, quand le jeu et le moteur seront
  plus avancés) : aujourd'hui elle n'est active qu'en Debug, ce qui rend le jeu lent (46 images/s au lieu de 250).
  Une variable `FLUX_VULKAN_VALIDATION=1` l'activerait en « Release + débogage » pour jouer vite tout en vérifiant.
- ✅ Version affichée dans le jeu : numéro + commit Git.
- ✅ Instructions AVX2 (processeur minimum : Intel Haswell 2013, AMD Ryzen 2017 ; Bruno, 2026-10-28) pour tout le projet, Jolt compris (il restait en SSE2 avec Ninja), sans FMA implicite (relief C++ identique au shader). Pas de SIMD écrit à la main : à envisager pour le bruit des tuiles de collision (`Terrain::GetHeight`, 4 points à la fois) seulement si une mesure le justifie (mesuré : 2,5 ms par tuile, -13 % avec la seule option AVX2).
- 🔧 Classe `Engine` allégée (demande de Bruno du 2026-10-25) : ses services sont des classes privées de `src/Core` qu'elle possède (caméra du joueur, sauvegarde rapide, liaisons des interfaces, environnement de vol et message court, écran de chargement, panneau Moteur) ; les changer ne recompile ni l'éditeur ni le jeu. À améliorer : `Engine::Run` et `UpdateGameSystems` restent longues (les infos de vol sont passées au jeu le 2026-11-01).

## Prochaines étapes

### Multijoueur (Bruno, 2026-11-08 ; plan : https://claude.ai/artifact/KpJC18BJG7EqMyQzbWAruu)
Pensé dès le départ pour un MMO : serveur qui fait autorité, un programme `ZoneServer` par zone (espace, sol, instance),
transfert d'autorité entre zones, shards, GameNetworkingSockets, PostgreSQL plus tard, serveur portable Linux.
- 🔨 1. Pas fixe de 60 Hz pour tout le jeu (`SimulationClock`), touches retenues jusqu'au pas, rendu interpolé entre les deux derniers pas. Limites : projectiles et animations non interpolés (à 60 pas par seconde).
- 🔧 2. Commandes de joueur (`PlayerCommand` : marche, manche, vue, un bit par action) remplies une fois par pas, seule lecture du clavier pendant un pas ; verrou et groupes d'armes rangés par joueur. Limites : l'écran de changement de scène lit encore le clavier.
- ✅ 3. `FluxSim` (simulation, sans Vulkan ni Win32) séparé de `FluxEngine` : FluxSim.dll dans `Sim/` (scène, physique, planètes côté CPU, modèles ; libFluxSim.so sous Linux), compilée par la CI Linux (GCC 14) avec ses tests (FluxSimTests).
- 🔧 4. `ArsSolarisServer` sans rendu (`Game/Server/`, Windows et Linux) : charge le projet et une scène, fait tourner le jeu simulé (`GameSim`, lié en statique) à 60 pas par seconde, bilan dans la console toutes les 10 s, arrêt propre par Ctrl+C ; la CI le lance 600 pas. Classe `Simulation` (FluxSim) sortie d'`Engine` ; code du jeu coupé en `GameSim` (état par simulation) et `GameClient` (touches, affichages, éditeur), bits des actions déclarés par la simulation. Limites : aucun joueur ni réseau (étapes 5 et 6), durées des animations des sièges inconnues du serveur (lues par le rendu sur le client).
- 🔧 5. Transport, session, vérification de version : module `FluxNet` (`Net/`, DLL, Windows et Linux) sur GameNetworkingSockets, `Net::Server` et `Net::Client`, poignée de main qui compare les versions de FluxNet et du protocole du jeu (`GameProtocolVersion`) ; `ArsSolarisServer` écoute le port 27015 (`--port`), « Ars Solaris.exe --serveur adresse[:port] » s'y connecte, connexions et refus au journal. Limites : adresse IP seulement (pas de nom de domaine), rien d'affiché à l'écran, pas de message du jeu avant l'étape 6 ; mise à jour roulante des serveurs plus tard, vers l'alpha.
- 🔨 6. Réplication des champs marqués dans `ComponentRegistry`, interpolation des autres. Avec elle, perte du serveur (Bruno, 2026-11-08) : boîte de dialogue « connexion au serveur interrompue » (avec la raison), OK ramène au menu principal ; une coupure courte est d'abord réessayée quelques secondes sans rien afficher.
  - 🔧 6a. Instantanés du serveur vers le client : champs marqués `.Replicated()` (pour l'instant la place des entités), différences depuis le dernier instantané reçu, 30 par seconde ; entités créées pendant le jeu envoyées en entier ; le client avec `--serveur` charge la scène du serveur et l'affiche interpolée, 100 ms en retard. Limites : le joueur regardait seulement (joue depuis 6b-2, parents répliqués aussi), pas de limite de taille par envoi (étape 10).
  - 🔧 6b. Plusieurs joueurs. Fait (6b-1) : la simulation fait marcher, asseoir et piloter chaque personnage joué avec sa commande (`Simulation::GetPlayers`, `SetPlayerCommand`) ; missions, navigation et messages de chaque joueur sur son personnage (chacun sa mission, choix de Bruno) ; les ennemis visent le vaisseau de joueur le plus proche. Fait (6b-2) : `--joueur Nom` (comptes simples, sans sécurité) ; point d'apparition (`spawnPoint`, prefab du personnage) où le serveur crée le personnage à l'arrivée et le retire au départ, le joueur posé dans la scène retiré ; commandes du client à chaque pas (3 dernières répétées, bornées par le serveur) ; caméra sur son personnage ; animations des personnages déduites sur le client de leur mouvement (repos, marche, course, chute). Limites : pas de prédiction (~100 ms de retard sur ses propres gestes), sièges non répliqués (piloter depuis un client ne marche pas encore), position de la vue crue sur parole.
  - ⬜ 6b-3. Scène de sélection du personnage (« Rejoindre », retour après une coupure), avec 6c.
  - ⬜ 6c. Perte du serveur : boîte de dialogue, retour au menu.
- ⬜ 7. Prédiction et réconciliation du vaisseau du joueur.
- ⬜ Persistance (Bruno, 2026-11-09) : personnages, vaisseaux et monde dans PostgreSQL, lus et écrits par le serveur seul (`libpq`) ; Docker (`docker-compose.yml`) en développement et en CI.
- ⬜ Comptes (Bruno, 2026-11-09) : jusque-là un nom au lancement (`--joueur`) ; avant l'ouverture aux joueurs, connexion par Steam (jeton vérifié par le serveur, aucun mot de passe chez nous).
- ⬜ 8. Solo et « Jouer » de l'éditeur par un serveur local intégré.
- ⬜ 9. Deux `ZoneServer`, transfert d'autorité, copies en lecture seule à la frontière.
- ⬜ 10. Envoi selon la distance, test de charge avec des bots.
- Planètes en réseau (Bruno, 2026-11-08) : relief et décor générés par le client depuis la graine, le serveur ne calcule que la collision autour des joueurs (CPU, double) ; la persistance ne garde que les différences (bases, objets, ressources). Relief CPU et GPU strictement égaux (`PlanetGpuTests` les compare déjà).

### Missions en graphe de nœuds (Bruno, 2026-11-08 ; après tout le multijoueur)
Référence : Subsumption de Star Citizen (éditeur visuel, logique en C++).
- ⬜ Mission décrite en JSON (`.fluxmission`) : étapes, déclencheurs, conditions, récompenses, reliés en graphe.
- ⬜ Exécution par le serveur (`ZoneServer`), état de chaque mission par joueur ou groupe ; nœuds écrits en C++ dans le jeu.
- ⬜ Éditeur visuel du graphe dans FluxEditor (voir `Editor/ROADMAP.md`).
- ⬜ Plus tard : scripts Luau en complément, pour les cas spéciaux.

### Performances au sol (constaté : 150 images/s en Release face à l'océan)
- ✅ Démarrage : textures du sol décodées en parallèle sans bloquer le jeu, police et images optimisées en Debug.
- ✅ Air, climat et reflet du ciel sur l'océan calculés par sommet pour le sol et l'eau, plus au pixel.
- 🔧 Ciel et air lus dans deux tables refaites à chaque image quand la caméra est dans l'air (Hillaire 2020), au lieu
  de 16 à 24 pas par sommet et par pixel ; écart mesuré sous 1,5 % de la marche d'avant. Limites : vu de l'espace,
  marche d'avant (l'air n'y couvre qu'un anneau) ; une seule diffusion (pas de diffusion multiple).
- ✅ Tuiles des planètes dessinées par appels indirects : un appel par page de 256 tuiles et par passe (sol, eau,
  chaque cascade d'ombres) au lieu d'un appel par tuile.
- ⬜ Perspective aérienne précalculée (texture 3D) pour l'air devant les modèles, si l'air par sommet ne suffit pas.
- 🔨 Océan : tuiles plus grossières et moins nombreuses. Fait : l'eau, le fond et les terres basses sous l'horizon de la mer ne sont plus gardés (scène d'essai à 1 300 m : 20 tuiles d'eau → 10, 313 tuiles → 291). Reste : tuiles d'eau plus grossières que celles du sol ; eau cachée derrière une montagne écartée par la pyramide de profondeur seulement avec les mesh shaders.
- ✅ Réglage « Qualité des nuages » (Bas, Moyen, Élevé par défaut : pas le long du rayon et vers le soleil), aux mêmes endroits que « Détail du sol ».
- 🔧 Réglage « Distance de la végétation » (Bas 50 %, Moyen 75 %, Élevé 100 % par défaut, Ultra 150 % de la portée de la planète), aux mêmes endroits que « Détail du sol » et dans les préréglages. Limite : en Ultra, au-delà de 256 plaines en vue, les plus lointaines restent sans arbres.
- 🔧 Réglage « Détail du sol » (Bas, Moyen, Élevé par défaut, Ultra : tuile coupée à 3, 4, 5 ou 6 fois son bord), dans les Paramètres de l'éditeur, les réglages du jeu et les listes déroulantes des interfaces. Limite : choisi à la main, pas encore selon la carte graphique ni la vitesse mesurée.

### Rendu
- 🔧 HDR + bloom (lueurs, soleil, réacteurs) ; soleil dans son propre shader (disque assombri au bord, halo, rayons en étoile ou long trait horizontal façon EVE Online, au choix du composant Soleil). Force réglable dans Paramètres > Graphiques de l'éditeur ; le jeu garde la force par défaut (réglage du jeu à venir).
- ⬜ Particules sur le GPU : armes, explosions, étincelles, fumée (et poussière sous les réacteurs au sol).
- 🔧 Anticrénelage temporel (TAA) : sous-pixels de Halton, reprojection par la profondeur, mouvement propre des objets (vaisseau, cockpit), voisinage en YCoCg ; grain des nuages qui change à chaque image et s'efface. Limites : le mouvement des os d'un personnage animé n'est pas compté (le voisinage rattrape), les nuages ont leur propre historique (demi-résolution) avant le TAA.
- 🔨 Mise à l'échelle FSR 3.1 d'AMD (Bruno, 2026-10-25) : réglage « Mise à l'échelle » (Natif avec notre TAA, FSR Natif AA, Qualité par défaut, Équilibré, Performance, Ultra Performance) et netteté, dans les Paramètres de l'éditeur, `Reglages.json` et les listes des interfaces. La scène est dessinée à la taille de rendu puis agrandie par la DLL signée d'AMD (téléchargée par CMake, chargée au démarrage ; absente ou refusée : Natif). Fait (étape 1) : taille de rendu séparée de la vue dans le graphe (nuages en demi-taille de rendu), mouvement complet (caméra + objets) pour FSR, sous-pixels selon le rapport. Étape 2 : masque réactif écrit par les nuages, les flammes et les traçantes (les nuages tremblaient en FSR). Reste : transparents hors du masque (vitres : traînées possibles), biais de mip des textures (un peu floues en Performance), mesures. Pas de génération d'images (écran 60 Hz).
- ⬜ Ombres des lumières intérieures.
- ⬜ Champs d'astéroïdes (l'instancing des modèles est fait : reste à les générer et à les placer, comme les arbres).
- ⬜ Rendu vers texture : écrans du cockpit, interfaces dans le monde 3D.
- 🔧 Fonctions selon le GPU (décisions de Bruno du 2026-10-19 et du 2026-10-29) : chaque fonctionnalité est conçue pour la configuration recommandée (classe RTX 3060 / RX 6700 XT), puis reçoit un repli pour la configuration minimum (GTX 1060 / RX 5700). Le moteur détecte au démarrage ce que la carte sait faire et choisit le chemin, sans réglage à toucher (le panneau Moteur dit le chemin pris). Fait : sol des planètes en mesh shaders (VK_EXT_mesh_shader : RTX 20, RX 6000, Arc et plus récentes) : chaque tuile en 16 paquets de 8x8 carrés (groupes de 128 threads, ou moitiés de 8x4 à 64 threads quand le pilote préfère des groupes plus petits), le GPU écarte les paquets hors champ ou cachés derrière le relief (pyramide de profondeur de l'image précédente). Le chemin de base reste CDLOD + dessin indirect, qui tourne sur une GTX 1060 ou une RX 5700. À améliorer : gain à mesurer en Release chez Bruno ; sommets des bords de paquets calculés deux fois (environ 27 % de sommets en plus) ; un paquet qui réapparaît derrière une crête peut arriver une image en retard ; pas d'élimination des faces de dos par paquet (le sol fait face à la caméra, gain faible) ; modèles et arbres restent en vertex shaders.
- ⬜ Arbres en mesh shaders (Bruno, 2026-10-22, à voir après la mesure du sol) : modèles découpés en paquets à l'import (meshoptimizer), paquets cachés écartés par le GPU et détail choisi paquet par paquet (façon Nanite) au lieu des 3 niveaux fixes : plus de détail de près pour le même temps GPU, transitions moins visibles.
- ⬜ Ray tracing pour l'éclairage et les reflets (Bruno, 2026-10-22) : beaucoup plus tard, une fois le moteur abouti et stable ; facultatif et détecté au démarrage comme les mesh shaders (la configuration minimum n'en a pas).
- 🔧 Relief plus riche (plan en 10 étapes : `/mnt/project-files/Recherche/relief-planetes-plan.md`) : bruit avec sa pente exacte, érosion analytique (versants lisses, détail sur les crêtes), déformation du domaine (côtes découpées), chaînes de montagnes allongées au cœur des continents, vraies montagnes : sommets placés un par un, aux versants de pentes différentes, creusés de ravines en arbre (bruit d'érosion), largeur des montagnes en mètres quel que soit le rayon (réglage « Largeur des montagnes »). À améliorer : la raideur dépend du rapport relief / largeur (trop de relief = aiguilles) ; pas de vraies vallées ni de rivières, ni de plateaux, de falaises ou de cratères ; une seule forme de terrain pour toutes les planètes (types de planète à faire, voir plus bas) ; contreforts encore réguliers, pas de rochers posés sur les versants.
- 🔧 Arbres semés et triés par le GPU (`VegetationComponent`) : 3 niveaux de détail (détaillé, simplifié qui fait
  aussi les ombres, imposteur espacé au loin), vent, jusqu'à 3 km, puis la couleur des forêts sur le sol (visible de
  haut et de l'espace). Semis réparti sur 16 groupes de threads par plaine (un seul groupe montait à 7,6 ms en vol
  au-dessus des forêts de montagne). Plaines semées d'avance autour de la caméra et sur son trajet (800 m, 64 au
  plus), arbres d'une plaine semée en vue qui apparaissent un à un en 0,5 s ; budget de 4 plaines semées par image
  encore à régler sur mesure. Conifères générés (épicéa, sapin et pin, deux variantes chacun, `Modeles/Coniferes`) en plus
  du feuillu ; plan de la forêt façon Star Citizen en 4 étapes (`/mnt/project-files/Plans/arbres-realistes.md`).
  Biomes et espèces en fichiers (`Vegetation/Biomes/*.biome.json`, `Vegetation/Especes/*.espece.json`, relus à chaud,
  réglés dans le panneau Végétation de l'éditeur avec aperçu en direct, `Planetology::SetVegetationPreview`) :
  une planète liste jusqu'à 8 biomes, chacun avec ses conditions (plaines, collines, montagnes et piémonts, altitude,
  température, humidité), ses forêts et jusqu'à 4 espèces avec leur part ; le premier qui convient prend le sol.
  Livrés : marais, désert, montagne (épicéas et sapins), taïga, pinède, plaine (feuillus). Teinte propre à chaque
  arbre, couleur des forêts lointaines selon le biome. Tailles d'arbres adultes (2026-11-04) : épicéa 29-50 m, sapin
  28-46 m, pin 26-43 m, feuillu 20-33 m (le chasseur fait 21 m de long) ; à faire : conifères au tronc nu sur 40-50 %
  de la hauteur, comme les forêts de Star Citizen (branches presque jusqu'au sol aujourd'hui). Humidité en bandes de latitude comme sur Terre (déserts vers
  25-30°, taïga vers 55-65°, toundra au-delà). Espacement par espèce (m) et facteur par biome (demande de Bruno du
  2026-10-28) : une place sur k×k de la grille garde l'arbre, posé au hasard dans son bloc ; deux arbres d'une même
  espèce restent à 40 % de l'espacement au moins, et les places qu'aucun bloc ne garde sont écartées avant le relief.
  À améliorer : pas de sécheresse au cœur des continents ni d'ombre de
  pluie derrière les montagnes ; espacement arrondi à la case du semis (environ 6 m) et pas garanti entre deux
  espèces d'espacements différents ; 8 variantes d'arbres au plus par planète ; imposteurs en 3 quads croisés (plats vus
  d'en haut) ; passage d'un niveau de détail à l'autre sans fondu ; arbres hors du champ sans ombre ; pas
  d'occlusion (un arbre derrière une montagne ou derrière d'autres arbres est quand même dessiné : prochaine étape,
  avec la pyramide de profondeur des objets) ; une seule planète à arbres à la fois ; au loin, l'arbre posé sur le relief fin peut flotter ou
  s'enfoncer un peu sur le sol plus grossier.
- 🔧 Collision des arbres : les plaines autour de ce qui peut les toucher (vaisseaux, personnages, objets simulés) sont
  resemées par le CPU en double (`Planetology/TreeSowing`, copie du semis du GPU, mêmes arbres à 0,5 % près, vérifié
  par `VegetationGpuTests`) sur les fils des tuiles de collision, une capsule Jolt par tronc (rayon et hauteur par
  espèce, panneau Végétation), un corps par plaine ; les 32 dernières plaines restent gardées (allers-retours sans
  refaire). Hasard du climat et des forêts haché sur des entiers (le même sur tous les GPU et le CPU ; les forêts ont
  changé de place). À améliorer : environ 20 ms de fil par plaine semée (mesuré sur un Xeon à 2,1 GHz, 600 arbres) ;
  troncs droits sans branches (un vaisseau passe dans les cimes) ; arbres qu'on ne peut ni couper ni planter (le jour
  venu : enregistrer seulement les changements, plaine + place, façon Minecraft, lus par les deux semis).
- ⬜ Types de planète (demande de Bruno du 2026-11-07) : rocheuse (celle d'aujourd'hui), martienne (sol rouge, air
  mince et poussiéreux, sans eau, cratères et canyons), gelée (glace et neige, mers gelées, crevasses), de lave
  (roche sombre, lave lumineuse dans les creux, fumées, air chargé). Un préréglage choisi dans l'inspecteur règle
  relief, matières du sol, climat et biomes, atmosphère et surface liquide (eau, glace, lave émissive) ; chaque
  réglage reste modifiable ensuite. Voir l'étape 8 de `PLANETOLOGY.md`.
- ⬜ Canyons (demande de Bruno du 2026-10-21) : gorges creusées dans le relief, parois raides en strates, fond plat ou
  sinueux, sur les planètes sèches comme le long des rivières. À faire dans `Terrain::GetHeight` et `Height`
  (`PlanetTile.slang`) ensemble ; les parois presque verticales demanderont plus de sommets ou une matière de falaise.
- ⬜ Rochers, buissons et herbe posés sur le sol (même semis que les arbres).
- ⬜ Mélange des matières par la hauteur des textures (displacement) plutôt qu'un fondu.
- 🔧 Nuages : une couche épaisse par planète (`CloudsComponent`), en volume (64 pas en demi-résolution au plus, allongés entre les nuages, reprojetés d'une image à l'autre), cumulus et stratus, avec leur ombre au sol. Coût mesuré chez Bruno (RX 7700 XT, 1080p, ciel plein écran) : 2,1 ms avant les pas allongés. Limites : pas de masque réactif pour FSR, qualité choisie à la main.
- 🔨 Nuages volumétriques (3 étapes) : 1) ✅ bruit 3D (forme Perlin-Worley 128³, détail Worley 32³) calculé une fois
  par le GPU, avec ses mips ; 2) ✅ rendu en demi-résolution avec reprojection, 64 pas, ombre interne
  vers le soleil, diffusion multiple, cumulus et stratus ; 3) ✅ pas allongés dans le vide (environ 45 % de
  calcul en moins) et réglage « Qualité des nuages » (Bas 32/4 pas, Moyen 48/5, Élevé 64/6).
- ⬜ Remplacer la texture de sable (aerial_beach_01), trop marron.
- ⬜ 2 ou 3 textures par matière, pour varier davantage le sol de près.

### Gameplay
- 🔧 Gameplay hors du moteur (Bruno, 2026-10-19 : « pour les missions, le jeu pourrait s'en charger » ; audit : `/mnt/project-files/Recherche/audit-moteur-2026-10-19.md`). Fait : systèmes du jeu enregistrables à une étape de l'image (`Engine::AddGameSystem` : Interaction, BeforePhysics, AfterPhysics, Hud, Reset) ; missions, choix de la cible de l'IA de combat et commandes des armes du joueur (verrou, groupes, tir auto, leur affichage) dans GameModule.dll. Socle (2026-11-01) : code du jeu en bibliothèque `GameLogic` testée par FluxTests (2026-11-08 : coupée en `GameSim`, `Game/Sim/src`, et `GameClient`), objets du jeu dans Placer déclarés par le jeu (`Engine::GetEditorExtensions`). Missions sorties : `MissionGiverComponent` et ses briques (`Game/Module/src/MissionGiver.cpp`). IA de combat sortie (`Game/Module/src/CombatAI.cpp`, `Steering.hpp` devient une brique publique du moteur). Amarrage et warp sortis (`Game/Module/src/Docking.cpp`, `Warp.cpp`, `Navigation.cpp` pour les touches L et J et leur affichage) : `ShipComponent` ne garde que deux drapeaux génériques (`autopilot` : mené par le jeu ; `passThrough` : sans collision), la vitesse de warp devient le composant « Moteur de warp » du jeu, ajouté à la lecture des anciennes scènes (`ComponentRegistry::AddMovedField`). Armes sorties : emports, armes et points de vie au jeu (`Game/Module/src/Weapons.cpp`, dégâts dans un système du jeu, aimant de l'éditeur déclaré par le jeu), le moteur garde les projectiles et leurs traçantes (`Projectiles.hpp`). Commandes du vaisseau et infos de vol sorties (choix de Bruno : le modèle de vol `Ships::Update` reste une brique du moteur, comme le personnage) : `Game/Module/src/Piloting.cpp` lit les touches et les donne au moteur (`Engine::SetPilotInput`), `FlightInfo.cpp` dessine les infos de vol (`Engine::GetFlightState`) ; l'étape `Hud` du jeu tourne aussi en pause (temps arrêté). Reste à voir : `Seats`, la caméra du joueur (touche V) et le pimpl d'`Engine`.
- ✅ Prefabs : exemplaires qui suivent leur prefab, différences gardées champ par champ.
- ⬜ Prefab dans un prefab (station avec ses vaisseaux).
- ⬜ Construction sur les planètes (Bruno, 2026-11-09), en deux temps : d'abord des bâtiments posés (entités répliquées, fondations qui suivent la pente, pose validée par le serveur, persistance PostgreSQL) ; plus tard le relief modifié (aplanir, creuser) par une couche de différences appliquée au CPU et au GPU, enregistrée et répliquée.
- ✅ Réglages enregistrés pour Ars Solaris.exe (`GameStartup`, `Reglages.json` : fenêtre, plein écran sans bordure (Alt+Entrée, F11), V-Sync, anticrénelage, mise à l'échelle et netteté FSR, volume, panneau Moteur).
- ✅ Exe du jeu nommé « Ars Solaris.exe » (cible CMake `Game`) : les pilotes comme AMD Adrenalin le reconnaissent comme un jeu.
- ✅ Écran de chargement : la scène reste cachée et le jeu arrêté tant que le sol des planètes et ses textures ne sont pas prêts (30 s au plus).
- 🔨 Écran de chargement : attend aussi les modèles de la scène. Reste : image ou interface `.fluxui` au choix.
- 🔧 Menu Options du jeu (`Interfaces/MenuOptions.fluxui`, entité « Interface à l'écran » des scènes qui en veulent, pas
  d'un écran titre) : plein écran, opaque, HUD et scène 3D non dessinés derrière, Échap l'ouvre et met le jeu en pause ;
  Graphiques, Son, Touches, Reprendre, Quitter. Ouvrable depuis une autre interface (bouton Options d'un menu principal).
- ✅ Fenêtres d'interface à « Touche d'ouverture » (une action des Touches : inventaire…), qui l'ouvre et la ferme.
  Limites : la liste des touches est écrite dans le fichier (une action ajoutée au jeu doit y être ajoutée) ; pas
  encore le plein écran ni le choix du GPU dans les réglages ; pas de préréglages Bas à Ultra comme dans l'éditeur.
- ⬜ Événements d'animation (bruits de pas).
- ⬜ Vêtements et équipement des personnages (demandé par Bruno le 2026-10-17) : aujourd'hui, seuls les vêtements
  du même fichier que le corps s'animent. À faire : vêtements interchangeables dans leur propre fichier qui suivent
  le squelette du corps, objets accrochés à un os (casque, arme en main, sac), parties du corps cachées sous le
  vêtement ; plus tard, tissu simulé (cape) et morph targets.
- ✅ Armes des vaisseaux, étape 1 : composants « Emport » (genre, taille) et « Arme » (son enfant), tourelles qui
  pivotent vers le point regardé dans leur cône, tir par groupes d'armes sur les touches 1 à 0 (1 le nez, 2 les autocanons, 3 les missiles sur le Chasseur), projectiles en liste (pas
  des entités), traçantes dessinées par le GPU en un seul appel. Le Chasseur a 5 emports, avec leurs armes en
  modèles séparés qui pivotent (Gatling MK1 sous le nez, Autocanon MK1 sous les racines d'aile, lance-missiles).
- ✅ Armes, étape 2 : cibles et dégâts. Chaque projectile lance son segment de l'image contre tous les mondes
  physiques (`PhysicsWorld::CastRay`, le tireur ignoré) ; composant « Points de vie » sur la cible ou un ancêtre,
  détruite avec ses enfants à 0 (sauf si elle porte le joueur) ; éclat lumineux à l'impact ; Placer > Cible.
- ✅ Armes, étape 3 : missiles guidés. Verrouillage de la cible au centre de la vue à l'appui sur une touche de tir, gardé hors du viseur tant qu'elle est à portée ; les tourelles visent la cible en avance ; tir automatique (un appui active le groupe, ses armes tirent seules à portée, un nouvel appui l'arrête) (marque à quatre
  coins et distance à l'écran), un missile par appui sur la touche de son groupe, ailes à tour de rôle,
  4 missiles par lance-missiles (champ « Missiles emportés »), poursuite en avance sur un vaisseau qui vole,
  explosion plus grande à l'impact, missiles restants dans les infos de vol.
- ⬜ Armes, suite : dégâts de zone des missiles, leurres, alerte quand on est verrouillé, barre de vie de la cible,
  armes des PNJ.
- ⬜ Armes : sortir les armes dans fighter.py lui-même (aujourd'hui armes.py les retire du modèle déjà texturé, en
  couleurs unies), et un vrai modèle de lance-missiles.
- 🔧 Second chasseur « Manta » (Placer > Chasseur Manta, fichiers `Raie*.glb` : nom de code) : aile volante de 14 x 18,8 m, 53 000 triangles, 11 matériaux,
  textures 4096 calculées dans l'espace UV (scripts dans `Modeles/Chasseur2` des fichiers du projet) ; verrière orange
  jusqu'au nez, cockpit praticable (passage de 65 cm à côté du siège), deux canons fixes, gouvernes séparées de l'aile
  par un vrai interstice, tuyères à lèvres en dents de scie, siège éjectable, levier du train (pièce à part, `Raie-levier-train.glb`), marquages en décalque (nom MANTA, série, sauvetage, ne pas marcher). À faire : actionner le levier avec le train, braquer les gouvernes (pièces à part dans `Raie.glb`, fusionnées pour l'instant),
  rentrer le train (animation « Rentrer » de `Raie-train.glb`, boîtes de collision des roues) et ouvrir la verrière (pivot de `Raie-verriere-mobile.glb`
  autour de sa charnière) depuis le jeu ; collision concave du cockpit pour y marcher (enveloppe convexe aujourd'hui).
- ✅ Vaisseau sans pilote soumis à la gravité près d'une planète ou dans son atmosphère, pas dans l'espace (« Tombe
  sans pilote », décochable pour une station).
- ✅ Station, étape 1 : maquette en cubes (Placer > Station) : hangar de 140 x 30 x 100 m ouvert sur l'espace avec
  4 quais, couloir, deux boutiques (comptoir, place du marchand) et une salle de 4 terminaux ; gravité à bord, immobile.
- 🔧 Station, étape 2 : amarrage par pilote automatique (action « Pilote automatique », L : le quai libre le plus proche
  à moins de 5 km est réservé, le vaisseau y est conduit et posé, puis devient enfant de la station ; on se lève et on
  marche dans la station ; L amarré : il ressort par l'entrée et rend la main ; L en route : coupé). Composant « Quai ». À améliorer : trajet en ligne droite (voir Pilote automatique plus bas).
- ✅ Collision vaisseau contre vaisseau et contre station : un vaisseau à la racine bute sur la coque et le décor des
  autres (copies cinématiques dans le monde) comme sur le décor immobile.
- ⬜ Pilote automatique : chercher un trajet libre (aujourd'hui par-dessus ou par-dessous la station, en ligne droite :
  il reste bloqué contre un obstacle sur son chemin), éviter les autres vaisseaux.
- 🔧 Missions : un PNJ de la station (Placer > Station, boutique 1) donne une mission « détruire un groupe
  d'ennemis » (F pour parler, Accepter / Refuser / Abandonner) ; groupe à 500 km par défaut, marqueur et distance à
  l'écran, « Mission accomplie » quand tout est détruit. À améliorer : pas de récompense (voir plus bas) ; mission en cours gardée par la sauvegarde rapide.
- ✅ Warp pour les grandes distances (J vers la destination visée : station, planète, objectif de mission ; le mode
  voyage, Tab, reste un déplacement accéléré).
- ⬜ Rétrofusées vers l'avant (demande de Bruno) : freinage net, flammes visibles à l'avant, et dérive en virage
  corrigée plus fort (aujourd'hui les propulseurs latéraux ne tiennent que ShipComponent::acceleration).
- 🔧 IA de combat des ennemis (composant « IA de combat ») : patrouille, attaque, rupture, fuite ; tir avec les armes
  du vaisseau. Missions sans prefab : drones cubes armés. À améliorer : pas de factions ni de formations (voir plus bas).
- ⬜ IA : factions (qui attaque qui), escortes, formations, appels de renforts ; arbres de comportement si besoin.
- ⬜ Mort du joueur : son vaisseau ne peut pas être détruit aujourd'hui (Game::Weapons::ApplyHits le protège).
- ⬜ Missions : récompenses, autres sortes (sur les planètes et dans l'espace), par le graphe de nœuds (voir plus haut).
- ⬜ Station, étape 3 : trafic de vaisseaux PNJ qui viennent s'amarrer aux quais libres, puis repartent.
- ⬜ Station, étape 4 : marchands derrière les comptoirs et terminaux qui ouvrent une fenêtre ; vrai modèle Blender.
- 🔨 Station orbitale (vrai modèle Blender, `Modeles/Station`, demande de Bruno du 2026-10-26) : tour de 4 km, anneau haut
  de 1,6 km au symbole de Flux doré, double anneau bas, 48 portes de hangar aux halos bleus. L'intérieur
  (hangar XL d'un vaisseau à la fois, console pour en changer, cabine du joueur, promenade
  des marchands et des PNJ de mission) est fait de pièces de la station, derrière la porte du joueur.
  Fait : extérieur dans le jeu (Placer > Station orbitale), peinture blanche lisse, modules des anneaux fondus dans la coque, 850 000
  triangles, version lointaine de 110 000, collision simplifiée. Détails et décalcomanies à ajouter au cas par cas (choix de Bruno). Hangar du
  joueur dans la station, derrière le sas de sa porte, sans chargement (choix de Bruno du 2026-10-27) : on voit l'espace
  depuis le hangar ; le champ bleu se traverse, opaque du dehors. L conduit le vaisseau sur la plateforme et l'en fait
  ressortir, même garé ailleurs sur le sol. La partie commencera dans la cabine du joueur. À faire : cabine, promenade,
  console ; plus tard, charger ces pièces en arrière-plan à l'approche (comme Star Citizen) ; trajet du pilote
  automatique en ligne droite (peut buter sur la tour en arrivant de derrière).
- ⬜ Vol sous un surplomb, entrée dans un hangar étroit (enveloppe convexe du vaisseau aujourd'hui).
- ⬜ Interdire de se lever en vol hors gravité (chute sans fin).
- ⬜ GameMode (règles de partie : personnage du joueur, point d'apparition, interface, victoire), quand il y aura des règles à y mettre.

## Plus tard
- ⬜ Mises à jour, gérées par le moteur : manifeste de version (liste des fichiers et de leurs empreintes) publié en HTTPS, seuls les fichiers
  changés téléchargés puis vérifiés, retour à la version précédente en cas d'échec. Le Launcher s'en sert ; Ars Solaris.exe
  vérifie au démarrage qu'il est à jour.
- ⬜ Launcher (paramètres, bouton Jouer), par-dessus les mises à jour du moteur.
- ⬜ Protection contre la triche (Bruno, 2026-11-09) : le serveur décide déjà de tout (un fichier modifié ne change que ce que voit le tricheur). Plus tard : assets en paquets `.pak` dont l'empreinte est vérifiée au lancement (manifeste ci-dessus), envoi limité à ce qui est près du joueur (étape 10 du multijoueur), anti-triche (Easy Anti-Cheat ou équivalent) vers la bêta.
- ⬜ Version alpha livrée (Bruno, 2026-11-08) : option CMake de build monolithique, FluxSim, FluxEngine et le module du jeu liés
  statiquement avec LTO dans un seul exécutable, pour le client et pour le serveur. Le développement garde les DLL (module du jeu
  rechargé à chaud). Références : Unreal (liaison modulaire ou monolithique, publié), Star Citizen (un seul StarCitizen.exe livré).
- 🔨 Render graph (demande de Bruno, 2026-10-22), en 4 étapes :
  - ✅ 1. Squelette : passes et étapes déclarées avec leurs lectures et écritures, ordre vérifié, mesures GPU du panneau Moteur par passe et par étape.
  - ✅ 2. Barrières des textures calculées par le graphe : usage retenu par texture, une barrière par lot avant chaque passe, plus de transitions devinées dans `BeginRenderPass`/`EndRenderPass`. Les buffers gardent la barrière globale de calcul.
  - 🔧 3. Textures de travail créées et redimensionnées par le graphe (HDR, mouvement, profondeurs, historique à deux exemplaires échangés par le graphe). Limites : niveaux du bloom encore créés par `PostRenderer` ; pas de partage de mémoire (aliasing), une dizaine de Mo à gagner seulement aujourd'hui (seul le mouvement ne vit pas jusqu'à l'image finale).
  - ⬜ 4. Passes sans lecteur sautées ; rendu vers texture (écrans du cockpit).
  - Calcul asynchrone (seconde file) : ligne à part, plus haut (« Chargement et rendu communs »).
- ✅ Image du Renderer découpée (demande de Bruno, 2026-10-27) : `BeginFrame` (attente du GPU), `Render` (vue, anticrénelage, préparation des renderers, graphe), `EndFrame` (présentation), appelés à la suite après les mises à jour de la scène. Pas de fil de rendu séparé (comme CryEngine) tant que les mesures ne montrent pas le CPU limitant.
- ⬜ Bus de mixage dans `AudioSystem` (graphe de miniaudio), base de la table de mixage de l'éditeur.
- ⬜ Crédits et `LICENCES-TIERCES.txt` générés à côté du jeu (avant toute distribution).
- ⬜ Backend DX12.

## Dettes et limites connues
- Un processeur sans AVX2 s'arrête net au lancement, sans message (à ajouter avec la configuration de distribution).
- Les lumières sont parcourues pixel par pixel, sans tuiles (lent au-delà de quelques dizaines).
- Les reflets viennent seulement du ciel, pas de la scène.
- L'atlas de police est régénéré au démarrage et limité au Latin-1.
- Le mode Jouer copie la scène en JSON (lent sur une grosse scène).
- Pas de DPI : l'interface suppose une mise à l'échelle Windows de 100 %.
- Les textures du sol pèsent 45 Mo dans le dépôt (2K, 5 matières).
