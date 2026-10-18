# Feuille de route de l'éditeur (FluxEditor)

Éditeur de scènes et d'interfaces d'**Ars Solaris**, construit avec FluxUI (pas d'ImGui). Ce fichier dit où on en est
et ce qui vient ensuite. L'ordre des sections « Prochaines étapes » est une proposition : Bruno décide.

Légende : ✅ atteint · 🔧 à améliorer (fait, avec des limites connues, dites sur la ligne) · 🔨 commencé (en cours ou PR ouverte) · ⬜ à faire

## Où on en est

### Fenêtre et panneaux
- ✅ Panneaux ancrés en onglets avec glisser-déposer, panneaux flottants redimensionnables, disposition enregistrée.
- ✅ Thèmes : Sombre (gris neutres et accent bleu, façon Unreal / Unity), Clair, Graphite, Ars Solaris.
- ✅ Barre de menus (Fichier, Édition…), dialogues Windows natifs, étoile dans le titre si la scène est modifiée.
- ✅ Barre d'outils : nouvelle scène, ouvrir, enregistrer, annuler / rétablir, Jouer et Pause au milieu, Paramètres à droite.
- ✅ Paramètres : graphiques (GPU, version de Vulkan, V-Sync, anticrénelage, bloom et son intensité), interface, son, touches.
- ✅ Console.
- ✅ Version de l'éditeur dans le titre.

### Scène
- ✅ Onglet « Scènes » : scènes en vignettes rangées en dossiers, objets regroupés par type avec icônes, glisser-déposer pour l'ordre et la hiérarchie.
- ✅ Inspecteur généré depuis les composants (listes déroulantes, couleurs avec sélecteur Windows, choix de fichiers).
- ✅ Barre d'outils « Texte » : poser un texte à l'écran, choisir sa police (dossier `Polices/` du projet, « Ajouter
  une police... » la copie), sa taille et sa couleur ; sur le texte sélectionné (annulable), sinon pour le prochain.
- ✅ Panneau « Placer » : cube, personnage, vaisseaux (maquette, Chasseur), station (maquette, posée à l'horizontale, entrée face à la caméra), cible (cube à points de vie), lumières, ciel, soleil, planète, son, interface…
- ✅ « Placer » en liste à icônes par catégories repliables, avec recherche ; modes du gizmo en pictogrammes dans la vue ; chemin cliquable dans Contenu.
- ✅ Bouton « Monde » de la barre de menus : fenêtre des réglages du monde de la scène (gravité, air, chute max), annulables ; l'objet Monde qui les garde n'est plus montré dans la scène ni dans Placer.
- ✅ Touche F : aller voir l'objet sélectionné (une planète ou un soleil à des millions de km). Soleil posé au centre de la scène ; planète posée en orbite quand la scène a un Soleil (la caméra va la voir), avec un liseré bleuté.
- 🔧 Vue 3D : sélection au clic (un vaisseau se sélectionne en entier), contour, gizmo déplacer / tourner / échelle (W, E, R ; Ctrl = pas de 15°). À améliorer : un seul objet sélectionné à la fois, gizmo dans les axes du monde.
- 🔧 Annuler / refaire (Ctrl+Z, Ctrl+Y). À améliorer : un objet à la fois, l'étoile du titre reste après une annulation.
- ✅ Aimant des armes : une arme lâchée sur un emport dans l'onglet Scènes, ou déplacée au gizmo à moins de 1 m de
  lui, s'y fixe (enfant, à sa place exacte) ; refus expliqué si l'emport ne l'accepte pas (genre, taille, déjà occupé).
- 🔧 Mode Jouer (F5), pause (F6), scène en lecture seule pendant le jeu. À améliorer : la scène est copiée en JSON, lent sur une grosse scène.
- ✅ Scène de démarrage du projet.

### Contenu (assets)
- 🔧 Explorateur de fichiers : arbre de dossiers, vignettes avec icônes Windows, nouveau dossier, renommer, déplacer (les références des scènes suivent), corbeille. À améliorer : pas d'aperçu des images et des modèles, `.gltf` refusé.
- ✅ Copie des assets à côté de l'exe, rechargement à chaud des images, modèles et textures du sol.

### Interfaces du jeu
- ✅ Éditeur de `.fluxui` : écran à la résolution choisie, poignées, ancres, propriétés, fenêtres, boutons, liaisons, onglets, zones de défilement.

## Prochaines étapes

### Confort d'édition
- ⬜ Dupliquer (Ctrl+D), copier / coller des objets de la scène.
- ⬜ Sélection multiple (clic + Maj, rectangle) ; l'annulation devra suivre plusieurs objets.
- ⬜ Gizmo dans les axes de l'objet (aujourd'hui : déplacement et rotation dans les axes du monde).
- ⬜ Magnétisme : grille de déplacement, pas d'échelle.
- ⬜ Glisser un modèle ou un son du Contenu directement dans la vue 3D.

### Contenu
- ⬜ Aperçu des images et des modèles dans les vignettes.
- ⬜ Accepter les `.gltf` (pas seulement les `.glb`).
- ⬜ Annuler un déplacement de fichier qui remet aussi les anciens chemins dans les scènes.

### Prefabs
- 🔧 Créer un prefab depuis l'inspecteur, le placer depuis « Placer », l'ouvrir pour le modifier (double-clic dans
  Contenu) ; champs modifiés marqués, « Revenir au prefab », « Délier ». À améliorer : pas encore « Appliquer au prefab ».
- ⬜ « Appliquer au prefab » : envoyer les modifications d'un exemplaire dans le prefab sans l'ouvrir.
- ⬜ Glisser un prefab du Contenu dans la vue.
- ⬜ Remplacer les vaisseaux codés en dur dans « Placer » par des prefabs.

## Plus tard
- ⬜ Mise à l'échelle DPI (écrans 125 %, 150 %).
- ⬜ Éditeur de matériaux.
- ⬜ Éditeur de terrain des planètes (réglages en direct avec aperçu).
- ⬜ Profileur dans l'éditeur (historique des temps CPU / GPU).

## Dettes et limites connues
- Le mode Jouer copie toute la scène en JSON : lent sur une grosse scène.
- L'étoile du titre reste après une annulation jusqu'à l'enregistrement.
- « Placer » code en dur certains chemins (Modeles/Chasseur).
