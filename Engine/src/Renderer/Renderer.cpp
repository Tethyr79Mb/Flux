#include "Flux/Renderer/Renderer.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/MeshRenderer.hpp"
#include "Renderer/OverlayRenderer.hpp"
#include "Planetology/CloudRenderer.hpp"
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
        // Durée d'une image annoncée à FSR quand il n'y a pas d'image précédente (ms).
        constexpr float DefaultFrameMilliseconds = 1000.0f / 60.0f;
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
    // dans une image HDR à la taille de rendu (sa zone, ou moins quand FSR l'agrandit), son anticrénelage ou FSR, son
    // bloom, et la passe principale vers la fenêtre : la scène avec la
    // courbe de tons dans sa zone (PostRenderer), puis l'interface (OverlayRenderer) sur toute la fenêtre. Aucune API
    // graphique ici : tout passe par le RHI.
    struct Renderer::Impl {
        Window& window;
        // Déclaré en premier : détruit en dernier, après les ressources créées par lui.
        std::unique_ptr<RHI::Device> device;
        std::unique_ptr<SkyRenderer> sky;
        std::unique_ptr<MeshRenderer> meshes;
        std::unique_ptr<ThrusterRenderer> thrusters;
        std::unique_ptr<TracerRenderer> tracers;
        std::span<const Projectile> projectiles; // tableaux de l'appelant (Renderer::SetProjectiles)
        std::span<const Impact> impacts;
        std::unique_ptr<OverlayRenderer> overlay;
        std::unique_ptr<PostRenderer> post;
        std::unique_ptr<Planetology::CloudRenderer> clouds;
        // FSR : créé pour la taille de la vue quand un mode FSR est choisi, libéré en Natif (sa mémoire vidéo).
        std::unique_ptr<RHI::Upscaler> upscaler;
        RHI::Extent upscalerExtent;
        bool upscalerFailed = false; // DLL absente ou GPU refusé : Natif jusqu'à la fermeture, dit une fois
        std::filesystem::path assetsDirectory;
        Math::LinearColor clearColor{0.02f, 0.02f, 0.05f, 1.0f};
        bool vsync = true;
        float bloom = Renderer::DefaultBloom; // 0 : sans bloom
        bool temporalAA = true;
        Upscaling upscaling = DefaultUpscaling;
        float sharpness = DefaultSharpness;
        RHI::Extent renderExtent; // de la dernière image
        Clock::time_point previousFrame;
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
            RHI::Extent render; // taille de rendu
            float exposure = 1.0f;
            TemporalView temporal;
            Math::Matrix4f reprojection = Math::Matrix4f::Identity();
            PostRenderer::GridParams gridParams{};
            std::uint32_t cloudCopy = 0; // exemplaire de l'historique des nuages écrit à cette image
            bool fsr = false;           // FSR agrandit cette image (sinon anticrénelage, ou rien)
            RHI::UpscaleInputs upscale; // ses paramètres, textures remplies par sa passe
        };
        FrameState frame;
        // Textures de travail du graphe.
        struct Targets
        {
            RenderGraph::ResourceId sceneColor = 0;
            RenderGraph::ResourceId motion = 0;
            RenderGraph::ResourceId fullMotion = 0;
            RenderGraph::ResourceId sceneDepth = 0;
            RenderGraph::ResourceId history = 0;
            RenderGraph::ResourceId windowDepth = 0;
            RenderGraph::ResourceId cloudLight = 0;
            RenderGraph::ResourceId cloudDistances = 0;
            RenderGraph::ResourceId cloudHistory = 0;
        };
        Targets targets;

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

        // Textures de la vue 3D, de rendu et de sa moitié (nuages) à leur taille ; recréées, PostRenderer et
        // CloudRenderer refont leurs groupes.
        std::expected<void, std::string> ResizeViewTargets(RHI::Extent view, RHI::Extent render) {
            auto viewResized = graph.ResizeTextures(*device, RenderGraph::TextureSize::View, view);
            if (!viewResized) {
                return std::unexpected(viewResized.error());
            }
            auto renderResized = graph.ResizeTextures(*device, RenderGraph::TextureSize::Render, render);
            if (!renderResized) {
                return std::unexpected(renderResized.error());
            }
            auto halfResized = graph.ResizeTextures(*device, RenderGraph::TextureSize::HalfView,
                                                    {(render.width + 1) / 2, (render.height + 1) / 2});
            if (!halfResized) {
                return std::unexpected(halfResized.error());
            }
            if (!*viewResized && !*renderResized && !*halfResized) {
                return {};
            }
            clouds->Bind({.sceneDepth = graph.GetTexture(targets.sceneDepth),
                          .light = graph.GetTexture(targets.cloudLight),
                          .distances = graph.GetTexture(targets.cloudDistances),
                          .history = {graph.GetCopy(targets.cloudHistory, 0), graph.GetCopy(targets.cloudHistory, 1)}});
            return post->Bind({.scene = graph.GetTexture(targets.sceneColor),
                               .depth = graph.GetTexture(targets.sceneDepth),
                               .motion = graph.GetTexture(targets.motion),
                               .fullMotion = graph.GetTexture(targets.fullMotion),
                               .history = {graph.GetCopy(targets.history, 0), graph.GetCopy(targets.history, 1)}});
        }

        // FSR prêt pour une vue de `view` si un mode FSR est choisi (créé, ou recréé quand la vue change de taille),
        // libéré sinon. false : Natif à cette image.
        bool PrepareUpscaler(RHI::Extent view) {
            const bool wanted = IsFsr(upscaling) && !upscalerFailed;
            if (upscaler && (!wanted || upscalerExtent != view)) {
                device->WaitIdle(); // les images en vol s'en servent peut-être encore
                upscaler.reset();
            }
            if (!wanted || upscaler) {
                return wanted;
            }
            auto created = device->CreateUpscaler(view);
            if (!created) {
                Log::Warning("FSR indisponible ({}) : rendu en Natif", created.error());
                upscalerFailed = true;
                return false;
            }
            upscaler = std::move(*created);
            upscalerExtent = view;
            return true;
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
            // Profondeur de l'image finale à la taille de la swapchain (recréée quand la fenêtre change de taille).
            if (auto resized = graph.ResizeTextures(*device, RenderGraph::TextureSize::Window, device->GetSwapchainExtent()); !resized) {
                Log::Error("{}", resized.error());
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
            const RHI::Extent view{scene3d.width, scene3d.height};
            const bool fsr = sceneVisible && PrepareUpscaler(view);
            // Comme FSR la calcule : la vue divisée par le rapport du mode, arrondie en dessous.
            const float scale = fsr ? GetRenderScale(upscaling) : 1.0f;
            const RHI::Extent render{std::max(1u, static_cast<std::uint32_t>(static_cast<float>(view.width) * scale)),
                                     std::max(1u, static_cast<std::uint32_t>(static_cast<float>(view.height) * scale))};
            const Clock::time_point now = Clock::now();
            const float frameMilliseconds = previousFrame == Clock::time_point{}
                                                ? DefaultFrameMilliseconds
                                                : static_cast<float>(Milliseconds(now - previousFrame));
            previousFrame = now;
            const float aspect = static_cast<float>(scene3d.width) / static_cast<float>(scene3d.height);
            float exposure = 1.0f;
            const Math::Matrix4f viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
            TemporalView temporal;
            Math::Matrix4f reprojection = Math::Matrix4f::Identity();
            if (sceneVisible) {
                const Math::Vector3d moved = Math::Delta(previousPosition, camera.position);
                temporal.continuous = hasPrevious && Math::Length(moved) < MaxTemporalJump;
                temporal.history = (temporalAA || fsr) && temporal.continuous;
                if (temporal.continuous) {
                    temporal.previousViewProjection = previousViewProjection;
                    temporal.cameraDelta = Math::Vector3f(moved);
                    // Un pixel et sa profondeur -> relatif à la caméra -> relatif à l'ancienne -> son clip d'alors.
                    reprojection = previousViewProjection * Math::Matrix4f::Translation(temporal.cameraDelta)
                                 * InverseViewProjection(camera, aspect);
                }
                if (temporalAA || fsr) {
                    ++temporalFrame;
                    // FSR veut plus de décalages quand l'image est plus réduite (8 fois le rapport au carré).
                    const std::uint32_t phases = fsr ? upscaler->GetJitterPhaseCount(render.width) : JitterPhases;
                    const std::uint32_t phase = temporalFrame % phases + 1; // Halton(0) vaut 0 : sauté
                    // En pixels de rendu, x vers la droite, y vers le bas (comme les uv et le clip space de Vulkan).
                    const float jitterX = Halton(phase, 2) - 0.5f;
                    const float jitterY = Halton(phase, 3) - 0.5f;
                    temporal.jitter = {2.0f * jitterX / static_cast<float>(render.width),
                                       2.0f * jitterY / static_cast<float>(render.height)};
                    temporal.noiseFrame = temporalFrame;
                    frame.upscale.jitterX = jitterX;
                    frame.upscale.jitterY = jitterY;
                }
            }
            hasPrevious = sceneVisible;
            previousViewProjection = viewProjection;
            previousPosition = camera.position;
            if (sceneVisible) {
                if (auto resized = ResizeViewTargets(view, render); !resized) {
                    Log::Error("{}", resized.error());
                    return false;
                }
                if (!temporalAA && !fsr) {
                    post->SkipTemporal();
                }
                const SkyLighting lighting = sky->Prepare(scene, camera, aspect, static_cast<float>(render.height),
                                                          assetsDirectory, frameIndex);
                exposure = lighting.exposure;
                meshes->Prepare(scene, camera, aspect, hidden, lighting, temporal, assetsDirectory, frameIndex);
                if (!meshes->HasClouds() || !temporal.continuous) {
                    clouds->ResetHistory(); // les passes des nuages sautées, ou la caméra a sauté
                }
                thrusters->Prepare(meshes->GetThrusters(), camera, aspect, temporal.jitter);
                tracers->Prepare(projectiles, impacts, camera, aspect, static_cast<float>(render.height), temporal.jitter, frameIndex);
                const OcclusionCulling::Stats occlusion = meshes->GetOcclusionStats();
                timings.occlusionTested = occlusion.tested;
                timings.occlusionHidden = occlusion.hidden;
                const MeshRenderer::MeshletStats meshlets = meshes->GetMeshletStats();
                timings.modelMeshlets = meshlets.sent;
                timings.modelMeshletsDrawn = meshlets.drawn;
                timings.modelMeshShaders = meshlets.meshShaders;
                const MeshRenderer::PlanetTileStats tiles = meshes->GetPlanetTileStats();
                timings.planetTiles = tiles.drawn;
                timings.planetOceanTiles = tiles.ocean;
                timings.planetStoredTiles = tiles.stored;
                timings.planetPendingTiles = tiles.pending;
                timings.planetComputedTiles = tiles.computed;
                timings.groundMeshThreads = tiles.meshThreads;
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
                timings.sownTreePlains = vegetation.sown;
            }

            const Clock::time_point recordStart = Clock::now();
            timings.prepare = Milliseconds(recordStart - prepareStart);
            frame.index = frameIndex;
            frame.scene3d = scene3d;
            frame.render = render;
            renderExtent = render;
            frame.fsr = fsr;
            frame.upscale.renderSize = render;
            frame.upscale.sharpness = sharpness;
            frame.upscale.frameMilliseconds = frameMilliseconds;
            frame.upscale.nearPlane = camera.nearPlane;
            frame.upscale.verticalFov = camera.verticalFov;
            frame.upscale.reset = !temporal.continuous;
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
            // Gardées d'une image à l'autre : tuiles calculées une fois, ciel et tables de l'air recalculés quand ils
            // changent, cascades lointaines des ombres redessinées une image sur deux, pyramide et historique de
            // l'image précédente.
            const auto tiles = graph.AddResource("tuiles de planète", Lifetime::Previous);
            const auto pyramid = graph.AddResource("pyramide de profondeur", Lifetime::Previous);
            const auto culled = graph.AddResource("arbres et objets retenus");
            // Textures du ciel et des ombres, à leurs renderers : le graphe pose leurs barrières, seulement avec une scène.
            const auto texture = [this](const char* name, Lifetime lifetime, auto source) {
                return graph.AddTexture(name, lifetime, [this, source]() -> const RHI::Texture* {
                    return sceneVisible ? source() : nullptr;
                });
            };
            const auto skyBackground = texture("fond du ciel", Lifetime::Previous, [this] { return &sky->GetBackground(); });
            const auto skyRayleigh = texture("ciel : Rayleigh", Lifetime::Previous, [this] { return &sky->GetSkyRayleigh(); });
            const auto skyMie = texture("ciel : Mie", Lifetime::Previous, [this] { return &sky->GetSkyMie(); });
            const auto airRayleigh = texture("air : Rayleigh", Lifetime::Previous, [this] { return &sky->GetAirRayleigh(); });
            const auto airMie = texture("air : Mie", Lifetime::Previous, [this] { return &sky->GetAirMie(); });
            const auto shadowMap = texture("carte d'ombres", Lifetime::Previous, [this] { return &meshes->GetShadowMap(); });
            // Textures de travail, créées par le graphe à la taille de la vue 3D. Profondeur lisible : l'anticrénelage
            // y retrouve où était chaque point. L'historique a deux exemplaires : l'anticrénelage lit l'un, écrit l'autre.
            using TextureSize = RenderGraph::TextureSize;
            // La scène à la taille de rendu ; l'historique, résultat de l'anticrénelage ou de FSR, à celle de la vue.
            targets.sceneColor = graph.CreateTexture("image de la scène", Lifetime::Frame, {PostRenderer::SceneFormat, TextureSize::Render});
            targets.motion = graph.CreateTexture("mouvement", Lifetime::Frame, {PostRenderer::MotionFormat, TextureSize::Render});
            targets.fullMotion = graph.CreateTexture("mouvement complet", Lifetime::Frame, {PostRenderer::MotionFormat, TextureSize::Render});
            targets.sceneDepth = graph.CreateTexture("profondeur de la scène", Lifetime::Frame, {DepthFormat, TextureSize::Render});
            targets.history = graph.CreateTexture("historique de l'anticrénelage", Lifetime::Previous,
                                                  {PostRenderer::SceneFormat, TextureSize::View, true, true});
            targets.windowDepth = graph.CreateTexture("profondeur de la fenêtre", Lifetime::Frame,
                                                      {DepthFormat, TextureSize::Window, false});
            // Nuages, à la moitié de la vue : ce qu'en voit cette image, puis mêlé à l'historique.
            targets.cloudLight = graph.CreateTexture("nuages", Lifetime::Frame,
                                                     {Planetology::CloudRenderer::LightFormat, TextureSize::HalfView});
            targets.cloudDistances = graph.CreateTexture("distances des nuages", Lifetime::Frame,
                                                         {Planetology::CloudRenderer::DistanceFormat, TextureSize::HalfView});
            targets.cloudHistory = graph.CreateTexture("historique des nuages", Lifetime::Previous,
                                                       {Planetology::CloudRenderer::LightFormat, TextureSize::HalfView});
            const auto sceneColor = targets.sceneColor;
            const auto motion = targets.motion;
            const auto fullMotion = targets.fullMotion;
            const auto sceneDepth = targets.sceneDepth;
            const auto history = targets.history;
            const auto windowDepth = targets.windowDepth;
            // Niveaux du bloom : internes à PostRenderer::DrawBloom, qui laisse son résultat lisible.
            const auto bloomTextures = graph.AddResource("bloom");
            const auto windowImage = graph.AddResource("image de la fenêtre"); // préparée par sa passe (RHI)
            const auto scene = [this] { return sceneVisible; };

            graph.AddPass("calculs")
                .When(scene)
                .Reads(cpu)
                .Reads(pyramid)
                .Writes(tiles)
                .Writes(culled)
                .Step("tuiles de planète", [this](RHI::CommandList& commands) { meshes->ComputeTiles(commands, frame.index); })
                .Step("semis des arbres", [this](RHI::CommandList& commands) { meshes->SowTrees(commands, frame.index); })
                .Step("tri des arbres", [this](RHI::CommandList& commands) { meshes->CullTrees(commands, frame.index); })
                .Step("objets cachés", [this](RHI::CommandList& commands) { meshes->CullHidden(commands, frame.index); });
            graph.AddPass("tables de l'air")
                .When([this] { return sceneVisible && sky->TablesPending(); })
                .Reads(cpu)
                .Writes(skyRayleigh)
                .Writes(skyMie)
                .Writes(airRayleigh)
                .Writes(airMie)
                .Step("tables de l'air", [this](RHI::CommandList& commands) { sky->BakeTables(commands, frame.index); });
            graph.AddPass("nébuleuse")
                .When([this] { return sceneVisible && sky->BackgroundPending(); })
                .Reads(cpu)
                .Writes(skyBackground)
                .Step("nébuleuse", [this](RHI::CommandList& commands) { sky->BakeBackground(commands, frame.index); });
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
                .Reads(skyBackground)
                .Reads(skyRayleigh)
                .Reads(skyMie)
                .Reads(airRayleigh)
                .Reads(airMie)
                .Writes(sceneColor)
                .Writes(motion)
                .Writes(sceneDepth)
                .Begin([this](RHI::CommandList& commands) {
                    // Fond sans ciel : la couleur choisie doit ressortir telle quelle de la courbe de tons.
                    // Profondeur et mouvement gardés pour l'anticrénelage.
                    commands.BeginRenderPass({.clearColor = PostRenderer::InverseToneMap(clearColor, frame.exposure),
                                              .depth = graph.GetTexture(targets.sceneDepth), .clearDepth = 0.0f,
                                              .colorTarget = graph.GetTexture(targets.sceneColor),
                                              .secondColorTarget = graph.GetTexture(targets.motion), .keepDepth = true});
                    commands.SetViewport({0.0f, 0.0f, static_cast<float>(frame.render.width), static_cast<float>(frame.render.height)});
                    commands.SetScissor({0, 0, frame.render.width, frame.render.height});
                })
                .Step("objets", [this](RHI::CommandList& commands) { meshes->DrawOpaque(commands, frame.index); })
                .Step("arbres", [this](RHI::CommandList& commands) { meshes->DrawVegetation(commands, frame.index); })
                .Step("sol", [this](RHI::CommandList& commands) { meshes->DrawGround(commands, frame.index); })
                .Step("fond du ciel", [this](RHI::CommandList& commands) { sky->Draw(commands, frame.index); })
                .Step("océan", [this](RHI::CommandList& commands) { meshes->DrawOcean(commands, frame.index); });
            // Nuages : ils lisent la profondeur de la scène opaque (le relief, les vaisseaux les arrêtent), hors de sa passe.
            const auto hasClouds = [this] { return sceneVisible && meshes->HasClouds(); };
            const auto cloudLight = targets.cloudLight;
            const auto cloudDistances = targets.cloudDistances;
            const auto cloudHistory = targets.cloudHistory;
            const auto halfViewport = [this](RHI::CommandList& commands) {
                const RHI::Extent half = graph.GetTexture(targets.cloudLight)->GetExtent();
                commands.SetViewport({0.0f, 0.0f, static_cast<float>(half.width), static_cast<float>(half.height)});
                commands.SetScissor({0, 0, half.width, half.height});
            };
            graph.AddPass("nuages")
                .When(hasClouds)
                .Reads(cpu)
                .Reads(sceneDepth)
                .Writes(cloudLight)
                .Writes(cloudDistances)
                .Begin([this, halfViewport](RHI::CommandList& commands) {
                    commands.BeginRenderPass({.colorTarget = graph.GetTexture(targets.cloudLight),
                                              .secondColorTarget = graph.GetTexture(targets.cloudDistances)});
                    halfViewport(commands);
                })
                .Step("nuages", [this](RHI::CommandList& commands) { clouds->DrawMarch(commands, frame.index); });
            graph.AddPass("historique des nuages")
                .When(hasClouds)
                .Reads(cpu)
                .Reads(cloudLight)
                .Reads(cloudDistances)
                .Reads(cloudHistory)
                .Writes(cloudHistory)
                .Begin([this, halfViewport](RHI::CommandList& commands) {
                    frame.cloudCopy = graph.GetWriteCopy(targets.cloudHistory);
                    commands.BeginRenderPass({.colorTarget = graph.GetCopy(targets.cloudHistory, frame.cloudCopy)});
                    halfViewport(commands);
                })
                .Step("historique des nuages", [this](RHI::CommandList& commands) {
                    clouds->DrawResolve(commands, frame.index, frame.cloudCopy);
                });
            const auto sceneViewport = [this](RHI::CommandList& commands) {
                commands.SetViewport({0.0f, 0.0f, static_cast<float>(frame.render.width), static_cast<float>(frame.render.height)});
                commands.SetScissor({0, 0, frame.render.width, frame.render.height});
            };
            graph.AddPass("composition des nuages")
                .When(hasClouds)
                .Reads(cpu)
                .Reads(sceneDepth)
                .Reads(cloudDistances)
                .Reads(cloudHistory)
                .Writes(sceneColor)
                .Begin([this, sceneViewport](RHI::CommandList& commands) {
                    commands.BeginRenderPass({.colorTarget = graph.GetTexture(targets.sceneColor), .loadColor = true});
                    sceneViewport(commands);
                })
                .Step("composition des nuages", [this](RHI::CommandList& commands) {
                    clouds->DrawComposite(commands, frame.index, frame.cloudCopy);
                });
            // La scène reprise là où elle en était (couleur, mouvement et profondeur gardés) : ce qui reste est devant les
            // nuages ou mêlé à eux, dans l'ordre où il se recouvre.
            graph.AddPass("transparents")
                .When(scene)
                .Reads(cpu)
                .Reads(tiles)
                .Reads(culled)
                .Reads(shadowMap)
                .Reads(skyBackground)
                .Reads(skyRayleigh)
                .Reads(skyMie)
                .Reads(airRayleigh)
                .Reads(airMie)
                .Writes(sceneColor)
                .Writes(motion)
                .Writes(sceneDepth)
                .Begin([this, sceneViewport](RHI::CommandList& commands) {
                    commands.BeginRenderPass({.depth = graph.GetTexture(targets.sceneDepth),
                                              .colorTarget = graph.GetTexture(targets.sceneColor),
                                              .secondColorTarget = graph.GetTexture(targets.motion), .keepDepth = true,
                                              .loadDepth = true, .loadColor = true});
                    sceneViewport(commands);
                })
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
                    meshes->BuildDepthPyramid(commands, frame.index, *graph.GetTexture(targets.sceneDepth), frame.render);
                });
            // Coupé (réglage), ou FSR à la place : la passe ne joue pas, l'historique garde son dernier exemplaire écrit.
            graph.AddPass("anticrénelage")
                .When([this] { return sceneVisible && temporalAA && !frame.fsr; })
                .Reads(sceneColor)
                .Reads(motion)
                .Reads(sceneDepth)
                .Reads(history)
                .Writes(history)
                .Step("anticrénelage", [this](RHI::CommandList& commands) {
                    post->ResolveTemporal(commands, graph.GetWriteCopy(targets.history), frame.reprojection, frame.temporal.history);
                });
            const auto upscaled = [this] { return sceneVisible && frame.fsr; };
            graph.AddPass("mouvement complet")
                .When(upscaled)
                .Reads(motion)
                .Reads(sceneDepth)
                .Writes(fullMotion)
                .Step("mouvement complet", [this](RHI::CommandList& commands) { post->DrawMotion(commands, frame.reprojection); });
            // FSR écrit l'historique (taille de la vue) par ses compute shaders ; il garde son propre historique.
            graph.AddPass("FSR")
                .When(upscaled)
                .Reads(sceneColor)
                .Reads(fullMotion)
                .Reads(sceneDepth)
                .Writes(history, RHI::TextureAccess::Storage)
                .Step("FSR", [this](RHI::CommandList& commands) {
                    const std::uint32_t target = graph.GetWriteCopy(targets.history);
                    frame.upscale.color = graph.GetTexture(targets.sceneColor);
                    frame.upscale.depth = graph.GetTexture(targets.sceneDepth);
                    frame.upscale.motion = graph.GetTexture(targets.fullMotion);
                    frame.upscale.output = graph.GetCopy(targets.history, target);
                    upscaler->Dispatch(commands, frame.upscale);
                    post->UseUpscaled(target);
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
                .Writes(windowDepth)
                .Writes(windowImage)
                .Begin([this](RHI::CommandList& commands) {
                    commands.BeginRenderPass({.clearColor = clearColor, .depth = graph.GetTexture(targets.windowDepth), .clearDepth = 0.0f});
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
        auto clouds = Planetology::CloudRenderer::Create(*impl->device, *impl->meshes, sceneFormat);
        if (!clouds) {
            return std::unexpected(clouds.error());
        }
        impl->clouds = std::move(*clouds);
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

    void Renderer::SetUpscaling(Upscaling upscaling) {
        m_impl->upscaling = upscaling; // FSR créé ou libéré au début de la prochaine image
    }

    Upscaling Renderer::GetUpscaling() const {
        return m_impl->upscaling;
    }

    bool Renderer::IsUpscalingAvailable() const {
        return !m_impl->upscalerFailed;
    }

    void Renderer::SetSharpness(float sharpness) {
        m_impl->sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    }

    float Renderer::GetSharpness() const {
        return m_impl->sharpness;
    }

    Math::Vector2i Renderer::GetRenderSize() const {
        return {static_cast<int>(m_impl->renderExtent.width), static_cast<int>(m_impl->renderExtent.height)};
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

    void Renderer::SetGroundDetail(GroundDetail detail) {
        m_impl->meshes->SetGroundDetail(detail);
    }

    GroundDetail Renderer::GetGroundDetail() const {
        return m_impl->meshes->GetGroundDetail();
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
