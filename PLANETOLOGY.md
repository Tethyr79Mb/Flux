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
2. **Largeur des montagnes.** Elle fixe l'écart entre deux grands sommets voisins, environ un sixième de la largeur (8 km
   pour 50 km par défaut). Elle ne dépend pas
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

Une tuile qui manque est calculée par le GPU au début de l'image suivante. En attendant, le quart de sa parente qu'elle
couvre est dessiné à sa place, ce que permettent les indices rangés par quart (`BuildTileIndices`). Avant, c'était toute
la parente : quand la caméra tournait, une tuile au loin qui entrait dans le champ remettait pour une image du sol
grossier jusque sous les pieds, sans arbres, et le sol clignotait.

Une tuile cachée par la courbure n'est ni dessinée ni découpée. Le test prend l'horizon du sol le plus bas possible et
le point le plus haut de cette tuile, et non le plus haut sommet de la planète : près du sol, une plaine au loin est
écartée bien avant une montagne. Le sol est aussi découpé pour là où la caméra sera dans 2 secondes (sa vitesse dans le
repère de la planète, lissée). À pied, cela ne change rien. En vaisseau au ras du sol, les tuiles sont prêtes quand il
arrive. En attendant, elles sont dessinées à la forme de leur parente (glissement CDLOD), donc sans différence visible.
Avec un océan, caméra au-dessus de la mer, l'horizon est celui de la mer : rien n'est plus bas que sa surface. L'eau,
le fond et les terres basses qui passent dessous ne sont plus gardés. Le fond vu au travers de l'eau ne le serait qu'en
rasant la surface, où elle reflète tout (fresnel de `fragmentOcean`).

### Le relief : une formule, en deux exemplaires

Le relief est une formule : la hauteur dans une direction donnée. Rien n'est stocké, et le terrain n'est pas creusable
(décision de Bruno du 2026-10-07). La formule existe deux fois :

- `Terrain::GetHeight` (`Planet.cpp`), en double précision, sert à la collision. Elle tourne sur le CPU, tuile par
  tuile, autour du joueur et des vaisseaux.
- `Height` (`PlanetTile.slang`), en float, sert à l'affichage. Un compute shader calcule chaque tuile, un groupe de
  256 threads par tuile. Le CPU prépare en double ce qui en demande (`PrepareTileGpu`) : le point de départ de chaque
  octave au centre de la tuile. Le GPU garde ainsi le millimètre, même sur une planète de la taille de la Terre.
