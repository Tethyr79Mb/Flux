# Scénario : la trame d'Ars Solaris

Ce document fixe l'univers, les factions et la structure de l'histoire, puis ce qu'ils demandent au moteur (graphe de
missions, multijoueur). Il complète `ARCHITECTURE.md` (technique) et `Engine/ROADMAP.md` (avancement). Les noms sont
provisoires tant que Bruno ne les a pas validés.

## 1. Les choix de Bruno (2026-11-18)

- Piste retenue : « L'Héritage solaire » (frontière, mystère d'une espèce inconnue).
- Quatre factions jouables, choisies à la création du personnage, comme dans EVE Online. Pas de PvP.
- Des factions dirigées par l'IA, dont une espèce inconnue.
- Jeu scénarisé jusqu'au niveau maximum, façon World of Warcraft ou Star Wars: The Old Republic, puis monde ouvert.
- Une histoire commune aux quatre factions, avec des variantes par faction (confirmé par Bruno).
- Les joueurs des quatre factions peuvent jouer en groupe ensemble.
- Un seul système solaire au lancement. Inspiration principale : EVE Online.
- Ton (choisi par Claude, à confirmer) : sérieux et réaliste, comme The Expanse, avec une part de mystère. Pas d'humour
  parodique, pas de magie : l'étrange vient de l'espèce inconnue et reste explicable à la fin.

## 2. L'univers

**Époque.** An 2740. L'humanité occupe le système solaire et une poignée de colonies, reliées par les **Portes** :
des anneaux trouvés dans l'espace au XXIIIe siècle, que personne ne sait construire. On sait seulement les allumer.

**L'événement de départ.** La Porte de la Faille, éteinte depuis toujours, s'allume seule et ouvre sur un système
inconnu : **Solaris**, autour de l'étoile **Héliade**. On y trouve des planètes habitables, des ruines, et autour de
l'étoile des structures gigantesques qui captent son énergie. Les savants les appellent l'*Ars Solaris*, « l'art du
soleil » : le nom du jeu.

**Pourquoi pas de PvP.** Les quatre puissances ont signé le **Pacte de Solaris** : partage du système, neutralité
armée, interdiction de combattre entre signataires. Le Pacte tient parce que la Porte s'est déjà fermée une fois,
pendant une escarmouche, et a laissé une flotte coupée de la Terre. Personne ne veut recommencer. Les rivalités passent
par l'économie, la course aux découvertes et les coups bas des factions IA.

## 3. Les factions jouables

| Faction | Qui | Ce qu'elle veut à Solaris | Départ |
|---|---|---|---|
| **Concorde terrienne** | gouvernement de la Terre, armée, administration | garder le contrôle de la Porte et l'ordre | station militaire près de la Porte |
| **Consortium Halcyon** | alliance de corporations, industrie, commerce | exploiter les ressources et la technologie des ruines | station industrielle en orbite d'une géante |
| **Libres Colonies** | colons indépendants, prospecteurs, anciens réfugiés | une terre à eux, loin de la Terre et des corporations | colonie au sol sur une planète tempérée |
| **Ordre de l'Aube** | savants et explorateurs, presque une confrérie | comprendre les bâtisseurs avant que les autres ne pillent tout | base de recherche sur une lune, près des ruines |

Chaque faction a sa ville de départ, ses vaisseaux, son style (couleurs, architecture) et son regard sur le mystère.

## 4. Les factions de l'IA

- **Les Bâtisseurs** (nom humain, provisoire) : l'espèce inconnue. Absente au début, connue par ses ruines, ses
  signaux et ses machines. Sa vérité se dévoile au niveau maximum.
- **Les Sentinelles** : machines des Bâtisseurs, encore actives, qui défendent les ruines et les structures solaires.
  Premier adversaire « étranger » du joueur.
- **Les Écorcheurs** : pirates et pilleurs d'épaves, nés de la flotte coupée de la Terre lors de la fermeture.
- **Le Cartel de Vesper** : contrebande, trafic d'artefacts ; il travaille en secret pour des membres des quatre
  factions et sert de levier pour les trahisons de l'histoire.

## 5. Le système Solaris (à caler sur les planètes du jeu)

Un seul système, découpé en zones de niveau comme les continents de WoW. Les planètes et les lunes sont fixes.

