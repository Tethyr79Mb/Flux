#pragma once

#include "../Sim/Check.hpp" // CHECK et groupes de la simulation (FluxSimTests)
#include "Flux/Platform/Environment.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

// Groupe qui demande un GPU, sur une machine qui n'en a pas : ignoré (réussi), sauf si FLUX_TESTS_GPU vaut 1 (la CI,
// qui installe un GPU logiciel) : là, un GPU absent par erreur fait échouer au lieu de passer inaperçu.
inline bool GpuMissing(const char* group, const char* error) {
    const std::optional<std::string> required = Flux::Platform::ReadEnvironment("FLUX_TESTS_GPU");
    if (required && required->starts_with('1')) {
        std::printf("ÉCHEC %s : GPU exigé (FLUX_TESTS_GPU), aucun trouvé : %s\n", group, error);
        return false;
    }
    std::printf("       %s ignoré : %s\n", group, error);
    return true;
}

bool RunRenderGraphTests();
bool RunUiTests();
bool RunGameSettingsTests();
bool RunPickingTests();
bool RunScreenUiTests();
bool RunInterfaceTests();
bool RunImageTests();
bool RunLocalHttpServerTests();
bool RunTextureCompressionTests();
bool RunShadowTests();
bool RunSkyTests();
bool RunEnvironmentGpuTests();
bool RunExposureTests();
bool RunExposureGpuTests();
bool RunAudioTests();
bool RunInputTests();
bool RunSimulationClockTests();
bool RunPlanetTests();
bool RunComputeTests();
bool RunRayTracingTests();
bool RunTransferTests();
bool RunPlanetGpuTests();
bool RunCloudNoiseTests();
bool RunVegetationGpuTests();
bool RunWeaponsTests();
bool RunDockingAutopilotTests();
bool RunMissionTests();
bool RunWarpTests();
bool RunPlayerCommandTests();
bool RunCombatAITests();
bool RunPlaceItemsTests(); // Game/ : code du jeu (GameLogic)
bool RunGeometryTests();
