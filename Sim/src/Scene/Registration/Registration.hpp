#pragma once

#include "Flux/Scene/ComponentRegistry.hpp"

// Enregistrement des composants du moteur, un fichier par domaine (ShipRegistration.cpp...), tous appelés par
// RegisterEngineComponents (ComponentRegistry.cpp).
namespace Flux {
    void RegisterRenderComponents(ComponentRegistry& registry);
    void RegisterPlanetComponents(ComponentRegistry& registry);
    void RegisterShipComponents(ComponentRegistry& registry);
    void RegisterCharacterComponents(ComponentRegistry& registry);
    void RegisterInterfaceComponents(ComponentRegistry& registry);
}
