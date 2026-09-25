#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>

FLUX_GAME_MODULE_EXPORT std::uint32_t FluxGameApiVersion() {
    return Flux::GameModuleApiVersion;
}

FLUX_GAME_MODULE_EXPORT void FluxRegisterGame(Flux::Engine& engine) {
    // Enregistrer ici les composants et systèmes du jeu.
    (void)engine;
}

FLUX_GAME_MODULE_EXPORT void FluxUnregisterGame(Flux::Engine& engine) {
    // Retirer tout ce que FluxRegisterGame a enregistré : la DLL va être déchargée.
    (void)engine;
}
