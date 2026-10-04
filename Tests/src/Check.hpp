#pragma once

#include <cstdio>

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
bool RunUiTests();
bool RunProjectTests();
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