- Sauf la déformation (`Warp`) : 0,06 radian, multiplié par la fréquence des sommets (environ 6 000 sur la Terre), il
  lui faut 1e-10 radian, hors de portée du float (1 à 2,5 m d'écart avec la collision dans les montagnes). Quand le
  GPU a `shaderFloat64` (GTX 1060, RX 5700 et mieux), `PlanetTile64.slang` compile le même fichier avec la déformation,
  l'écart au centre et les points du bruit en double (`Precise`) ; `PrepareTileGpu` envoie le reste des floats du
  centre et des octaves (`centerLow`, `fractionsLow`). Le bruit lui-même reste en float, sauf celui de la déformation.
  Sans `shaderFloat64`, tout reste en float et l'écart revient.

**Toute modification va dans les deux.** `PlanetGpuTests` vérifie qu'ils restent égaux, sur trois planètes : 60 km,
200 km et la Terre.

### Les couches de bruit

Chaque couche est une somme d'octaves de bruit de Perlin avec sa pente exacte. Chaque octave a une fréquence double
de la précédente et une amplitude plus faible (gain).

| Couche | Rôle | Octaves | Taille |
|---|---|---|---|
| Continents | terres et bassins | 0 à 4 | planète |
| Zone de montagnes | où passent les chaînes | 1 à 2 | planète |
| Zone de collines | où sont les collines | 2 à 4 | planète |
| Sommets | grands sommets, puis sommets secondaires (deux réseaux) | 2 à 3 | largeur des montagnes |
| Collines | bosses moyennes | 4 à 8 | largeur des montagnes |
| Détail | rochers, bosses fines (gain 0,58) | 6 à 21 | planète |
| Grande déformation | tord continents et zones | 0 à 2 | planète |
| Petite déformation | tord les collines | 3 à 5 | planète |
| Ravines | sillons dans le sens de la pente, en arbre | 3 à 6 | largeur des montagnes |

Une tuile ne calcule que les octaves plus grandes que deux fois l'écart de ses sommets (`Terrain::GetOctaves`). Les
plus fines ne se verraient pas et coûteraient cher. Une octave ne dépend que des plus grandes, si bien qu'une tuile
grossière garde la forme de ses enfants. Les couches « à la largeur des montagnes » (sommets, ravines et collines) ont une fréquence multipliée par
`Terrain::GetScale` et perdent leurs octaves `GetScaleShift` niveaux plus tôt. Les zones restent à l'échelle de la
planète : chaque chaîne est large et porte de nombreux sommets.

### La formule, pas à pas (`Terrain::GetHeight`)

1. **Déformation.** Le point lu est poussé le long de la pente d'un bruit lent. Les côtes se découpent et les chaînes
   s'allongent et serpentent, au lieu de faire des taches rondes.
2. **Continents.** `land` vaut 0 en mer et 1 sur terre, avec une transition douce.
3. **Zones.** Les chaînes de montagnes sont des bandes là où un bruit lent passe par zéro (`1 - |bruit|` au-dessus de
   0,75, pleine au-delà de 0,95), au cœur des continents. Les collines occupent une autre zone. Le reste est de la plaine.
4. **Montagnes et collines,** calculées seulement dans leur zone : une plaine ne paie ni les sommets ni les ravines.
5. **Détail fin.** Presque rien en plaine (1,2 % du relief), plus en collines (3 %) et en montagne (6,5 %).
6. **Somme.** Bassins vers -0,4, plaines vers 0,05 à 0,1, sommets jusqu'à 1, le tout multiplié par le relief.

## 3. Les montagnes en détail

Une chaîne de montagnes est faite de trois choses :

- **Un dos continu** le long de la bande : 20 % de sa hauteur (`ChainBase`). Les vallées entre les sommets sont
  donc des vallées d'altitude, et les sommets ne sortent pas d'une plaine.
- **Des sommets** (`Terrain::Peaks`). Une montagne est un pic à une certaine hauteur, avec des versants tout autour,
  de pentes différentes (définition de Bruno). Chaque maille d'un réseau 3D (environ largeur des montagnes / 2π)
  porte au plus un sommet (`PeakChance`), décalé au hasard. Seules comptent les mailles dont le sommet tombe près de la
  sphère (`PeakSlabInner`, `PeakSlabOuter`), pour que les sommets soient aussi serrés partout. Chaque sommet tire sa
  hauteur (`PeakLowest` à 1) et le rayon de son pied (`PeakRadius`). Le pied s'éloigne ou se rapproche selon la
  direction : un côté plus doux que l'autre (`PeakAsymmetry`), une forme allongée (`PeakElongation`) et quatre
  contreforts, des arêtes qui descendent du sommet (`PeakSpurs`, `PeakSpurSharpness`). Le profil d'un versant,
  concave en bas et à la pointe arrondie (`PeakShape`, `PeakTop`, `PeakTip`), va du sommet au pied. Les sommets voisins
  s'unissent par un maximum doux (`PeakBlend`) : un col arrondi entre eux. Un second réseau, deux fois plus fin, ajoute
  des sommets secondaires plus bas (`PeakSecondary`). La pente des sommets est calculée exactement, pour les ravines.
  Le pied reste à moins d'une maille : les 27 mailles autour du point suffisent.
- **Des ravines** (`Terrain::Gullies`, bruit d'érosion de Clay John et Fewes). Dans chaque cellule d'un réseau 3D, une
  onde `cos` varie le long de la perpendiculaire à la pente : elle fait des sillons dans le sens de la pente. Les 27
  cellules voisines se mélangent par un poids gaussien autour d'un point décalé au hasard. Chaque octave suit la pente
  totale, celle des sommets plus celle des ravines déjà posées (`GullyBranch`) : les petites ravines partent des
  grandes, en arbre. Rien sous 4,5° de pente (`GullyStartSlope`), plein vers 19° (`GullyFullSlope`) : les plaines
  restent lisses, et les sommets et cols, où la pente n'a pas de direction, ne tournent pas en tourbillons.

L'érosion analytique (méthode d'Inigo Quilez) reste sur les collines et le détail. Chaque octave est atténuée là où
la couche est déjà en pente, par un facteur `1 / (1 + force × pente²)`, la pente étant mesurée en mètres par mètre.

Les réglages sont des constantes en tête de `Planet.cpp`, recopiées dans `PlanetTile.slang` :

| Constante | Effet |
|---|---|
| `ChainStart`, `ChainEnd` | largeur des bandes de chaînes (plus bas : chaînes plus larges) |
| `ChainBase` | hauteur du dos de la chaîne (trop haut : un plateau) |
| `PeakHeight`, `PeakLowest`, `PeakSecondary` | hauteur des sommets au-dessus du dos, du plus bas, des secondaires |
| `PeakChance`, `PeakRadius` | nombre de sommets, largeur de leur pied (plus large : versants plus doux) |
| `PeakAsymmetry`, `PeakElongation`, `PeakSpurs` | différences de pente autour d'un sommet, contreforts |
| `PeakShape`, `PeakTop`, `PeakTip` | profil d'un versant (concavité, sommet arrondi ou pointu) |
| `GullyAmplitude`, `GullyGain` | profondeur des ravines (première octave, puis d'une octave à la suivante) |
| `GullyStartSlope`, `GullyFullSlope` | pentes où les ravines commencent et sont pleines |
| `GullyBranch` | ramification des ravines (plus haut : plus de branches, mais plus de bruit) |
| `MountainWeight`, `HillWeight` | part du relief des montagnes et des collines |
| `ErosionHills`, `ErosionDetail` | force de l'érosion (plus haut : versants plus lisses) |
| `DetailMountains` | rugosité des sommets |
| `WarpLarge`, `WarpSmall` | force de la déformation |

Défauts déjà rencontrés :

- **Un mur avec des pics** (2026-10-19). Les zones restaient à l'échelle de la planète alors que les montagnes
  rapetissaient : chaque chaîne devenait un plateau de 1500 m aux bords raides, couvert de petits pics. Corrigé en
  mettant les zones à la largeur des montagnes.
- **Des racines d'arbre vues d'orbite** (2026-10-19, juste après). Les zones à la largeur des montagnes faisaient de
  minces bandes sinueuses. Corrigé en remettant les zones à l'échelle de la planète, mais plus larges et aux bords plus
  doux (`ChainStart` 0,75, `ChainEnd` 0,95), avec un dos plus bas (`ChainBase` 0,12) pour éviter le plateau.
- **Des plaines où l'on ne peut pas se poser** (2026-10-19). Le détail fin suivait aussi la largeur des montagnes :
  plus fin à la même hauteur, il était quatre fois plus raide, et aucune plaine ne restait sous 5° de pente. Le détail
  est revenu à l'échelle de la planète ; `PlanetTests` vérifie qu'une terre sur deux reste une plaine.
- **Des racines au lieu de montagnes** (2026-10-19, vu par Bruno au sol et d'orbite). Les crêtes `1 - |bruit|`
  suivent les lignes où le bruit de Perlin passe par zéro : des boucles fines, toutes de la même hauteur, avec du plat
  entre elles. Ni le « ridged multifractal » de Musgrave ni d'autres puissances n'y changeaient rien. Remplacées par
  des massifs (bruit au carré) et des ravines.
- **Des sillons parallèles** (en réglant les ravines). Une maille du réseau fait la longueur d'onde divisée par 2π :
  des ravines commencées à l'octave 5 faisaient 1 km au lieu de 6, en peigne. Elles commencent à l'octave 3.
- **Un réseau de crêtes reliées** (2026-10-19, vu par Bruno sur la version massifs et ravines). Les massifs, faits
  d'un bruit, n'avaient ni vrais sommets ni vallées : des crêtes reliées entre elles dans tous les sens. Remplacés par
  des sommets placés un par un (`Terrain::Peaks`), comparés en rendu Blender à l'image de Star Citizen de Bruno.
- **Des marches entre CPU et GPU** (en portant les sommets). Le plafond dur de l'allongement du pied et la pointe du
  sommet cassaient la pente : les ravines qui la suivent sautaient d'un côté à l'autre, et le float du GPU ne tombait
  pas du même côté que le double du CPU. Minimum doux (`PeakStretchSoftness`) et pointe arrondie (`PeakTip`).
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
- 🔧 Largeur des montagnes en mètres, indépendante du rayon. Limites : quelques sommets un peu plats, et les tuiles
  lointaines ne sont pas plus fines alors que le terrain est plus raide.
- 🔧 4. Vraies montagnes : sommets placés un par un, aux versants de pentes différentes et aux contreforts, vallées
  d'altitude entre eux, ravines en arbre (bruit d'érosion). Limites : contreforts encore réguliers (quatre par
  sommet, en étoile vus de dessus) ; pas de rochers posés sur les versants ; le relief d'un point coûte environ 4 fois
  plus sur le CPU en montagne qu'avant le bruit d'érosion (collision des tuiles proches).
- ⬜ 5. Plateaux, terrasses et falaises (mesas, canyons). Une falaise restera une pente raide, pas un mur vertical.
- ⬜ 6. Vallées et rivières par formule. Crédibles de loin, sans garantir que l'eau descende toujours.
- ⬜ 7. Cratères, pour les lunes et les planètes sans air.
- ⬜ 8. Types de planète (tellurique, lune, désert, glace, volcanique, océanique) qui règlent le poids de chaque couche.
- ⬜ 9. (À décider par Bruno) Érosion hydraulique précalculée. C'est la plus réaliste, mais elle stocke une carte par
  planète, ce qui va contre la décision du 2026-10-07.
- ⬜ 10. Rochers et cailloux posés sur le sol en instancing, plutôt que des octaves toujours plus fines.
