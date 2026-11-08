#pragma once

#include <cstdio>

// Tests de la simulation (FluxSim), compilés sous Windows et sous Linux : rien ici ne dépend de FluxEngine.
//
// Mini-cadre de test : chaque groupe est une fonction bool, CHECK sort du groupe au premier échec.
#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #condition);              \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

bool RunSceneTests();
bool RunSceneSerializerTests();
bool RunSceneMergeTests();
bool RunSceneChangeTests();
bool RunPrefabTests();
bool RunProjectTests();
bool RunJobSystemTests();
bool RunModelTests();
bool RunPhysicsTests();
bool RunAnimationTests();
bool RunVegetationTests();
bool RunSimulationTests();
bool RunGameSimTests();
bool RunNetTests();
