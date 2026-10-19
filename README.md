<p align="center">
  <img src="Engine/assets/logo/FluxEngine.png" alt="FluxEngine" width="440">
</p>

# Flux

Moteur de jeu 3D spatial (**FluxEngine**), son éditeur (**FluxEditor**) et **Ars Solaris**, le jeu construit avec eux :
piloter un vaisseau depuis le cockpit, se poser sur des planètes sans chargement, marcher à bord et en sortir.

- Rendu Vulkan derrière une couche RHI, shaders Slang, C++23.
- Mondes géants : secteurs de 1 million de km et positions en double, rendu relatif à la caméra.
- Planètes entières calculées par le GPU (relief, sol, océan, nuages, arbres), physique Jolt (un monde par vaisseau).
- Interface maison FluxUI, partagée par l'éditeur et le jeu.

## Compiler

Windows avec Visual Studio (MSVC) et le SDK Vulkan (qui fournit `slangc`). Ouvrir le dossier dans Visual Studio,
choisir le préréglage `msvc-debug` ou `msvc-release`, puis Générer. Les dépendances se téléchargent seules.

## Pour aller plus loin

- `ARCHITECTURE.md` : vue technique d'ensemble.
- `Engine/ROADMAP.md` et `Editor/ROADMAP.md` : ce qui est fait et ce qui vient.
