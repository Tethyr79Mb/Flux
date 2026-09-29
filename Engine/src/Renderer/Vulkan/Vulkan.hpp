#pragma once

// Point d'entrée unique vers Vulkan pour les sources du moteur.
// VK_NO_PROTOTYPES, VK_USE_PLATFORM_WIN32_KHR et la configuration de VMA sont posés par CMake.
#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdio>

namespace Flux::Vk {
    // Journalise l'échec d'un appel Vulkan ; retourne true si le résultat est un succès.
    inline bool Check(VkResult result, const char* call) {
        if (result >= VK_SUCCESS) {
            return true;
        }
        std::fprintf(stderr, "Vulkan : %s a échoué (VkResult %d)\n", call, static_cast<int>(result));
        return false;
    }
}
