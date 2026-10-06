#include "PhysicsContextImpl.hpp"

#include "Flux/Assets/Model.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace Flux {
    namespace {
        // Initialisation globale de Jolt (allocateur, fabrique de types) : une fois pour tous les contextes.
        std::mutex g_joltMutex;
        int g_joltUsers = 0;

        void AcquireJolt() {
            const std::scoped_lock lock(g_joltMutex);
            if (g_joltUsers++ == 0) {
                JPH::RegisterDefaultAllocator();
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            }
        }

        void ReleaseJolt() {
            const std::scoped_lock lock(g_joltMutex);
            if (--g_joltUsers == 0) {
                JPH::UnregisterTypes();
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
        }

        // Enveloppe convexe d'un nuage de points (quickhull de Jolt), limitée à 256 sommets : quelques centaines de
        // points au lieu des dizaines de milliers de sommets d'un maillage, la même enveloppe à quelques centimètres
        // près. Elle reste valable après une transformation affine (échelle, rotation) : ce sont des points du nuage.
        std::vector<Math::Vector3f> ComputeHull(const std::vector<Math::Vector3f>& points) {
            std::vector<Math::Vector3f> hull;
            JPH::ConvexHullBuilder::Positions positions;
            positions.reserve(points.size());
            for (const Math::Vector3f& point : points) {
                positions.push_back(JPH::Vec3(point.x, point.y, point.z));
            }
            JPH::ConvexHullBuilder builder(positions);
            const char* error = nullptr;
            const JPH::ConvexHullBuilder::EResult result = builder.Initialize(256, 0.01f, error);
            if (result != JPH::ConvexHullBuilder::EResult::Success && result != JPH::ConvexHullBuilder::EResult::MaxVerticesReached) {
                return points.size() <= 256 ? points : hull; // plat ou dégénéré : sans enveloppe
            }
            std::vector<bool> used(points.size(), false);
            for (const JPH::ConvexHullBuilder::Face* face : builder.GetFaces()) {
                const JPH::ConvexHullBuilder::Edge* edge = face->mFirstEdge;
                do {
                    if (!used[edge->mStartIdx]) {
                        used[edge->mStartIdx] = true;
                        hull.push_back(points[edge->mStartIdx]);
                    }
                    edge = edge->mNextEdge;
                } while (edge != face->mFirstEdge);
            }
            return hull;
        }

        CollisionMesh LoadMesh(const std::filesystem::path& path) {
            CollisionMesh mesh;
            std::error_code error;
            mesh.time = std::filesystem::last_write_time(path, error);
            const std::string name = Platform::ToUtf8(path.filename());
            std::expected<ModelData, std::string> model = LoadModelFile(path, false);
            if (!model) {
                Log::Warning("maillage de collision illisible : {}", model.error());
                return mesh;
            }
            if (model->IsAnimated()) {
                Log::Warning("« {} » est animé : pas de collision sur ses triangles (prendre une capsule ou un fichier "
                             "statique)", name);
                return mesh;
            }
            JPH::VertexList vertices;
            vertices.reserve(model->vertices.size());
            std::vector<Math::Vector3f> points;
            points.reserve(model->vertices.size());
            for (const ModelVertex& vertex : model->vertices) {
                vertices.push_back(JPH::Float3(vertex.position.x, vertex.position.y, vertex.position.z));
                points.push_back(vertex.position);
            }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(model->indices.size() / 3);
            for (std::size_t i = 0; i + 2 < model->indices.size(); i += 3) {
                triangles.push_back(JPH::IndexedTriangle(model->indices[i], model->indices[i + 1], model->indices[i + 2]));
            }
            // Les triangles plats (arêtes confondues) sont retirés par Jolt.
            const JPH::ShapeSettings::ShapeResult result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
            if (result.HasError()) {
                Log::Warning("maillage de collision « {} » refusé : {}", name, result.GetError().c_str());
                return mesh;
            }
            mesh.shape = result.Get();
            mesh.hull = ComputeHull(points);
            return mesh;
        }
    }

    PhysicsContext::Impl::Impl() {
        AcquireJolt();
        tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
        // Tous les cœurs sauf un (le thread principal, qui dessine), au moins un.
        const int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
        jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
    }

    PhysicsContext::Impl::~Impl() {
        meshes.clear(); // formes Jolt : avant que Jolt soit libéré
        jobSystem.reset();
        tempAllocator.reset();
        ReleaseJolt();
    }

    const CollisionMesh* PhysicsContext::Impl::FindMesh(std::string_view relative) {
        if (assetsDirectory.empty()) {
            return nullptr;
        }
        const std::filesystem::path path = assetsDirectory / Platform::PathFromUtf8(relative);
        auto found = meshes.find(std::string(relative));
        if (found != meshes.end()) {
            // Fichier remplacé depuis (modèle refait, l'éditeur ouvert) : relu.
            std::error_code error;
            const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
            if (!error && time != found->second.time) {
                found->second = LoadMesh(path);
            }
            return &found->second;
        }
        return &meshes.emplace(std::string(relative), LoadMesh(path)).first->second;
    }

    PhysicsContext::PhysicsContext() : m_impl(std::make_unique<Impl>()) {}

    PhysicsContext::~PhysicsContext() = default;

    void PhysicsContext::SetAssetsDirectory(const std::filesystem::path& directory) {
        if (directory != m_impl->assetsDirectory) {
            m_impl->assetsDirectory = directory;
            m_impl->meshes.clear();
        }
    }

    int PhysicsContext::GetMeshCount() const {
        return static_cast<int>(m_impl->meshes.size());
    }
}
