// Lance les tests de la simulation (FluxSim) ; code de sortie 0 si tout passe (lancer FluxSimTests ou ctest).
#include "Check.hpp"

#include <chrono>

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
        {"simulation sans rendu", RunSimulationTests},
        {"jeu simulé (GameSim)", RunGameSimTests},
    };

    int failures = 0;
    for (const Group& group : groups) {
        // Durée de chaque groupe : la CI les affiche (ctest -V), pour voir lesquels la ralentissent.
        const auto start = std::chrono::steady_clock::now();
        const bool passed = group.run();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%s %s (%.1f s)\n", passed ? "OK    " : "ÉCHEC ", group.name, seconds);
        failures += passed ? 0 : 1;
    }
    std::printf(failures == 0 ? "tous les tests passent\n" : "%d groupe(s) en échec\n", failures);
    return failures == 0 ? 0 : 1;
}
