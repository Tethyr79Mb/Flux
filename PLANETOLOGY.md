# Planetology : créer une planète et son relief

Ce document explique comment Flux fabrique une planète. Il couvre ce qu'on règle dans l'éditeur, ce que fait le
moteur, le détail des montagnes, puis la démarche à suivre pour améliorer le relief. Il complète `ARCHITECTURE.md`
(vue d'ensemble) et `Engine/ROADMAP.md` (ce qui est fait). Le code est dans `Engine/include/Flux/Planetology/` et
`Engine/src/Planetology/`, les shaders dans `Engine/shaders/PlanetTile.slang` (relief, arbres) et
`Engine/shaders/Mesh.slang` (sol, océan, nuages).

## 1. Créer une planète dans l'éditeur

Une planète est une entité qui porte un composant **Planète**. D'autres composants peuvent s'y ajouter sur la même
entité. **Placer > Espace > Planète** la pose à trois rayons devant la caméra.

| Composant | Rôle |
|---|---|
| Planète | forme (rayon, relief, montagnes, graine), sol, climat, mer, gravité, atmosphère |
| Végétation | arbres semés par le GPU autour de la caméra |
| Nuages | une couche de nuages qui dérive et fait de l'ombre |
| Orbite | place la planète autour de son étoile |

Ordre conseillé des réglages, du plus grand au plus fin :

1. **Rayon.** Il fixe la courbure vue de l'espace et la distance de l'horizon. 60 km fait une petite lune, 200 km est
   la taille des planètes de Star Citizen, 6 370 km celle de la Terre. Le coût de rendu change peu, grâce à l'arbre
   de tuiles.
2. **Largeur des montagnes.** C'est l'écart entre les grandes crêtes, en mètres (50 km par défaut). Elle ne dépend pas
   du rayon : une grande planète a plus de chaînes, pas des chaînes plus larges. Sur une planète trop petite pour
   cette largeur, les montagnes restent à la taille que permet le rayon.
3. **Relief.** C'est la hauteur des plus hauts sommets au-dessus du niveau zéro (et la profondeur des creux). **La
   raideur dépend du rapport relief / largeur.** Repères :
   - 2000 à 3000 m pour 50 km : montagnes franches, versants de 20 à 30° ;
   - 4500 m pour 20 km : aiguilles, à éviter.
4. **Graine.** Une autre graine donne une autre planète avec les mêmes réglages.
5. **Mer.** `Niveau de la mer` est en % du relief, 0 % étant le niveau zéro. On voit le fond là où l'eau est peu
   profonde.
6. **Climat.** La température baisse vers les pôles (axe y de la planète) et avec l'altitude (6,5 °C par km).
   L'humidité va du désert à la prairie. Ils choisissent la matière du sol (herbe, terre, sable, neige), la roche
   prenant les pentes. La neige commence à `Neige à partir de` (% du relief) : baisser le relief la fait descendre.
7. **Atmosphère.** Densité (1 = Terre), hauteur et couleur du ciel. Elle règle aussi la vitesse permise au vaisseau :
   le mode voyage est bridé à mesure que l'air s'épaissit.
8. **Nuages, puis végétation.** Les arbres ne poussent que là où pousserait l'herbe : pente douce, assez humide, ni
   plage, ni neige, ni gel.

## 2. Ce que fait le moteur

### La forme : un cube gonflé en sphère

Les six faces d'un cube sont projetées sur la sphère. La projection « tangente » égalise la taille des mailles (au
plus 20 % d'écart). Chaque face est un arbre de tuiles (quadtree) de 32 × 32 quads : une tuile proche de la caméra
est coupée en quatre enfants, jusqu'à environ 1,5 m entre deux sommets.

C'est du **CDLOD**. Chaque sommet glisse vers la forme de sa tuile parente à mesure qu'il s'éloigne, si bien que deux
niveaux de détail se fondent sans saut ni fente. Des jupes sous les bords cachent les petits écarts qui restent.

### Le relief : une formule, en deux exemplaires

Le relief est une formule : la hauteur dans une direction donnée. Rien n'est stocké, et le terrain n'est pas creusable
(décision de Bruno du 2026-10-07). La formule existe deux fois :

- `Terrain::GetHeight` (`Planet.cpp`), en double précision, sert à la collision. Elle tourne sur le CPU, tuile par
  tuile, autour du joueur et des vaisseaux.
- `Height` (`PlanetTile.slang`), en float, sert à l'affichage. Un compute shader calcule chaque tuile, un groupe de
  256 threads par tuile. Le CPU prépare en double ce qui en demande (`PrepareTileGpu`) : le point de départ de chaque
  octave au centre de la tuile. Le GPU garde ainsi le millimètre, même sur une planète de la taille de la Terre.

**Toute modification va dans les deux.** `PlanetGpuTests` vérifie qu'ils restent égaux, sur trois planètes : 60 km,
200 km et la Terre.

### Les couches de bruit

Chaque couche est une somme d'octaves de bruit de Perlin avec sa pente exacte. Chaque octave a une fréquence double
de la précédente et une amplitude plus faible (gain).

| Couche | Rôle | Octaves | Taille |
|---|---|---|---|
| Continents | terres et bassins | 0 à 4 | planète |
| Zone de montagnes | où passent les chaînes | 1 à 2 | largeur des montagnes |
| Zone de collines | où sont les collines | 2 à 4 | largeur des montagnes |
| Montagnes | crêtes | 2 à 9 | largeur des montagnes |
| Collines | bosses moyennes | 4 à 8 | largeur des montagnes |
| Détail | rochers, bosses fines (gain 0,58) | 6 à 21 | largeur des montagnes |
| Grande déformation | tord continents et zones | 0 à 2 | planète |
| Petite déformation | tord montagnes et collines | 3 à 5 | planète |

Une tuile ne calcule que les octaves plus grandes que deux fois l'écart de ses sommets (`Terrain::GetOctaves`). Les
plus fines ne se verraient pas et coûteraient cher. Une octave ne dépend que des plus grandes, si bien qu'une tuile
grossière garde la forme de ses enfants. Les couches « à la largeur des montagnes » ont une fréquence multipliée par
`Terrain::GetScale`. Elles perdent leurs octaves `GetScaleShift` niveaux plus tôt, sauf les zones, qui gardent toutes
les leurs : sans octave, une zone vaudrait « en pleine chaîne » partout.

### La formule, pas à pas (`Terrain::GetHeight`)

1. **Déformation.** Le point lu est poussé le long de la pente d'un bruit lent. Les côtes se découpent et les chaînes
   s'allongent et serpentent, au lieu de faire des taches rondes.
2. **Continents.** `land` vaut 0 en mer et 1 sur terre, avec une transition douce.
3. **Zones.** Les chaînes de montagnes sont des bandes là où un bruit lent passe par zéro (`1 - |bruit|` au-dessus de
   0,84), au cœur des continents. Les collines occupent une autre zone. Le reste est de la plaine.
4. **Montagnes et collines,** calculées seulement dans leur zone : une plaine ne paie pas les crêtes.
5. **Détail fin.** Presque rien en plaine (1,2 % du relief), plus en collines (3 %) et en montagne (6,5 %).
6. **Somme.** Bassins vers -0,4, plaines vers 0,05 à 0,1, sommets jusqu'à 1, le tout multiplié par le relief.

## 3. Les montagnes en détail

Une chaîne de montagnes est faite de trois choses :

- **Un dos continu** le long de la bande : 25 % de sa hauteur (`ChainBase`). Sans lui, les sommets seraient des pics
  isolés dans la plaine.
- **Des crêtes** : du bruit « ridged » `(1 - |bruit|)³`. Il fait une arête là où le bruit passe par zéro, et le cube
  élargit les vallées. L'arête est arrondie (`RidgeRounding`), sinon une arête vive au mètre près ferait un trait à
  l'ombrage.
- **L'érosion analytique** (méthode d'Inigo Quilez). Chaque octave est atténuée là où la couche est déjà en pente,
  par un facteur `1 / (1 + force × pente²)`, la pente étant mesurée en mètres par mètre. Les versants restent lisses
  et creusés de ravines, et le détail se garde pour les crêtes et les replats, comme un relief usé par l'eau.

