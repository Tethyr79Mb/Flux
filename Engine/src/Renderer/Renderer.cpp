#include "Flux/Renderer/Renderer.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/MeshRenderer.hpp"
#include "Renderer/OverlayRenderer.hpp"
#include "Planetology/PlanetTileCompute.hpp"
#include "Renderer/PostRenderer.hpp"
#include "Renderer/RHI/RHI.hpp"
#include "Renderer/RenderGraph.hpp"
#include "Renderer/SkyRenderer.hpp"
#include "Renderer/TemporalView.hpp"
#include "Renderer/ThrusterRenderer.hpp"
#include "Renderer/TracerRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Flux {
    namespace {
        // Profondeur 32 bits flottante : obligatoire pour que le reverse-Z tienne l'échelle galaxie.
        constexpr RHI::Format DepthFormat = RHI::Format::D32Float;

        // Décalages d'un sous-pixel de l'anticrénelage : 8 points de la suite de Halton (bases 2 et 3), bien répartis
        // dans le pixel, puis on recommence. Plus de points lisseraient un peu mieux les bords presque droits, au prix
        // d'un scintillement plus lent à disparaître.
        constexpr std::uint32_t JitterPhases = 8;
        // Au-delà (m) d'une image à l'autre, la caméra a sauté (changement de scène, téléportation) : rien à reprendre.
        // En vol à 100 km/s, elle avance de 2 km par image : la reprojection suit encore.
        constexpr double MaxTemporalJump = 1.0e5;

        float Halton(std::uint32_t index, std::uint32_t base) {
            float result = 0.0f;
            float fraction = 1.0f;
            for (; index > 0; index /= base) {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(index % base);
            }
            return result;
        }

        // Inverse de la vue et de la projection (reverse-Z infini) de Camera, sans inversion générale : la vue est une
        // rotation (sa transposée), la projection s'inverse terme à terme.
        Math::Matrix4f InverseViewProjection(const Camera& camera, float aspect) {
            const float f = 1.0f / std::tan(0.5f * camera.verticalFov);
            Math::Matrix4f inverseProjection;
            inverseProjection.columns[0] = {aspect / f, 0.0f, 0.0f, 0.0f};
            inverseProjection.columns[1] = {0.0f, -1.0f / f, 0.0f, 0.0f};
            inverseProjection.columns[2] = {0.0f, 0.0f, 0.0f, 1.0f / camera.nearPlane};
            inverseProjection.columns[3] = {0.0f, 0.0f, -1.0f, 0.0f};
            const Math::Vector3f right = camera.GetRight();
            const Math::Vector3f up = camera.GetUp();
            const Math::Vector3f forward = camera.GetForward();
            Math::Matrix4f inverseView;
            inverseView.columns[0] = {right.x, right.y, right.z, 0.0f};
            inverseView.columns[1] = {up.x, up.y, up.z, 0.0f};
            inverseView.columns[2] = {-forward.x, -forward.y, -forward.z, 0.0f};
            inverseView.columns[3] = {0.0f, 0.0f, 0.0f, 1.0f};
            return inverseView * inverseProjection;
        }

        // Constantes de la grille au sol (plan y = 0 du monde, secteur 0) vue de la caméra.
        PostRenderer::GridParams MakeGridParams(const Camera& camera, float aspect, const Math::Matrix4f& viewProjection) {
            // Le motif se répète tous les 100 m : la caméra modulo 1000 m garde la précision des float loin de l'origine.
            const Math::Vector3d position = Math::Delta(Math::WorldPosition{}, camera.position);
            const auto pattern = [](double value) { return static_cast<float>(std::fmod(value, 1000.0)); };
            // Les axes ne se voient que près de l'origine : au-delà, leur position peut perdre en précision.
            const auto axis = [](double value) { return static_cast<float>(std::clamp(value, -1.0e7, 1.0e7)); };
            const float height = static_cast<float>(position.y);
            const std::array<std::array<float, 4>, 4>& c = viewProjection.columns;
            PostRenderer::GridParams params{};
            params.inverseViewProjection = InverseViewProjection(camera, aspect);
            params.depthRow = {c[0][2], c[1][2], c[2][2], c[3][2]};
            params.wRow = {c[0][3], c[1][3], c[2][3], c[3][3]};
            params.camera = {pattern(position.x), height, pattern(position.z), 0.0f};
            // Plus la caméra est haute, plus loin la grille reste visible.
            params.axes = {axis(position.x), std::max(150.0f, 40.0f * std::abs(height)), axis(position.z), 0.0f};
            return params;
        }

        using Clock = std::chrono::steady_clock;
        double Milliseconds(Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        }
    }

    // Une image : la carte d'ombres, puis la scène (opaques, fond du ciel et soleil, océan, nuages, transparents, flammes)
    // dans une image HDR à la taille de sa zone, son anticrénelage, son bloom, et la passe principale vers la fenêtre : la scène avec la
    // courbe de tons dans sa zone (PostRenderer), puis l'interface (OverlayRenderer) sur toute la fenêtre. Aucune API
    // graphique ici : tout passe par le RHI.
    struct Renderer::Impl {
        Window& window;
        // Déclaré en premier : détruit en dernier, après les ressources créées par lui.
        std::unique_ptr<RHI::Device> device;
        std::unique_ptr<RHI::Texture> depth; // de la taille de la swapchain : passe principale (interface)
        std::unique_ptr<SkyRenderer> sky;
        std::unique_ptr<MeshRenderer> meshes;
        std::unique_ptr<ThrusterRenderer> thrusters;
        std::unique_ptr<TracerRenderer> tracers;
        std::span<const Projectile> projectiles; // tableaux de l'appelant (Renderer::SetProjectiles)
        std::span<const Impact> impacts;
        std::unique_ptr<OverlayRenderer> overlay;
        std::unique_ptr<PostRenderer> post;
        std::filesystem::path assetsDirectory;
        Math::LinearColor clearColor{0.02f, 0.02f, 0.05f, 1.0f};
        bool vsync = true;
        float bloom = Renderer::DefaultBloom; // 0 : sans bloom
        bool temporalAA = true;
        bool detailedTimings = false; // heures du GPU demandées pour cette image seulement
        // Anticrénelage : numéro de l'image (décalage, bruits) et caméra de l'image précédente.
        std::uint32_t temporalFrame = 0;
        bool hasPrevious = false;
        Math::Matrix4f previousViewProjection;
        Math::WorldPosition previousPosition;
        Math::Vector2f sceneViewportMin; // en pixels ; vide : toute la fenêtre
        Math::Vector2f sceneViewportMax;
        bool sceneVisible = true; // false : seule l'interface est dessinée (éditeur d'interface)
        bool grid = false;        // grille au sol de l'éditeur
        RenderTimings timings;
        // L'image décrite une fois (BuildGraph), jouée à chaque RenderFrame avec l'état de l'image ci-dessous.
        RenderGraph graph;
        struct FrameState
        {
            std::uint32_t index = 0;
            RHI::Rect scene3d;
            float exposure = 1.0f;
            TemporalView temporal;
            Math::Matrix4f reprojection = Math::Matrix4f::Identity();
            PostRenderer::GridParams gridParams{};
        };
        FrameState frame;

        explicit Impl(Window& w) : window(w) {}

        ~Impl() {
            if (device) {
                device->WaitIdle(); // les ressources ci-dessous ne servent plus quand elles sont détruites
            }
        }

        RHI::Extent WindowExtent() const {
            const Math::Vector2i size = window.GetSize();
            return {static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)};
        }

        // Tampon de profondeur à la taille de la swapchain (recréée quand la fenêtre change de taille).
        std::expected<void, std::string> UpdateDepth() {
            const RHI::Extent extent = device->GetSwapchainExtent();
            if (depth && depth->GetExtent() == extent) {
                return {};
            }
            device->WaitIdle(); // l'image précédente l'utilise peut-être encore
            depth.reset(); // libère l'ancien avant d'allouer le nouveau
            auto created = device->CreateRenderTarget(DepthFormat, extent);
            if (!created) {
                return std::unexpected(created.error());
            }
            depth = std::move(*created);
            return {};
        }

        // Rectangle de la scène (en pixels entiers), borné à la fenêtre : la vue 3D de l'éditeur, ou toute la fenêtre.
        RHI::Rect SceneRect() const {
            const RHI::Extent extent = device->GetSwapchainExtent();
            const auto clampTo = [](float value, std::uint32_t limit) {
                return static_cast<std::int32_t>(std::clamp(std::round(value), 0.0f, static_cast<float>(limit)));
            };
            const std::int32_t x0 = clampTo(sceneViewportMin.x, extent.width);
            const std::int32_t y0 = clampTo(sceneViewportMin.y, extent.height);
            const std::int32_t x1 = clampTo(sceneViewportMax.x, extent.width);
            const std::int32_t y1 = clampTo(sceneViewportMax.y, extent.height);
            if (x1 - x0 < 1 || y1 - y0 < 1) {
                return {0, 0, extent.width, extent.height};
            }
            return {x0, y0, static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)};
        }

        bool RenderFrame(const Scene& scene, const Camera& camera, Entity hidden) {
            const RHI::Extent extent = WindowExtent();
            if (extent.width == 0 || extent.height == 0) {
                return true; // fenêtre réduite : rien à présenter
            }
            if (!device->ResizeSwapchain(extent, vsync)) {
                return false;
            }
            if (auto updated = UpdateDepth(); !updated) {
                Log::Error("{}", updated.error());
                return false;
            }

            switch (device->BeginFrame()) {
                case RHI::FrameStatus::Ready: break;
                case RHI::FrameStatus::Skip: return true;
                case RHI::FrameStatus::Error: return false;
            }
            const Clock::time_point prepareStart = Clock::now();
            ReadTimings();
            const std::uint32_t frameIndex = device->GetFrameIndex();
            timings.fontsLoading = overlay->GetFontsLoading();
            if (!overlay->Upload(frameIndex)) {
                return false;
            }
            const RHI::Rect scene3d = SceneRect();
            const float aspect = static_cast<float>(scene3d.width) / static_cast<float>(scene3d.height);
            float exposure = 1.0f;
            const Math::Matrix4f viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
            TemporalView temporal;
            Math::Matrix4f reprojection = Math::Matrix4f::Identity();
            if (sceneVisible) {
                const Math::Vector3d moved = Math::Delta(previousPosition, camera.position);
                temporal.continuous = hasPrevious && Math::Length(moved) < MaxTemporalJump;
                temporal.history = temporalAA && temporal.continuous;
                if (temporal.continuous) {
                    temporal.previousViewProjection = previousViewProjection;
                    temporal.cameraDelta = Math::Vector3f(moved);
                    // Un pixel et sa profondeur -> relatif à la caméra -> relatif à l'ancienne -> son clip d'alors.
                    reprojection = previousViewProjection * Math::Matrix4f::Translation(temporal.cameraDelta)
                                 * InverseViewProjection(camera, aspect);
                }
                if (temporalAA) {
                    ++temporalFrame;
                    const std::uint32_t phase = temporalFrame % JitterPhases + 1; // Halton(0) vaut 0 : sauté
                    temporal.jitter = {(2.0f * Halton(phase, 2) - 1.0f) / static_cast<float>(scene3d.width),
                                       (2.0f * Halton(phase, 3) - 1.0f) / static_cast<float>(scene3d.height)};
                    temporal.noiseFrame = temporalFrame;
                }
            }
            hasPrevious = sceneVisible;
            previousViewProjection = viewProjection;
            previousPosition = camera.position;
            if (sceneVisible) {
                if (auto resized = post->Resize({scene3d.width, scene3d.height}); !resized) {
                    Log::Error("{}", resized.error());
                    return false;
                }
                const SkyLighting lighting = sky->Prepare(scene, camera, aspect, static_cast<float>(scene3d.height),
                                                          assetsDirectory, frameIndex);
                exposure = lighting.exposure;
                meshes->Prepare(scene, camera, aspect, hidden, lighting, temporal, assetsDirectory, frameIndex);
                thrusters->Prepare(meshes->GetThrusters(), camera, aspect, temporal.jitter);
                tracers->Prepare(projectiles, impacts, camera, aspect, static_cast<float>(scene3d.height), temporal.jitter, frameIndex);
                const OcclusionCulling::Stats occlusion = meshes->GetOcclusionStats();
                timings.occlusionTested = occlusion.tested;
                timings.occlusionHidden = occlusion.hidden;
                const MeshRenderer::PlanetTileStats tiles = meshes->GetPlanetTileStats();
                timings.planetTiles = tiles.drawn;
                timings.planetOceanTiles = tiles.ocean;
                timings.planetStoredTiles = tiles.stored;
                timings.planetPendingTiles = tiles.pending;
                timings.groundTexturesLoading = tiles.groundLoading;
                timings.modelsLoading = static_cast<std::uint32_t>(meshes->GetModels().GetLoadingCount());
                timings.planetTileCapacity = Planetology::PlanetTileCompute::MaxPages * Planetology::PlanetTileCompute::TilesPerPage;
                using Planetology::VegetationRenderer;
                const VegetationRenderer::Stats vegetation = meshes->GetVegetationStats();
                timings.trees = {vegetation.trees[VegetationRenderer::Near], vegetation.trees[VegetationRenderer::Middle],
                                 vegetation.trees[VegetationRenderer::Far]};
                timings.shadowTrees = 0;
                for (std::uint32_t c = 0; c < ShadowCascadeCount; ++c) {
                    timings.shadowTrees += vegetation.trees[VegetationRenderer::Shadow + c];
                }
                timings.treePlains = vegetation.plains;
                timings.pendingTreePlains = vegetation.pending;
            }

            const Clock::time_point recordStart = Clock::now();
            timings.prepare = Milliseconds(recordStart - prepareStart);
            frame.index = frameIndex;
            frame.scene3d = scene3d;
            frame.exposure = exposure;
            frame.temporal = temporal;
            frame.reprojection = reprojection;
            if (sceneVisible && grid) {
                frame.gridParams = MakeGridParams(camera, aspect, viewProjection);
            }
            graph.Execute(device->GetCommandList(), std::exchange(detailedTimings, false));
            const bool presented = device->EndFrame();
            // Présentation exclue : mesurée à part par le RHI.
            timings.record = Milliseconds(Clock::now() - recordStart) - device->GetWaitTimes().present;
            timings.present = device->GetWaitTimes().present;
            return presented;
        }

        // Attentes de BeginFrame et heures du GPU relues pour l'image qu'il vient de finir.
        void ReadTimings() {
            const RHI::WaitTimes& waits = device->GetWaitTimes();
            timings.waitGpu = waits.frame;
            timings.waitImage = waits.acquire;
            timings.gpu = graph.ReadTimings(device->GetTimestamps());
        }

        // L'image, dans l'ordre : calculs du GPU (tuiles, arbres, objets cachés), textures du ciel, carte d'ombres,
        // scène HDR, pyramide de profondeur, anticrénelage, bloom, puis l'image de la fenêtre (scène avec la courbe de
        // tons, grille, interface). Sans scène (éditeur d'interface), seule l'interface.
        std::expected<void, std::string> BuildGraph() {
            using Lifetime = RenderGraph::Lifetime;
            const auto cpu = graph.AddResource("données envoyées par le CPU", Lifetime::External);
            // Gardées d'une image à l'autre : tuiles calculées une fois, ciel recalculé quand il change, pyramide et
            // historique de l'image précédente.
            const auto tiles = graph.AddResource("tuiles de planète", Lifetime::Previous);
            const auto skyCube = graph.AddResource("textures du ciel", Lifetime::Previous);
            const auto pyramid = graph.AddResource("pyramide de profondeur", Lifetime::Previous);
            const auto history = graph.AddResource("historique de l'anticrénelage", Lifetime::Previous);
            const auto culled = graph.AddResource("arbres et objets retenus");
            const auto shadowMap = graph.AddResource("carte d'ombres");
            const auto sceneColor = graph.AddResource("image de la scène");
            const auto motion = graph.AddResource("mouvement");
            const auto sceneDepth = graph.AddResource("profondeur de la scène");
            const auto bloomTextures = graph.AddResource("bloom");
            const auto windowImage = graph.AddResource("image de la fenêtre");
            const auto scene = [this] { return sceneVisible; };

            graph.AddPass("calculs")
                .When(scene)
                .Reads(cpu)
                .Reads(pyramid)
                .Writes(tiles)
                .Writes(culled)
                .Step("calculs", [this](RHI::CommandList& commands) { meshes->Compute(commands, frame.index); });
            graph.AddPass("ciel")
                .When(scene)
                .Reads(cpu)
                .Writes(skyCube)
                .Step("ciel", [this](RHI::CommandList& commands) { sky->Bake(commands, frame.index); });
            graph.AddPass("ombres")
                .When(scene)
                .Reads(cpu)
                .Reads(tiles)
                .Reads(culled)
                .Writes(shadowMap)
                .Step("ombres", [this](RHI::CommandList& commands) { meshes->DrawShadows(commands, frame.index); });
            graph.AddPass("scène")
                .When(scene)
                .Reads(cpu)
                .Reads(tiles)
                .Reads(culled)
                .Reads(shadowMap)
                .Reads(skyCube)
                .Writes(sceneColor)
                .Writes(motion)
                .Writes(sceneDepth)
                .Begin([this](RHI::CommandList& commands) {
                    // Fond sans ciel : la couleur choisie doit ressortir telle quelle de la courbe de tons.
                    // Profondeur et mouvement gardés pour l'anticrénelage.
                    commands.BeginRenderPass({.clearColor = PostRenderer::InverseToneMap(clearColor, frame.exposure),
                                              .depth = &post->GetSceneDepth(), .clearDepth = 0.0f,
                                              .colorTarget = &post->GetSceneTarget(),
                                              .secondColorTarget = &post->GetMotionTarget(), .keepDepth = true});
                    commands.SetViewport({0.0f, 0.0f, static_cast<float>(frame.scene3d.width), static_cast<float>(frame.scene3d.height)});
                    commands.SetScissor({0, 0, frame.scene3d.width, frame.scene3d.height});
                })
                .Step("objets", [this](RHI::CommandList& commands) { meshes->DrawOpaque(commands, frame.index); })
                .Step("arbres", [this](RHI::CommandList& commands) { meshes->DrawVegetation(commands, frame.index); })
                .Step("sol", [this](RHI::CommandList& commands) { meshes->DrawGround(commands, frame.index); })
                .Step("fond du ciel", [this](RHI::CommandList& commands) { sky->Draw(commands, frame.index); })
                .Step("océan", [this](RHI::CommandList& commands) { meshes->DrawOcean(commands, frame.index); })
                .Step("nuages", [this](RHI::CommandList& commands) { meshes->DrawClouds(commands, frame.index); })
                .Step("transparents", [this](RHI::CommandList& commands) {
                    meshes->DrawTransparent(commands, frame.index);
                    commands.BeginStep("flammes des réacteurs");
                    thrusters->Draw(commands);
                    commands.BeginStep("traçantes");
                    tracers->Draw(commands);
                });
            // La profondeur de la scène réduite, pour écarter à l'image suivante ce qui sera caché.
            graph.AddPass("pyramide")
                .When(scene)
                .Reads(sceneDepth)
                .Writes(pyramid)
                .Step("pyramide", [this](RHI::CommandList& commands) {
                    meshes->BuildDepthPyramid(commands, frame.index, post->GetSceneDepth(), {frame.scene3d.width, frame.scene3d.height});
                });
            graph.AddPass("anticrénelage")
                .When(scene)
                .Reads(sceneColor)
                .Reads(motion)
                .Reads(sceneDepth)
                .Reads(history)
                .Writes(history)
                .Step("anticrénelage", [this](RHI::CommandList& commands) {
                    post->ResolveTemporal(commands, temporalAA, frame.reprojection, frame.temporal.history);
                });
            graph.AddPass("bloom")
                .When(scene)
                .Reads(sceneColor)
                .Reads(history)
                .Writes(bloomTextures)
                .Step("bloom", [this](RHI::CommandList& commands) { post->DrawBloom(commands, bloom); });
            graph.AddPass("image finale")
                .Reads(sceneColor)
                .Reads(history)
                .Reads(bloomTextures)
                .Reads(sceneDepth)
                .Writes(windowImage)
                .Begin([this](RHI::CommandList& commands) {
                    commands.BeginRenderPass({.clearColor = clearColor, .depth = depth.get(), .clearDepth = 0.0f});
                })
                .Step("scène", [this](RHI::CommandList& commands) {
                    commands.SetViewport({static_cast<float>(frame.scene3d.x), static_cast<float>(frame.scene3d.y),
                                          static_cast<float>(frame.scene3d.width), static_cast<float>(frame.scene3d.height)});
                    commands.SetScissor(frame.scene3d);
                    post->Composite(commands, bloom, frame.exposure);
                }, scene)
                // Même viewport que la scène.
                .Step("grille", [this](RHI::CommandList& commands) { post->DrawGrid(commands, frame.gridParams); },
                      [this] { return sceneVisible && grid; })
                .Step("interface", [this](RHI::CommandList& commands) {
                    overlay->Draw(commands, frame.index, device->GetSwapchainExtent());
                });
            return graph.Compile();
        }
    };

    std::expected<std::unique_ptr<Renderer>, std::string> Renderer::Create(Window& window) {
        auto impl = std::make_unique<Impl>(window);

        auto device = RHI::Device::Create(window.GetNativeHandle(), impl->WindowExtent(), impl->vsync);
        if (!device) {
            return std::unexpected(device.error());
        }
        impl->device = std::move(*device);
        if (auto depth = impl->UpdateDepth(); !depth) {
            return std::unexpected(depth.error());
        }
        const RHI::Format colorFormat = impl->device->GetSwapchainFormat();
        // La scène est dessinée en HDR (PostRenderer) ; seules l'image finale et l'interface vont vers la fenêtre.
        constexpr RHI::Format sceneFormat = PostRenderer::SceneFormat;
        constexpr RHI::Format motionFormat = PostRenderer::MotionFormat;
        auto sky = SkyRenderer::Create(*impl->device, sceneFormat, motionFormat, DepthFormat);
        if (!sky) {
            return std::unexpected(sky.error());
        }
        impl->sky = std::move(*sky);
        auto meshes = MeshRenderer::Create(*impl->device, sceneFormat, motionFormat, DepthFormat);
        if (!meshes) {
            return std::unexpected(meshes.error());
        }
        impl->meshes = std::move(*meshes);
        auto thrusters = ThrusterRenderer::Create(*impl->device, sceneFormat, motionFormat, DepthFormat);
        if (!thrusters) {
            return std::unexpected(thrusters.error());
        }
        impl->thrusters = std::move(*thrusters);
        auto tracers = TracerRenderer::Create(*impl->device, sceneFormat, motionFormat, DepthFormat);
        if (!tracers) {
            return std::unexpected(tracers.error());
        }
        impl->tracers = std::move(*tracers);
        auto overlay = OverlayRenderer::Create(*impl->device, colorFormat, DepthFormat);
        if (!overlay) {
            return std::unexpected(overlay.error());
        }
        impl->overlay = std::move(*overlay);
        auto post = PostRenderer::Create(*impl->device, colorFormat, DepthFormat);
        if (!post) {
            return std::unexpected(post.error());
        }
        impl->post = std::move(*post);
        if (auto graph = impl->BuildGraph(); !graph) {
            return std::unexpected(graph.error());
        }

        return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
    }

    Renderer::Renderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    Renderer::~Renderer() = default;

    bool Renderer::RenderFrame(const Scene& scene, const Camera& camera, Entity hidden) {
        const bool ok = m_impl->RenderFrame(scene, camera, hidden);
        m_impl->overlay->GetDrawList().Clear(); // même si l'image n'a pas été présentée (fenêtre réduite, swapchain recréée)
        return ok;
    }

    void Renderer::SetSceneViewport(Math::Vector2f min, Math::Vector2f max) {
        m_impl->sceneViewportMin = min;
        m_impl->sceneViewportMax = max;
    }

    void Renderer::SetSceneVisible(bool visible) {
        m_impl->sceneVisible = visible;
    }

    void Renderer::SetThrusterPreview(bool preview) {
        m_impl->meshes->SetThrusterPreview(preview);
    }

    void Renderer::SetProjectiles(std::span<const Projectile> projectiles, std::span<const Impact> impacts) {
        m_impl->projectiles = projectiles;
        m_impl->impacts = impacts;
    }

    void Renderer::SetClearColor(Math::LinearColor color) {
        m_impl->clearColor = color;
    }

    void Renderer::SetVSync(bool enabled) {
        m_impl->vsync = enabled; // swapchain recréée au début de la prochaine image
    }

    bool Renderer::IsVSyncEnabled() const {
        return m_impl->vsync;
    }

    void Renderer::SetBloom(float strength) {
        m_impl->bloom = std::clamp(strength, 0.0f, 1.0f);
    }

    float Renderer::GetBloom() const {
        return m_impl->bloom;
    }

    void Renderer::RequestDetailedTimings() {
        m_impl->detailedTimings = true;
    }

    void Renderer::SetTemporalAA(bool enabled) {
        m_impl->temporalAA = enabled;
    }

    bool Renderer::IsTemporalAAEnabled() const {
        return m_impl->temporalAA;
    }

    void Renderer::SetGridVisible(bool visible) {
        m_impl->grid = visible;
    }

    bool Renderer::IsGridVisible() const {
        return m_impl->grid;
    }

    void Renderer::SetOcclusionCulling(bool enabled) {
        m_impl->meshes->SetOcclusionCulling(enabled);
    }

    bool Renderer::IsOcclusionCullingEnabled() const {
        return m_impl->meshes->IsOcclusionCullingEnabled();
    }

    void Renderer::SetGroundGeometryOnly(bool enabled) {
        m_impl->meshes->SetGroundGeometryOnly(enabled);
    }

    bool Renderer::IsGroundGeometryOnly() const {
        return m_impl->meshes->IsGroundGeometryOnly();
    }

    std::string_view Renderer::GetPresentModeName() const {
        switch (m_impl->device->GetPresentMode()) {
            case RHI::PresentMode::Fifo: return "FIFO (synchronisé)";
            case RHI::PresentMode::FifoRelaxed: return "FIFO relaxed";
            case RHI::PresentMode::Mailbox: return "MAILBOX (sans déchirure)";
            case RHI::PresentMode::Immediate: return "IMMEDIATE (déchirure possible)";
            case RHI::PresentMode::Other: break;
        }
        return "autre";
    }

    std::optional<UI::TextureInfo> Renderer::LoadTexture(const std::filesystem::path& path) {
        return m_impl->overlay->LoadTexture(path);
    }

    std::optional<UI::TextureInfo> Renderer::CreateTexture(const ImageData& image) {
        return m_impl->overlay->CreateTexture(image);
    }

    std::optional<UI::FontRef> Renderer::LoadFont(const std::filesystem::path& path) {
        return m_impl->overlay->LoadFont(path);
    }

    void Renderer::SetAssetsDirectory(const std::filesystem::path& directory) {
        if (directory != m_impl->assetsDirectory) {
            m_impl->assetsDirectory = directory;
        }
    }

    std::optional<Math::Box3f> Renderer::LoadModel(const std::filesystem::path& path) {
        if (const GpuModel* model = m_impl->meshes->GetModels().Find(path)) {
            return model->bounds;
        }
        return std::nullopt;
    }

    std::vector<std::string> Renderer::GetAnimationNames(const std::filesystem::path& path) {
        std::vector<std::string> names;
        if (const GpuModel* model = m_impl->meshes->GetModels().Find(path)) {
            for (const ModelAnimation& animation : model->skeleton.animations) {
                names.push_back(animation.name);
            }
        }
        return names;
    }

    float Renderer::GetAnimationDuration(const std::filesystem::path& path, std::string_view name) {
        const GpuModel* model = m_impl->meshes->GetModels().Find(path);
        const ModelAnimation* animation = model ? FindAnimation(model->skeleton, name) : nullptr;
        return animation ? animation->duration : 0.0f;
    }

    UI::DrawList& Renderer::GetOverlay() {
        return m_impl->overlay->GetDrawList();
    }

    const RenderTimings& Renderer::GetTimings() const {
        return m_impl->timings;
    }

    std::string_view Renderer::GetDeviceName() const {
        return m_impl->device->GetName();
    }

    std::string_view Renderer::GetApiVersion() const {
        return m_impl->device->GetApiVersion();
    }

    std::pair<std::uint64_t, std::uint64_t> Renderer::GetGpuMemory() const {
        const RHI::MemoryUsage usage = m_impl->device->GetMemoryUsage();
        return {usage.device, usage.host};
    }
}
