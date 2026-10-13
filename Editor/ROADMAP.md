# Feuille de route de l'éditeur (FluxEditor)

Éditeur de scènes et d'interfaces d'**Ars Solaris**, construit avec FluxUI (pas d'ImGui). Ce fichier dit où on en est
et ce qui vient ensuite. L'ordre des sections « Prochaines étapes » est une proposition : Bruno décide.

Légende : ✅ fait · 🔨 en cours (PR ouverte) · ⬜ à faire

## Où on en est

### Fenêtre et panneaux
- ✅ Panneaux ancrés en onglets avec glisser-déposer, panneaux flottants redimensionnables, disposition enregistrée.
- ✅ Thèmes : Sombre (gris neutres et accent bleu, façon Unreal / Unity), Clair, Graphite, Ars Solaris.
- ✅ Barre de menus (Fichier, Édition…), dialogues Windows natifs, étoile dans le titre si la scène est modifiée.
- 🔨 Barre d'outils : nouvelle scène, ouvrir, enregistrer, annuler / rétablir, Jouer et Pause au milieu, Paramètres à droite.
- ✅ Paramètres : graphiques (GPU, V-Sync), interface, son, touches.
- ✅ Console.
- ✅ Version de l'éditeur dans le titre.

### Scène
- ✅ Onglet « Scènes » : scènes en vignettes rangées en dossiers, objets regroupés par type avec icônes, glisser-déposer pour l'ordre et la hiérarchie.
- ✅ Inspecteur généré depuis les composants (listes déroulantes, couleurs avec sélecteur Windows, choix de fichiers).
<<<<<<< HEAD
- ✅ Panneau « Placer » : cube, personnage, vaisseaux (maquette, Chasseur), lumières, ciel, soleil, planète, son, interface…
=======
- ✅ Panneau « Placer » : cube, personnage, vaisseaux (maquette, Chasseur), lumières, ciel, planète, son, interface…
- 🔨 « Placer » en liste à icônes par catégories repliables, avec recherche ; modes du gizmo en pictogrammes dans la vue ; chemin cliquable dans Contenu.
>>>>>>> origin/master
- ✅ Vue 3D : sélection au clic (un vaisseau se sélectionne en entier), contour, gizmo déplacer / tourner / échelle (W, E, R ; Ctrl = pas de 15°).
- ✅ Annuler / refaire (Ctrl+Z, Ctrl+Y).
- ✅ Mode Jouer (F5), pause (F6), scène en lecture seule pendant le jeu.
- ✅ Scène de démarrage du projet.

### Contenu (assets)
- ✅ Explorateur de fichiers : arbre de dossiers, vignettes avec icônes Windows, nouveau dossier, renommer, déplacer (les références des scènes suivent), corbeille.
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
- ⬜ Aller à l'objet sélectionné (touche F) depuis la vue.

### Contenu
- ⬜ Aperçu des images et des modèles dans les vignettes.
- ⬜ Accepter les `.gltf` (pas seulement les `.glb`).
- ⬜ Annuler un déplacement de fichier qui remet aussi les anciens chemins dans les scènes.

### Prefabs
- ✅ Créer un prefab depuis l'inspecteur, le placer depuis « Placer », l'ouvrir pour le modifier (double-clic dans
  Contenu) ; champs modifiés marqués, « Revenir au prefab », « Délier ».
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
