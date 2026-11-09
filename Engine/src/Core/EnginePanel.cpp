#include "Core/EnginePanel.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/FlyCameraController.hpp"
#include "Flux/Platform/Memory.hpp"
#include "Flux/UI/Context.hpp"
#include "Core/Measurements.hpp"
#include "Scene/Photometry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace Flux {
    namespace {
        std::string FormatCost(double milliseconds) {
            return milliseconds < 0.0 ? std::string("?") : std::format("{:.2f} ms", milliseconds);
        }

        std::string FormatMegabytes(std::uint64_t bytes) {
            return std::format("{:.0f} Mo", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }

        // Durée d'un relevé lancé depuis le panneau Moteur.
        constexpr double MeasureSeconds = 10.0;

        // En EV100, comme les bornes du Ciel (Photometry::ExposureFromEv100 à l'envers).
        float ExposureToEv100(float exposure) {
            return std::log2(Photometry::NitsPerUnit / (1.2f * std::max(exposure, 1.0e-12f)));
        }
    }

    void DrawEnginePanel(UI::Context& ui, Renderer& renderer, const FrameStats& stats, MeasureRecorder& measures,
                         Camera& camera, FlyCameraController* controller) {
        if (ui.BeginPanel("Moteur", {8.0f, 8.0f}, 360.0f)) {
            renderer.RequestDetailedTimings();
            // Le relevé écrit toutes ces mesures dans un fichier (Mesures/ à côté de l'exécutable).
            if (measures.IsRecording()) {
                ui.LabelText("Enregistrement", std::format("encore {:.0f} s", std::ceil(measures.GetRemainingSeconds())));
            } else if (ui.Button("Enregistrer les mesures (10 s)")) {
                measures.Start(MeasureSeconds);
            }
            // Toujours visible : la cadence et qui la mène. Lecture : si « Attente écran » domine alors que le GPU
            // travaille peu, la cadence vient de l'extérieur (V-Sync, pilote) ; si le GPU est long, c'est le
            // dessin ; sinon, le CPU. Le détail est replié en sections, pour tenir dans l'écran.
            ui.LabelText("Images/s", std::format("{:.0f} ({:.2f} ms)", stats.GetFps(), stats.GetFrameMilliseconds()));
            // Moins de 60 : la machine ne suit plus, le jeu ralentit (au plus 4 pas par image).
            ui.LabelText("Pas de simulation/s", std::format("{:.0f}", stats.GetStepsPerSecond()));
            ui.LabelText("Pire image (10 s)", std::format("{:.1f} ms", stats.GetWorstMilliseconds()));
            ui.LabelText("CPU màj + rendu", std::format("{:.2f} + {:.2f} ms (jeu {:.2f})", stats.Get(UpdateCost),
                                                        stats.Get(PrepareCost) + stats.Get(RecordCost), stats.Get(GameCost)));
            ui.LabelText("Attente", std::format("GPU {:.2f}, écran {:.2f} ms", stats.Get(WaitGpuCost), stats.Get(WaitImageCost)));

            // Une ligne par passe du graphe de rendu ; ses étapes à part, repliées (une vingtaine de lignes).
            double gpuTotal = 0.0;
            for (const FrameStats::Row& row : stats.GetGpu()) {
                gpuTotal += !row.step && row.average > 0.0 ? row.average : 0.0;
            }
            ui.LabelText("GPU", FormatCost(gpuTotal));
            if (ui.CollapsingHeader("Temps du GPU")) {
                for (const FrameStats::Row& row : stats.GetGpu()) {
                    if (!row.step) {
                        ui.LabelText(row.name, FormatCost(row.average));
                    }
                }
            }
            if (ui.CollapsingHeader("Étapes du GPU", false)) {
                // Les passes d'une seule étape n'ont rien à détailler.
                const auto rows = stats.GetGpu();
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    const bool hasSteps = i + 1 < rows.size() && rows[i + 1].step;
                    if (rows[i].step || hasSteps) {
                        ui.LabelText(rows[i].step ? std::format("  {}", rows[i].name) : std::string(rows[i].name),
                                     FormatCost(rows[i].average));
                    }
                }
            }

            // Préparation du rendu, par partie : où partent les millisecondes avant l'enregistrement.
            if (ui.CollapsingHeader("Préparation (CPU)", false)) {
                ui.LabelText("Total", std::format("{:.3f} ms", stats.Get(PrepareCost)));
                ui.LabelText("Ciel et exposition", std::format("{:.3f} ms", stats.Get(PrepareSkyCost)));
                ui.LabelText("Entités", std::format("{:.3f} ms", stats.Get(PrepareEntitiesCost)));
                ui.LabelText("Sol des planètes", std::format("{:.3f} ms", stats.Get(PrepareGroundCost)));
                ui.LabelText("Arbres", std::format("{:.3f} ms", stats.Get(PrepareTreesCost)));
                ui.LabelText("Envois et lots", std::format("{:.3f} ms", stats.Get(PrepareUploadsCost)));
            }

            // Temps CPU d'enregistrement de chaque passe (moyenne glissante), sur le fil qui l'a faite : la somme dépasse
            // « rendu » quand plusieurs fils enregistrent en même temps.
            if (ui.CollapsingHeader("Enregistrement (CPU)", false)) {
                bool parallel = renderer.IsParallelRecording();
                if (ui.Checkbox("Rendu sur plusieurs fils", parallel)) {
                    renderer.SetParallelRecording(parallel);
                }
                const RenderTimings& render = renderer.GetTimings();
                ui.LabelText("Listes de commandes", std::format("{}", render.recordLists));
                for (const RenderPassTiming& pass : render.gpu) {
                    if (!pass.step && pass.cpuMilliseconds >= 0.0) {
                        ui.LabelText(pass.name, std::format("{:.3f} ms", pass.cpuMilliseconds));
                    }
                }
            }

            if (ui.CollapsingHeader("Scène", false)) {
                // V-Sync et anticrénelage : dans Fichier > Paramètres (le panneau garde la mesure).
                bool occlusion = renderer.IsOcclusionCullingEnabled();
                if (ui.Checkbox("Objets cachés écartés", occlusion)) {
                    renderer.SetOcclusionCulling(occlusion);
                }
                bool geometryOnly = renderer.IsGroundGeometryOnly();
                if (ui.Checkbox("Sol sans pixels (mesure)", geometryOnly)) {
                    renderer.SetGroundGeometryOnly(geometryOnly);
                }
                const RenderTimings& render = renderer.GetTimings();
                const float ev100 = ExposureToEv100(render.exposure);
                ui.LabelText("Exposition", render.sceneLuminance > 0.0f
                                               ? std::format("EV100 {:.1f} (luminance mesurée {:.3g} cd/m²)", ev100,
                                                             render.sceneLuminance * Photometry::NitsPerUnit)
                                               : std::format("EV100 {:.1f} (rien de mesurable)", ev100));
                ui.LabelText("Objets testés", std::format("{} dont {} cachés", render.occlusionTested, render.occlusionHidden));
                ui.LabelText("Paquets des modèles", render.modelMeshShaders
                                                        ? std::format("{} dont {} dessinés", render.modelMeshlets,
                                                                      render.modelMeshletsDrawn)
                                                        : std::string("par les sommets"));
                if (render.modelsLoading > 0) {
                    ui.LabelText("Modèles en chargement", std::format("{}", render.modelsLoading));
                }
                ui.LabelText("Tuiles de planète", std::format("{} ({} eau), {}/{} gardées", render.planetTiles,
                                                              render.planetOceanTiles, render.planetStoredTiles,
                                                              render.planetTileCapacity));
                ui.LabelText("Sol dessiné par", render.groundMeshThreads > 0
                                                     ? std::format("mesh shaders ({} threads)", render.groundMeshThreads)
                                                     : std::string("sommets"));
                ui.LabelText("Arbres", std::format("{} près, {} simplifiés, {} loin", render.trees[0], render.trees[1], render.trees[2]));
                ui.LabelText("  ombres, plaines", std::format("{}, {} (+{} à semer, {} d'avance)", render.shadowTrees,
                                                             render.treePlains, render.pendingTreePlains, render.aheadTreePlains));
            }

            // RAM utilisée : présente en RAM en ce moment, bibliothèques partagées comprises (le Gestionnaire des
            // tâches montre la part privée, plus petite) ; réservée : promise au processus, même pas encore
            // touchée (pilote compris). GPU : ce que le rendu a demandé au pilote, en mémoire vidéo et en RAM.
            if (ui.CollapsingHeader("Mémoire", false)) {
                const Platform::ProcessMemory memory = Platform::GetProcessMemory();
                const auto [gpuDevice, gpuHost] = renderer.GetGpuMemory();
                ui.LabelText("RAM", std::format("{} (réservée {})", FormatMegabytes(memory.workingSet),
                                                FormatMegabytes(memory.privateBytes)));
                ui.LabelText("Mémoire vidéo", std::format("{} (+ {} en RAM)", FormatMegabytes(gpuDevice), FormatMegabytes(gpuHost)));
            }

            if (controller && ui.CollapsingHeader("Caméra", false)) {
                controller->DrawFields(ui, camera);
            }
        }
        ui.EndPanel();
    }

    std::string DescribeEngineStats(const Renderer& renderer, const FrameStats& stats) {
        using Json = nlohmann::ordered_json;
        const RenderTimings& render = renderer.GetTimings();
        Json json;
        json["fps"] = stats.GetFps();
        json["stepsPerSecond"] = stats.GetStepsPerSecond();
        json["frameMs"] = stats.GetFrameMilliseconds();
        json["worstFrameMs"] = stats.GetWorstMilliseconds();
        json["cpu"] = {{"updateMs", stats.Get(UpdateCost)},
                       {"gameMs", stats.Get(GameCost)},
                       {"prepareMs", stats.Get(PrepareCost)},
                       {"prepareParts", {{"skyMs", stats.Get(PrepareSkyCost)},
                                         {"entitiesMs", stats.Get(PrepareEntitiesCost)},
                                         {"groundMs", stats.Get(PrepareGroundCost)},
                                         {"treesMs", stats.Get(PrepareTreesCost)},
                                         {"uploadsMs", stats.Get(PrepareUploadsCost)}}},
                       {"recordMs", stats.Get(RecordCost)},
                       {"waitGpuMs", stats.Get(WaitGpuCost)},
                       {"waitScreenMs", stats.Get(WaitImageCost)}};
        double gpuTotal = 0.0;
        Json passes = Json::array();
        for (const FrameStats::Row& row : stats.GetGpu()) {
            gpuTotal += !row.step && row.average > 0.0 ? row.average : 0.0;
            passes.push_back({{"name", row.name}, {"step", row.step}, {"ms", row.average}});
        }
        Json recording = Json::array();
        for (const RenderPassTiming& pass : render.gpu) {
            if (!pass.step) {
                recording.push_back({{"name", pass.name}, {"cpuMs", pass.cpuMilliseconds}});
            }
        }
        json["gpu"] = {{"totalMs", passes.empty() ? -1.0 : gpuTotal}, {"passes", std::move(passes)}};
        json["recording"] = {{"parallel", renderer.IsParallelRecording()}, {"lists", render.recordLists},
                             {"passes", std::move(recording)}};
        Json exposure = {{"ev100", ExposureToEv100(render.exposure)}, {"exposure", render.exposure}};
        // 0 : exposition fixe, ou rien de mesurable (image noire).
        exposure["measuredLuminanceNits"] = render.sceneLuminance * Photometry::NitsPerUnit;
        json["exposure"] = std::move(exposure);
        json["scene"] = {{"objectsTested", render.occlusionTested},
                         {"objectsHidden", render.occlusionHidden},
                         {"modelMeshShaders", render.modelMeshShaders},
                         {"modelMeshlets", render.modelMeshlets},
                         {"modelMeshletsDrawn", render.modelMeshletsDrawn},
                         {"modelsLoading", render.modelsLoading},
                         {"planetTiles", render.planetTiles},
                         {"planetOceanTiles", render.planetOceanTiles},
                         {"planetStoredTiles", render.planetStoredTiles},
                         {"planetTileCapacity", render.planetTileCapacity},
                         {"planetPendingTiles", render.planetPendingTiles},
                         {"groundMeshThreads", render.groundMeshThreads},
                         {"trees", {{"near", render.trees[0]}, {"simplified", render.trees[1]}, {"far", render.trees[2]}}},
                         {"shadowTrees", render.shadowTrees},
                         {"treePlains", render.treePlains},
                         {"pendingTreePlains", render.pendingTreePlains},
                         {"aheadTreePlains", render.aheadTreePlains}};
        const Platform::ProcessMemory memory = Platform::GetProcessMemory();
        const auto [gpuDevice, gpuHost] = renderer.GetGpuMemory();
        constexpr double Megabyte = 1024.0 * 1024.0;
        json["memoryMb"] = {{"ram", static_cast<double>(memory.workingSet) / Megabyte},
                            {"ramReserved", static_cast<double>(memory.privateBytes) / Megabyte},
                            {"video", static_cast<double>(gpuDevice) / Megabyte},
                            {"videoInRam", static_cast<double>(gpuHost) / Megabyte}};
        const Math::Vector2i renderSize = renderer.GetRenderSize();
        json["renderer"] = {{"gpu", renderer.GetDeviceName()},
                            {"vulkan", renderer.GetApiVersion()},
                            {"presentMode", renderer.GetPresentModeName()},
                            {"vsync", renderer.IsVSyncEnabled()},
                            {"renderSize", {renderSize.x, renderSize.y}},
                            // Mode demandé ; un mode FSR sans FSR disponible tourne en Natif.
                            {"upscaling", UpscalingKeys[static_cast<std::size_t>(renderer.GetUpscaling())]},
                            {"fsrAvailable", renderer.IsUpscalingAvailable()},
                            {"temporalAA", renderer.IsTemporalAAEnabled()},
                            {"occlusionCulling", renderer.IsOcclusionCullingEnabled()},
                            {"bloom", renderer.GetBloom()}};
        return json.dump(1);
    }
}
