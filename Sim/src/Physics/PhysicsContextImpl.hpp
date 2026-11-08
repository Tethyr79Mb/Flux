#pragma once

// Interne au moteur : le contexte physique vu par les mondes (PhysicsWorld.cpp). Seul endroit, avec les .cpp de la
// physique, où des en-têtes Jolt sont inclus.

#include "Flux/Math/Vector3.hpp"
#include "Flux/Physics/PhysicsContext.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Flux {
    // Maillage de collision lu une fois : sa forme Jolt (nullptr : illisible, pas de nouvel essai tant que le fichier
    // ne change pas) et les points de son enveloppe convexe, dans son repère, pour celle des vaisseaux.
    struct CollisionMesh {
        JPH::RefConst<JPH::Shape> shape;
        std::vector<Math::Vector3f> hull;
        std::filesystem::file_time_type time{};
    };

    struct PhysicsContext::Impl {
        Impl();
        ~Impl();

        // Maillage du fichier (chemin relatif aux assets), lu à la première demande ou si le fichier a changé ;
        // nullptr sans dossier des assets.
        const CollisionMesh* FindMesh(std::string_view relative);

        std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
        std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
        std::unique_ptr<JPH::JobSystemSingleThreaded> serialJobs; // petits mondes (voir ParallelBodyCount)
        std::filesystem::path assetsDirectory;
        std::unordered_map<std::string, CollisionMesh> meshes;
    };
}
