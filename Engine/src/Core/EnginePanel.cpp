#include "Core/EnginePanel.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/FlyCameraController.hpp"
#include "Flux/Platform/Memory.hpp"
#include "Flux/UI/Context.hpp"
#include "Core/Measurements.hpp"

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
                ui.LabelText("  ombres, plaines", std::format("{}, {} (+{} à semer)", render.shadowTrees, render.treePlains,
                                                             render.pendingTreePlains));
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
}
