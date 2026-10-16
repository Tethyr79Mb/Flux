// Lance tous les groupes de tests ; code de sortie 0 si tout passe (lancer FluxTests ou ctest).
#include "Check.hpp"

int main() {
    struct Group {
        const char* name;
        bool (*run)();
    };
    constexpr Group groups[] = {
        {"scène", RunSceneTests},
        {"sauvegarde de scène", RunSceneSerializerTests},
        {"prefabs", RunPrefabTests},
        {"interface (FluxUI) et rotations", RunUiTests},
        {"projet", RunProjectTests},
        {"réglages du jeu", RunGameSettingsTests},
        {"sélection dans la vue 3D", RunPickingTests},
        {"images et textures", RunImageTests},
        {"textes et images à l'écran", RunScreenUiTests},
        {"interfaces (.fluxui)", RunInterfaceTests},
        {"changement de scène", RunSceneChangeTests},
        {"modèles 3D (glTF)", RunModelTests},
        {"physique (Jolt)", RunPhysicsTests},
        {"animation squelettique", RunAnimationTests},
        {"cascades d'ombres", RunShadowTests},
        {"ciel", RunSkyTests},
        {"son", RunAudioTests},
        {"actions d'entrée", RunInputTests},
        {"planètes (cube-sphère)", RunPlanetTests},
        {"calculs sur le GPU (compute)", RunComputeTests},
        {"tuiles de planète sur le GPU", RunPlanetGpuTests},
        {"armes des vaisseaux", RunWeaponsTests},
        {"collisions des vaisseaux et amarrage", RunDockingAutopilotTests},
        {"missions", RunMissionTests},
        {"warp", RunWarpTests},
    };

    int failures = 0;
    for (const Group& group : groups) {
        const bool passed = group.run();
        std::printf("%s %s\n", passed ? "OK    " : "ÉCHEC ", group.name);
        failures += passed ? 0 : 1;
    }
    std::printf(failures == 0 ? "tous les tests passent\n" : "%d groupe(s) en échec\n", failures);
    return failures == 0 ? 0 : 1;
}