| Lieu | Rôle | Niveaux |
|---|---|---|
| La Porte de la Faille et ses stations | départs, ville commune neutre | 1-10 |
| Planète tempérée (forêts, colonies) | premières ruines au sol | 10-20 |
| Géante gazeuse et ses lunes | industrie, pirates, épaves | 20-30 |
| Planète désertique | grandes ruines, Sentinelles | 30-40 |
| Monde glacé lointain | signaux des Bâtisseurs | 40-50 |
| Orbite basse d'Héliade (structures solaires) | fin de l'histoire, monde ouvert | niveau max |

## 6. La structure de l'histoire

1. **Introduction de faction (niveaux 1-10)** : quatre débuts différents, dans la ville de la faction. Le joueur
   apprend à piloter, à marcher, à combattre, et arrive dans Solaris.
2. **Trame principale (10 à max)** : une histoire commune en chapitres, un par zone, avec des variantes par faction
   (qui donne la mission, ce qu'on y gagne, le regard sur l'événement). Fil rouge : comprendre pourquoi la Porte s'est
   rouverte, et ce que veulent les Bâtisseurs.
3. **Fin de l'histoire (niveau max)** : la révélation sur les Bâtisseurs et l'Ars Solaris, et le monde ouvert s'ouvre :
   les structures solaires, des missions répétables, la réputation, les événements de serveur et le commerce.

## 7. La progression (Bruno, 2026-11-18)

- **Le niveau** (expérience des missions) débloque du contenu : classes de vaisseaux, zones, chapitres, équipements.
  Il ne change jamais les statistiques du vaisseau.
- **Le livre de compétences**, comme dans EVE, mais qui progresse à l'usage et non avec le temps : piloter améliore
  le pilotage, miner le minage, tirer la précision. Ce sont les compétences qui rendent le joueur meilleur.
- Seules les actions utiles font progresser (validé par Bruno) : vraie cible, vraie cargaison, vraie mission.
- Un débutant peut donc voler avec un ancien : l'écart vient des compétences acquises, pas du niveau.

## 8. Avis franc et risques

- **Coût du contenu.** SWTOR a coûté des centaines de millions pour huit histoires complètes et doublées. Quatre
  histoires entières sont hors de portée : d'où une trame commune avec des variantes de faction et quatre
  introductions distinctes. C'est la plus grosse économie possible sans perdre le choix de faction.
- **Compétences à l'usage.** Le risque connu (Ultima Online, Skyrim) : répéter une action sans but pour monter,
  avec des scripts. Parades : seules les actions utiles comptent (vraie cible, vraie cargaison), gains décroissants
  à force de répéter, calcul sur le serveur.
- **Le Pacte doit vivre en jeu.** Sans PvP, il faut que la rivalité se sente : classement des factions, course aux
  découvertes, zones dont le contrôle change selon les missions réussies par chaque faction.

## 9. Ce que la trame demande au moteur

**Graphe de missions** (prévu après le multijoueur, `.fluxmission`) : nœuds de dialogue avec PNJ, conditions de
niveau, de faction et de réputation, chapitres enchaînés, variantes par faction dans un même graphe, récompenses
(expérience, réputation, licences, objets). Le gain de compétences, lui, vient des actions et non des missions.

**Multijoueur** :
- Progression par personnage (niveau, compétences, chapitres faits, réputation) dans PostgreSQL ; les compétences
  sont calculées par le serveur, jamais par le client.
- État du monde à deux niveaux : **global** (le même pour tous : événements de serveur, contrôle des zones) et
  **personnel** (l'avancée de son histoire). Ce qui dépend de l'histoire personnelle passe par des **phases** :
  le serveur ne montre à un joueur que les entités de son étape (un PNJ mort pour lui, vivant pour un autre).
- Moments forts de l'histoire en **instances** (zone Instance déjà prévue), seul ou en groupe.
- Groupes : un joueur avance son histoire quand le groupe est à la même étape ; les autres l'aident et gagnent
  expérience et réputation.

## 10. Questions ouvertes

- Niveau maximum : 50 proposé.
- Les vraies intentions des Bâtisseurs (la fin de la trame) : à écrire ensemble, en dernier.
- Noms définitifs des factions, du système et des lieux.
