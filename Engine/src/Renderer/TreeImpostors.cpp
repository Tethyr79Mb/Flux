#include "Renderer/TreeImpostors.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <vector>

namespace Flux {
    std::expected<std::unique_ptr<TreeImpostors>, std::string> TreeImpostors::Create(RHI::Device& device, ModelCache& models) {
        std::unique_ptr<TreeImpostors> impostors(new TreeImpostors(device, models));
        for (auto& commands : impostors->m_commands) {
            auto created = device.CreateBuffer({MaxPrimitives * sizeof(RHI::DrawIndexedCommand), RHI::BufferUsage::Indirect,
                                                RHI::MemoryType::Upload});
            if (!created) {
                return std::unexpected(created.error());
            }
            commands = std::move(*created);
        }
        return impostors;
    }

    TreeImpostors::~TreeImpostors() {
        // Les cases rendues par Retire le sont à ModelCache, détruit après : tout de suite, le GPU fini.
        m_device.WaitIdle();
        for (auto& [model, entry] : m_entries) {
            if (entry.slot != 0) {
                m_models.FreeImages(entry.slot, 2);
            }
        }
    }

    void TreeImpostors::Retire(Entry& entry) {
        m_device.Retire([models = &m_models, color = std::move(entry.color), normal = std::move(entry.normal), slot = entry.slot]() mutable {
            if (slot != 0) {
                models->FreeImages(slot, 2);
            }
            color.reset();
            normal.reset();
        });
        entry.slot = 0;
        entry.ready = false;
    }

    std::uint32_t TreeImpostors::Find(const GpuModel& model) {
        auto [found, created] = m_entries.try_emplace(&model);
        Entry& entry = found->second;
        entry.lastUsed = m_frame;
        const bool changed = entry.page != model.resources.page || entry.firstVertex != model.resources.firstVertex
                          || entry.writeTime != model.writeTime;
        if (!created && changed) {
            Retire(entry);
        }
        if (created || changed) {
            entry.failed = false;
            entry.page = model.resources.page;
            entry.firstVertex = model.resources.firstVertex;
            entry.writeTime = model.writeTime;
        }
        return entry.ready ? entry.slot : 0u;
    }

    void TreeImpostors::EndFrame(std::uint32_t frameIndex) {
        // Plus demandés depuis longtemps : rendus (ils seraient capturés à nouveau au retour sur la planète).
        std::erase_if(m_entries, [&](auto& item) {
            if (m_frame - item.second.lastUsed <= UnusedFrames) {
                return false;
            }
            Retire(item.second);
            return true;
        });
        ++m_frame;

        // La capture de l'image : le premier modèle demandé pas encore capturé.
        m_capture = nullptr;
        for (auto& [model, entry] : m_entries) {
            if (entry.ready || entry.failed || entry.lastUsed + 1 != m_frame || model->primitives.empty()) {
                continue;
            }
            if (!entry.color) {
                auto color = m_device.CreateRenderTarget(ColorFormat, {Size, Size}, true, false, MipLevels);
                auto normal = m_device.CreateRenderTarget(NormalFormat, {Size, Size}, true, false, MipLevels);
                const std::optional<std::uint32_t> slot = m_models.AllocateImages(2);
                if (!color || !normal || !slot) {
                    // Le modèle lointain reste ; nouvel essai si le modèle est relu.
                    if (slot) {
                        m_models.FreeImages(*slot, 2);
                    }
                    Log::Warning("Imposteur d'arbre impossible : {}", !color ? color.error() : !normal ? normal.error() : "table des images pleine");
                    entry.failed = true;
                    continue;
                }
                entry.color = std::move(*color);
                entry.normal = std::move(*normal);
                entry.slot = *slot;
                m_models.SetImage(entry.slot, *entry.color);
                m_models.SetImage(entry.slot + 1, *entry.normal);
            }
            if (!m_depth) {
                auto depth = m_device.CreateRenderTarget(DepthFormat, {Size, Size});
                if (!depth) {
                    Log::Warning("Imposteurs des arbres : {}", depth.error());
                    return;
                }
                m_depth = std::move(*depth);
            }
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(model->primitives.size(), MaxPrimitives));
            std::array<RHI::DrawIndexedCommand, MaxPrimitives> commands{};
            for (std::uint32_t p = 0; p < count; ++p) {
                const ModelPrimitive& primitive = model->primitives[p];
                commands[p] = {primitive.indexCount, Grid * Grid, primitive.firstIndex,
                               static_cast<std::int32_t>(model->resources.firstVertex), 0};
            }
            m_commands[frameIndex]->Write(commands.data(), count * sizeof(RHI::DrawIndexedCommand));
            m_capture = model;
            m_captureFrame = frameIndex;
            m_capturePrimitives = count;
            break;
        }
    }

    std::optional<TreeImpostors::Capture> TreeImpostors::GetCapture(std::uint32_t frameIndex) const {
        if (!m_capture || frameIndex != m_captureFrame) {
            return std::nullopt;
        }
        const Entry& entry = m_entries.at(m_capture);
        return Capture{m_capture, entry.color.get(), entry.normal.get(), m_depth.get(), m_commands[frameIndex].get(), m_capturePrimitives};
    }

    void TreeImpostors::Captured() {
        if (const auto found = m_entries.find(m_capture); found != m_entries.end() && !found->second.ready) {
            found->second.ready = true;
        }
        m_capture = nullptr;
    }
}
