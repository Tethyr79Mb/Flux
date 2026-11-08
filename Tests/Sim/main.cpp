// Lance les tests de la simulation (FluxSim) ; code de sortie 0 si tout passe (lancer FluxSimTests ou ctest).
#include "Check.hpp"

int main() {
    struct Group {
        const char* name;
        bool (*run)();
    };
    constexpr Group groups[] = {
        {"scène", RunSceneTests},
        {"sauvegarde de scène", RunSceneSerializerTests},
        {"partie reprise dans une scène modifiée", RunSceneMergeTests},
        {"prefabs", RunPrefabTests},
        {"projet", RunProjectTests},
        {"changement de scène", RunSceneChangeTests},
        {"modèles 3D (glTF)", RunModelTests},
        {"physique (Jolt)", RunPhysicsTests},
        {"animation squelettique", RunAnimationTests},
        {"fils de travail (JobSystem)", RunJobSystemTests},
        {"espèces et biomes des arbres", RunVegetationTests},
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
