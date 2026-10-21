#pragma once

#include <cstdio>
#include <cstdlib>

// Mini-cadre de test : chaque groupe est une fonction bool, CHECK sort du groupe au premier échec.
#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #condition);              \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

// Groupe qui demande un GPU, sur une machine qui n'en a pas : ignoré (réussi), sauf si FLUX_TESTS_GPU vaut 1 (la CI,
// qui installe un GPU logiciel) : là, un GPU absent par erreur fait échouer au lieu de passer inaperçu.
inline bool GpuMissing(const char* group, const char* error) {
#ifdef _MSC_VER
#pragma warning(suppress : 4996) // getenv : lu avant tout fil
#endif
    const char* required = std::getenv("FLUX_TESTS_GPU");
    if (required && required[0] == '1') {
        std::printf("ÉCHEC %s : GPU exigé (FLUX_TESTS_GPU), aucun trouvé : %s\n", group, error);
        return false;
    }
    std::printf("       %s ignoré : %s\n", group, error);
    return true;
}

bool RunSceneTests();
bool RunSceneSerializerTests();
bool RunPrefabTests();
bool RunUiTests();
bool RunProjectTests();
bool RunGameSettingsTests();
bool RunPickingTests();
bool RunScreenUiTests();
bool RunInterfaceTests();
bool RunImageTests();
bool RunSceneChangeTests();
bool RunModelTests();
bool RunPhysicsTests();
bool RunAnimationTests();
bool RunShadowTests();
bool RunSkyTests();
bool RunAudioTests();
bool RunInputTests();
bool RunPlanetTests();
bool RunComputeTests();
bool RunPlanetGpuTests();
bool RunVegetationGpuTests();
bool RunWeaponsTests();
bool RunDockingAutopilotTests();
bool RunMissionTests();
bool RunWarpTests();
bool RunCombatAITests();
bool RunGeometryTests();