Les réglages sont des constantes en tête de `Planet.cpp`, recopiées dans `PlanetTile.slang` :

| Constante | Effet |
|---|---|
| `ChainStart`, `ChainEnd` | largeur des bandes de chaînes (plus bas : chaînes plus larges) |
| `ChainBase` | hauteur du dos de la chaîne (trop haut : un plateau) |
| `RidgeContrast` | hauteur des crêtes au-dessus du dos |
| `MountainWeight`, `HillWeight` | part du relief des montagnes et des collines |
| `ErosionMountains`, `ErosionHills`, `ErosionDetail` | force de l'érosion (plus haut : versants plus lisses) |
| `DetailMountains` | rugosité des sommets |
| `WarpLarge`, `WarpSmall` | force de la déformation |

Défauts déjà rencontrés :

- **Un mur avec des pics** (2026-10-19). Les zones restaient à l'échelle de la planète alors que les montagnes
  rapetissaient : chaque chaîne devenait un plateau de 1500 m aux bords raides, couvert de petits pics. Corrigé en
  mettant les zones à la largeur des montagnes.
- **Des aiguilles.** Le rapport relief / largeur était trop grand. Il faut baisser le relief ou élargir les montagnes.

## 4. Démarche pour améliorer le relief

Le relief se juge à l'œil, mais l'œil se trompe vite. On avance donc toujours ainsi :

