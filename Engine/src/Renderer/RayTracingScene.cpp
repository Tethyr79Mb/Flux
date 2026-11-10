#include "Renderer/RayTracingScene.hpp"

#include "Flux/Core/Log.hpp"
#include "Renderer/ModelCache.hpp"

#include <algorithm>
#include <bit>

namespace Flux {
    namespace {
        // Mémoire de travail des structures basses construites en une image : au-delà, les suivantes attendent l'image
        // d'après (une station entière arrive en plusieurs images au lieu d'un à-coup). Toujours au moins une.
        constexpr std::uint64_t BottomScratchBudget = 64ull << 20;
        // Instances de la première structure haute ; doublées quand il en faut plus.
        constexpr std::uint32_t MinInstances = 256;
    }

    void RayTracingScene::Begin() {
        m_instances.clear();
        m_toBuild.clear();
        m_toBuildModels.clear();
        m_toBuildScratch = 0;
        m_used.clear();
        m_ready = false;
        m_stats = {};
    }

    void RayTracingScene::Add(const GpuModel& model, const Math::Matrix4f& world, std::uint32_t index) {
        RHI::AccelerationStructure* bottom = model.bottomLevel.get();
        if (!bottom) {
            return;
        }
        if (!model.bottomLevelBuilt) {
            const std::uint64_t scratch = bottom->GetScratchSize();
            if (!m_toBuild.empty() && m_toBuildScratch + scratch > BottomScratchBudget) {
                ++m_stats.waiting; // une entité de plus qui attend (pas un modèle : compté par entité)
                return;
            }
            model.bottomLevelBuilt = true;
            m_toBuild.push_back(bottom);
            m_toBuildModels.push_back(&model);
            m_toBuildScratch += scratch;
        }
        RHI::AccelerationInstance instance;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                instance.transform[row][column] = world.columns[column][row];
            }
        }
        instance.customIndex = index;
        instance.bottom = bottom;
        m_instances.push_back(instance);
        m_used.push_back(bottom);
    }

    bool RayTracingScene::Reserve(std::unique_ptr<RHI::Buffer>& buffer, std::uint64_t size) {
        if (buffer && buffer->GetSize() >= size) {
            return true;
        }
        // Une marge d'un quart : pas de nouvelle allocation pour quelques octets de plus à l'image suivante.
        auto created = m_device.CreateBuffer({size + size / 4, RHI::BufferUsage::AccelerationScratch, RHI::MemoryType::GpuOnly});
        if (!created) {
            Log::Error("ray tracing : mémoire de travail : {}", created.error());
            return false;
        }
        if (buffer) {
            m_device.Retire([old = std::move(buffer)]() mutable { old.reset(); });
        }
        buffer = std::move(*created);
        return true;
    }

    bool RayTracingScene::End(std::uint32_t frameIndex) {
        Frame& frame = m_frames[frameIndex];
        const auto count = static_cast<std::uint32_t>(m_instances.size());
        // Les ressources de cette image ne servent plus au GPU (BeginFrame a attendu) : remplacées sur place.
        if (!frame.topLevel || frame.topLevel->GetMaxInstances() < count) {
            auto created = m_device.CreateTopLevel(std::max(std::bit_ceil(count), MinInstances));
            if (!created) {
                Log::Error("ray tracing : structure de la scène : {}", created.error());
            } else {
                if (frame.topLevel) {
                    m_device.Retire([old = std::move(frame.topLevel)]() mutable { old.reset(); });
                }
                frame.topLevel = std::move(*created);
            }
        }
        m_ready = frame.topLevel && frame.topLevel->GetMaxInstances() >= count
               && Reserve(frame.topScratch, frame.topLevel->GetScratchSize())
               && (m_toBuild.empty() || Reserve(frame.bottomScratch, m_toBuildScratch));
        if (!m_ready) {
            // Rien ne sera construit : ces modèles seront redemandés à l'image suivante.
            for (const GpuModel* model : m_toBuildModels) {
                model->bottomLevelBuilt = false;
            }
            m_toBuild.clear();
            m_toBuildModels.clear();
            m_instances.clear();
        } else if (m_toBuild.empty() && frame.bottomScratch) {
            // Jusqu'à 80 Mo par image en vol qui ne servent qu'à l'arrivée de nouveaux modèles (rare) : rendus aussitôt,
            // quitte à les réallouer à la prochaine arrivée.
            m_device.Retire([old = std::move(frame.bottomScratch)]() mutable { old.reset(); });
        }

        std::ranges::sort(m_used);
        const auto [first, last] = std::ranges::unique(m_used);
        m_used.erase(first, last);
        m_stats.instances = static_cast<std::uint32_t>(m_instances.size());
        m_stats.models = static_cast<std::uint32_t>(m_used.size());
        m_stats.built = static_cast<std::uint32_t>(m_toBuild.size());
        for (const RHI::AccelerationStructure* bottom : m_used) {
            m_stats.modelBytes += bottom->GetSize();
        }
        for (const Frame& each : m_frames) {
            m_stats.sceneBytes += (each.topLevel ? each.topLevel->GetSize() : 0) + (each.topScratch ? each.topScratch->GetSize() : 0)
                                + (each.bottomScratch ? each.bottomScratch->GetSize() : 0);
        }
        return m_ready;
    }

    void RayTracingScene::BuildBottomLevels(RHI::CommandList& commands, std::uint32_t frameIndex) {
        if (m_ready && !m_toBuild.empty()) {
            commands.BuildBottomLevels(m_toBuild, *m_frames[frameIndex].bottomScratch);
        }
    }

    void RayTracingScene::BuildTopLevel(RHI::CommandList& commands, std::uint32_t frameIndex) {
        Frame& frame = m_frames[frameIndex];
        if (m_ready) {
            commands.BuildTopLevel(*frame.topLevel, m_instances, *frame.topScratch);
        }
    }
}
