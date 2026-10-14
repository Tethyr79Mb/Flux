#include "Flux/Renderer/Renderer.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/MeshRenderer.hpp"
#include "Renderer/OverlayRenderer.hpp"
#include "Renderer/PlanetTileCompute.hpp"
#include "Renderer/PostRenderer.hpp"
#include "Renderer/RHI/RHI.hpp"
#include "Renderer/SkyRenderer.hpp"
#include "Renderer/ThrusterRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Flux {
    namespace {
        // Profondeur 32 bits flottante : obligatoire pour que le reverse-Z tienne l'échelle galaxie.
        constexpr RHI::Format DepthFormat = RHI::Format::D32Float;

        // Heures notées par le GPU pendant l'image (RHI::CommandList::WriteTimestamp).
        // Pendant la passe principale, une heure attend la fin des dessins d'avant : le GPU ne fait plus chevaucher deux
        // étapes, d'où un léger surcoût et des temps d'étape un peu plus longs qu'en vrai.
        enum Timestamp : std::uint32_t {
            FrameStart, ShadowsDone, ObjectsDone, GroundDone, SkyDone, OceanDone, CloudsDone, SceneDone, BloomDone, InterfaceDone,
            TimestampCount
        };
        static_assert(TimestampCount <= RHI::MaxTimestamps);

        using Clock = std::chrono::steady_clock;
        double Milliseconds(Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        }
    }

    // Une image : la carte d'ombres, puis la scène (opaques, fond du ciel et soleil, océan, nuages, transparents, flammes)
    // dans une image HDR à la taille de sa zone, son bloom, et la passe principale vers la fenêtre : la scène avec la
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
        std::unique_ptr<OverlayRenderer> overlay;
        std::unique_ptr<PostRenderer> post;
        std::filesystem::path assetsDirectory;
        Math::LinearColor clearColor{0.02f, 0.02f, 0.05f, 1.0f};
        bool vsync = true;
        float bloom = Renderer::DefaultBloom; // 0 : sans bloom
        Math::Vector2f sceneViewportMin; // en pixels ; vide : toute la fenêtre
        Math::Vector2f sceneViewportMax;
        bool sceneVisible = true; // false : seule l'interface est dessinée (éditeur d'interface)
        RenderTimings timings;

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
            if (!overlay->Upload(frameIndex)) {
                return false;
            }
            const RHI::Rect scene3d = SceneRect();
            const float aspect = static_cast<float>(scene3d.width) / static_cast<float>(scene3d.height);
            float exposure = 1.0f;
            if (sceneVisible) {
                if (auto resized = post->Resize({scene3d.width, scene3d.height}); !resized) {
                    Log::Error("{}", resized.error());
                    return false;
                }
                const SkyLighting lighting = sky->Prepare(scene, camera, aspect, static_cast<float>(scene3d.height),
                                                          assetsDirectory, frameIndex);
                exposure = lighting.exposure;
                meshes->Prepare(scene, camera, aspect, hidden, lighting, assetsDirectory, frameIndex);
                thrusters->Prepare(meshes->GetThrusters(), camera, aspect);
                const MeshRenderer::PlanetTileStats tiles = meshes->GetPlanetTileStats();
                timings.planetTiles = tiles.drawn;
                timings.planetOceanTiles = tiles.ocean;
                timings.planetStoredTiles = tiles.stored;
                timings.planetTileCapacity = PlanetTileCompute::MaxPages * PlanetTileCompute::TilesPerPage;
            }

            const Clock::time_point recordStart = Clock::now();
            timings.prepare = Milliseconds(recordStart - prepareStart);
            RHI::CommandList& commands = device->GetCommandList();
            // Étapes nommées (BeginStep) : si le GPU se bloque, le rapport dit laquelle n'a pas fini.
            commands.WriteTimestamp(FrameStart);
            if (sceneVisible) {
                commands.BeginStep("tuiles de planète (compute)");
                meshes->Compute(commands, frameIndex); // compté avec les ombres
                commands.BeginStep("ciel (textures cube)");
                sky->Bake(commands, frameIndex); // compté avec les ombres, l'image où il a lieu
                commands.BeginStep("ombres");
                meshes->DrawShadows(commands, frameIndex);
            }
            commands.WriteTimestamp(ShadowsDone);
            if (sceneVisible) {
                // Fond sans ciel : la couleur choisie doit ressortir telle quelle de la courbe de tons.
                commands.BeginRenderPass({.clearColor = PostRenderer::InverseToneMap(clearColor, exposure),
                                          .depth = &post->GetSceneDepth(), .clearDepth = 0.0f,
                                          .colorTarget = &post->GetSceneTarget()});
                commands.SetViewport({0.0f, 0.0f, static_cast<float>(scene3d.width), static_cast<float>(scene3d.height)});
                commands.SetScissor({0, 0, scene3d.width, scene3d.height});
                commands.BeginStep("objets opaques");
                meshes->DrawOpaque(commands, frameIndex);
                commands.WriteTimestamp(ObjectsDone);
                commands.BeginStep("sol des planètes");
                meshes->DrawGround(commands, frameIndex);
                commands.WriteTimestamp(GroundDone);
                commands.BeginStep("fond du ciel");
                sky->Draw(commands, frameIndex);
                commands.WriteTimestamp(SkyDone);
                commands.BeginStep("océan");
                meshes->DrawOcean(commands, frameIndex);
                commands.WriteTimestamp(OceanDone);
                commands.BeginStep("nuages");
                meshes->DrawClouds(commands, frameIndex);
                commands.WriteTimestamp(CloudsDone);
                commands.BeginStep("transparents");
                meshes->DrawTransparent(commands, frameIndex);
                commands.BeginStep("flammes des réacteurs"); // comptées avec les transparents
                thrusters->Draw(commands);
                commands.EndRenderPass();
            }
            commands.WriteTimestamp(SceneDone);
            if (sceneVisible) {
                commands.BeginStep("bloom");
                post->DrawBloom(commands, bloom);
            }
            commands.BeginRenderPass({.clearColor = clearColor, .depth = depth.get(), .clearDepth = 0.0f});
            if (sceneVisible) {
                commands.SetViewport({static_cast<float>(scene3d.x), static_cast<float>(scene3d.y),
                                      static_cast<float>(scene3d.width), static_cast<float>(scene3d.height)});
                commands.SetScissor(scene3d);
                post->Composite(commands, bloom, exposure);
            }
            commands.WriteTimestamp(BloomDone); // image finale comptée avec le bloom
            commands.BeginStep("interface");
            overlay->Draw(commands, frameIndex, device->GetSwapchainExtent());
            commands.EndRenderPass();
            commands.WriteTimestamp(InterfaceDone);
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
            const std::span<const double> stamps = device->GetTimestamps();
            const auto between = [&](std::uint32_t from, std::uint32_t to) {
                return to < stamps.size() && stamps[from] >= 0.0 && stamps[to] >= 0.0 ? stamps[to] - stamps[from] : -1.0;
            };
            timings.gpuShadows = between(FrameStart, ShadowsDone);
            timings.gpuScene = between(ShadowsDone, SceneDone);
            timings.gpuObjects = between(ShadowsDone, ObjectsDone);
            timings.gpuGround = between(ObjectsDone, GroundDone);
            timings.gpuSky = between(GroundDone, SkyDone);
            timings.gpuOcean = between(SkyDone, OceanDone);
            timings.gpuClouds = between(OceanDone, CloudsDone);
            timings.gpuTransparent = between(CloudsDone, SceneDone);
            timings.gpuBloom = between(SceneDone, BloomDone);
            timings.gpuInterface = between(BloomDone, InterfaceDone);
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
        auto sky = SkyRenderer::Create(*impl->device, sceneFormat, DepthFormat);
        if (!sky) {
            return std::unexpected(sky.error());
        }
        impl->sky = std::move(*sky);
        auto meshes = MeshRenderer::Create(*impl->device, sceneFormat, DepthFormat);
        if (!meshes) {
            return std::unexpected(meshes.error());
        }
        impl->meshes = std::move(*meshes);
        auto thrusters = ThrusterRenderer::Create(*impl->device, sceneFormat, DepthFormat);
        if (!thrusters) {
            return std::unexpected(thrusters.error());
        }
        impl->thrusters = std::move(*thrusters);
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