1. **Des références.** Avant de coder, on choisit deux ou trois images de ce qu'on vise : Star Citizen, les Alpes vues
   d'avion, un désert de mesas. Elles vont dans `/mnt/project-files/Recherche/`.
2. **Des mesures.** On mesure l'existant avec un petit programme qui appelle `Terrain::GetHeight` :
   - un profil de hauteur sur 80 km à travers une chaîne ;
   - l'écart moyen entre deux sommets ;
   - la répartition des pentes ;
   - la part de plaines où un vaisseau peut se poser (par exemple, pente sous 10°).
   C'est ainsi qu'on a trouvé le mur : le profil ne descendait jamais sous 1200 m entre deux sommets de 2000 m.
3. **Une étape à la fois,** une PR par étape. Chaque PR :
   - change la formule dans les deux exemplaires ;
   - garde `PlanetGpuTests` vert ;
   - montre des rendus avant/après (orbite, montagne, plaine) ;
   - refait les mesures ;
   - mesure en Release (panneau Moteur : temps GPU des tuiles, temps CPU de la collision).
4. **Les règles qui restent vraies :**
   - le relief reste une formule, sans données stockées ;
   - chaque bruit ajouté coûte deux fois, sur le GPU pour l'affichage et sur le CPU pour la collision ;
   - les plaines doivent rester assez plates pour se poser et marcher.

## 5. Étapes du relief

Le plan d'origine est dans `/mnt/project-files/Recherche/relief-planetes-plan.md`.

- ✅ 1. Bruit avec sa pente exacte (fondation des étapes suivantes).
- ✅ 2. Érosion analytique.
- ✅ 3. Déformation du domaine : côtes découpées, chaînes allongées.
- 🔧 Largeur des montagnes en mètres, indépendante du rayon. Limites : quelques crêtes un peu plates, et les tuiles
  lointaines ne sont pas plus fines alors que le terrain est plus raide.
- ⬜ 4. Crêtes de vraies montagnes (« ridged multifractal » de Musgrave) : sommets nets et contreforts, pieds de
  montagne doux, moins réguliers qu'aujourd'hui.
- ⬜ 5. Plateaux, terrasses et falaises (mesas, canyons). Une falaise restera une pente raide, pas un mur vertical.
- ⬜ 6. Vallées et rivières par formule. Crédibles de loin, sans garantir que l'eau descende toujours.
- ⬜ 7. Cratères, pour les lunes et les planètes sans air.
- ⬜ 8. Types de planète (tellurique, lune, désert, glace, volcanique, océanique) qui règlent le poids de chaque couche.
- ⬜ 9. (À décider par Bruno) Érosion hydraulique précalculée. C'est la plus réaliste, mais elle stocke une carte par
  planète, ce qui va contre la décision du 2026-10-07.
- ⬜ 10. Rochers et cailloux posés sur le sol en instancing, plutôt que des octaves toujours plus fines.
