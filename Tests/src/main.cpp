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
        {"partie reprise dans une scène modifiée", RunSceneMergeTests},
        {"prefabs", RunPrefabTests},
        {"interface (FluxUI) et rotations", RunUiTests},
        {"projet", RunProjectTests},
        {"réglages du jeu", RunGameSettingsTests},
        {"sélection dans la vue 3D", RunPickingTests},
        {"images et textures", RunImageTests},
        {"textures compressées (BC7)", RunTextureCompressionTests},
        {"textes et images à l'écran", RunScreenUiTests},
        {"interfaces (.fluxui)", RunInterfaceTests},
        {"changement de scène", RunSceneChangeTests},
        {"modèles 3D (glTF)", RunModelTests},
        {"physique (Jolt)", RunPhysicsTests},
        {"animation squelettique", RunAnimationTests},
        {"cascades d'ombres", RunShadowTests},
        {"graphe de rendu", RunRenderGraphTests},
        {"ciel", RunSkyTests},
        {"reflets et lumière du ciel sur le GPU", RunEnvironmentGpuTests},
        {"exposition automatique et courbe de tons", RunExposureTests},
        {"mesure de la lumière sur le GPU", RunExposureGpuTests},
        {"son", RunAudioTests},
        {"actions d'entrée", RunInputTests},
        {"serveur HTTP local (outils MCP)", RunLocalHttpServerTests},
        {"planètes (cube-sphère)", RunPlanetTests},
        {"calculs sur le GPU (compute)", RunComputeTests},
        {"envois vers le GPU (file de transfert)", RunTransferTests},
        {"tuiles de planète sur le GPU", RunPlanetGpuTests},
        {"bruit 3D des nuages sur le GPU", RunCloudNoiseTests},
        {"espèces et biomes des arbres", RunVegetationTests},
        {"arbres des planètes sur le GPU", RunVegetationGpuTests},
        {"armes des vaisseaux", RunWeaponsTests},
        {"collisions des vaisseaux et amarrage", RunDockingAutopilotTests},
        {"missions", RunMissionTests},
        {"warp", RunWarpTests},
        {"IA de combat", RunCombatAITests},
        {"objets du jeu à placer (éditeur)", RunPlaceItemsTests},
        {"géométrie partagée des modèles", RunGeometryTests},
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
