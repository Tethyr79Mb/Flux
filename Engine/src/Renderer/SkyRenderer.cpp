#include "Renderer/SkyRenderer.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <optional>

namespace Flux {
    namespace {
        // Doit correspondre à SkyData dans shaders/Sky.slang.
        struct SkyData
        {
            std::array<float, 4> right;
            std::array<float, 4> up;
            std::array<float, 4> forward;      // w : angle d'un pixel
            std::array<float, 4> toSun;        // w : rayon du disque
            std::array<float, 4> sunColor;     // w : exposition
            std::array<float, 4> discColor;
            std::array<float, 4> nebulaColor;  // w : densité
            std::array<float, 4> nebulaColor2; // w : bande galactique
            std::array<float, 4> galaxyAxis;   // w : quantité d'étoiles
            std::array<float, 4> galaxyCenter; // w : éclat des étoiles
            std::array<float, 4> noiseOffset;  // w : luminosité
            std::array<std::uint32_t, 4> options; // x : graine ; y : panorama ; z : rayons du soleil (SunRays)
            Planetology::AtmosphereGpu atmosphere;
        };
        static_assert(sizeof(SkyData) == 192 + sizeof(Planetology::AtmosphereGpu));

        constexpr std::uint32_t DataSlot = 0;
        constexpr std::uint32_t PanoramaSlot = 1;
        constexpr std::uint32_t BackgroundSlot = 2;

        // Texture cube du fond généré : environ un texel par pixel en 1080p (champ de 70°). 16 bits flottants par
        // canal : la nébuleuse peut dépasser 1 avant la luminosité, sans bandes dans les dégradés sombres. 48 Mo.
        constexpr std::uint32_t BackgroundSize = 1024;

        // Texture cube de la lumière ambiante et des reflets : floue de toute façon (les reflets nets viendraient
        // d'autres techniques), petite pour être refaite vite quand un réglage du ciel change.
        constexpr std::uint32_t EnvironmentSize = 128;
        constexpr std::uint32_t GradientSize = 16;
        // Un réglage qu'on fait glisser change le ciel à chaque image : la texture cube suit au plus à ce rythme.
        constexpr std::chrono::milliseconds MinRebuildInterval{150};

        // Éclairage sans ciel, celui d'avant le ciel : soleil légèrement chaud, ambiante bleutée en haut et plus
        // sombre en bas (le sol renvoie moins que le ciel). Valeurs avant l'exposition et la courbe de tons :
        // 1 donne à peu près le blanc d'une surface blanche face à la lumière.
        constexpr float DefaultSunElevation = 62.9f;
        constexpr float DefaultSunAzimuth = 53.1f;
        const Math::Vector3f DefaultSunColor{1.45f, 1.4f, 1.3f};
        constexpr Math::LinearColor DefaultSkyAmbient{0.42f, 0.45f, 0.5f};
        constexpr Math::LinearColor DefaultGroundAmbient{0.2f, 0.19f, 0.18f};
        constexpr float Exposure = 1.0f;

        constexpr float RadiansPerDegree = std::numbers::pi_v<float> / 180.0f;

        std::array<float, 4> ToArray(const Math::Vector3f& v, float w = 0.0f) { return {v.x, v.y, v.z, w}; }
        std::array<float, 4> ToArray(const Math::LinearColor& c, float w = 0.0f) { return {c.r, c.g, c.b, w}; }

        std::uint32_t MipCount(std::uint32_t size) {
            return static_cast<std::uint32_t>(std::bit_width(size));
        }
    }

    std::expected<std::unique_ptr<SkyRenderer>, std::string> SkyRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                  RHI::Format motionFormat, RHI::Format depthFormat) {
        std::unique_ptr<SkyRenderer> sky(new SkyRenderer(device));

        // Le panorama fait le tour en largeur : répété (sans couture à la jonction).
        auto panoramaSampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Linear,
                                                     .address = RHI::AddressMode::Repeat});
        auto cubeSampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Linear,
                                                 .address = RHI::AddressMode::ClampToEdge});
        if (!panoramaSampler || !cubeSampler) {
            return std::unexpected(!panoramaSampler ? panoramaSampler.error() : cubeSampler.error());
        }
        sky->m_panoramaSampler = std::move(*panoramaSampler);
        sky->m_cubeSampler = std::move(*cubeSampler);
        constexpr std::array<std::uint8_t, 4> black{0, 0, 0, 255};
        auto blackTexture = device.CreateTexture(RHI::Format::RGBA8Srgb, {1, 1}, black);
        if (!blackTexture) {
            return std::unexpected(blackTexture.error());
        }
        sky->m_black = std::move(*blackTexture);
        constexpr std::array<std::uint8_t, 4 * 6> blackFaces{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
                                                             0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
        auto blackCube = device.CreateCubeTexture(RHI::Format::RGBA8Unorm, 1, blackFaces);
        auto background = device.CreateCubeRenderTarget(RHI::Format::RGBA16Float, BackgroundSize);
        if (!blackCube || !background) {
            return std::unexpected(!blackCube ? blackCube.error() : background.error());
        }
        sky->m_blackCube = std::move(*blackCube);
        sky->m_background = std::move(*background);

        constexpr std::array bindings{
            RHI::Binding{DataSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Fragment},
            RHI::Binding{PanoramaSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{BackgroundSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = RHI::FramesInFlight});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        sky->m_layout = std::move(*layout);
        for (std::uint32_t f = 0; f < RHI::FramesInFlight; ++f) {
            auto buffer = device.CreateBuffer({sizeof(SkyData), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            sky->m_groups[f] = device.CreateBindGroup(*sky->m_layout);
            if (!buffer || !sky->m_groups[f]) {
                return std::unexpected("création des ressources du ciel");
            }
            sky->m_buffers[f] = std::move(*buffer);
            sky->m_groups[f]->SetBuffer(DataSlot, *sky->m_buffers[f]);
            sky->m_groups[f]->SetTexture(PanoramaSlot, *sky->m_black, *sky->m_panoramaSampler);
            sky->m_boundPanorama[f] = sky->m_black.get();
            sky->m_groups[f]->SetTexture(BackgroundSlot, *sky->m_blackCube, *sky->m_cubeSampler);
            sky->m_boundBackground[f] = sky->m_blackCube.get();
        }

        // Derrière tout : seulement là où la profondeur est restée celle de l'effacement (0, l'infini en reverse-Z).
        const RHI::BindGroupLayout* layouts[] = {sky->m_layout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Sky.spv";
        desc.bindGroupLayouts = layouts;
        desc.colorFormat = colorFormat;
        desc.secondColorFormat = motionFormat; // le ciel ne bouge pas de lui-même : mouvement laissé à 0
        desc.depthFormat = depthFormat;
        desc.depthTest = true;
        desc.depthWrite = false;
        desc.depthCompare = RHI::CompareOp::GreaterOrEqual;
        auto pipeline = device.CreatePipeline(desc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        sky->m_pipeline = std::move(*pipeline);

        // Soleil : au même endroit que le fond, sa lumière ajoutée à lui.
        RHI::PipelineDesc sunDesc = desc;
        sunDesc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Sun.spv";
        sunDesc.blend = RHI::BlendMode::Additive;
        auto sunPipeline = device.CreatePipeline(sunDesc);
        if (!sunPipeline) {
            return std::unexpected(sunPipeline.error());
        }
        sky->m_sunPipeline = std::move(*sunPipeline);

        // Calcul du fond : le même triangle plein écran, dans chaque face de la texture cube.
        RHI::PipelineDesc bakeDesc;
        bakeDesc.shaderPath = desc.shaderPath;
        bakeDesc.fragmentEntryPoint = "bakeFragment";
        bakeDesc.bindGroupLayouts = layouts;
        bakeDesc.pushConstantsSize = sizeof(std::uint32_t);
        bakeDesc.colorFormat = RHI::Format::RGBA16Float;
        auto bakePipeline = device.CreatePipeline(bakeDesc);
        if (!bakePipeline) {
            return std::unexpected(bakePipeline.error());
        }
        sky->m_bakePipeline = std::move(*bakePipeline);
        return sky;
    }

    const SkyRenderer::Panorama& SkyRenderer::LoadPanorama(const std::filesystem::path& path) {
        if (const auto found = m_panoramas.find(path); found != m_panoramas.end()) {
            return found->second;
        }
        Panorama& panorama = m_panoramas[path];
        auto image = LoadImageFile(path);
        if (!image) {
            Log::Warning("ciel : {}", image.error());
            return panorama;
        }
        const RHI::Extent extent{static_cast<std::uint32_t>(image->size.x), static_cast<std::uint32_t>(image->size.y)};
        auto texture = m_device.CreateTexture(RHI::Format::RGBA8Srgb, extent, image->rgba, true);
        if (!texture) {
            Log::Error("ciel : {}", texture.error());
            return panorama;
        }
        panorama.texture = std::move(*texture);
        panorama.cube = PanoramaToCube(*image, EnvironmentSize);
        return panorama;
    }

    // Pixels refaits si le ciel a changé (pas trop souvent), puis la texture de l'image frameIndex si elle n'est pas à
    // jour : ses ressources sont libres (après BeginFrame), l'autre image en vol garde la sienne.
    void SkyRenderer::UpdateEnvironment(const EnvironmentKey& key, std::uint32_t frameIndex) {
        const auto now = std::chrono::steady_clock::now();
        if (!m_hasPixels || (key != m_key && now - m_lastBuild >= MinRebuildInterval)) {
            switch (key.kind) {
                case EnvironmentKey::Kind::Gradient:
                    m_pixels = GradientCube(DefaultSkyAmbient, DefaultGroundAmbient, GradientSize);
                    m_pixelsSize = GradientSize;
                    break;
                case EnvironmentKey::Kind::Generated:
                    m_pixels = GenerateSkyCube(key.parameters, EnvironmentSize);
                    m_pixelsSize = EnvironmentSize;
                    break;
                case EnvironmentKey::Kind::Panorama: {
                    const Panorama& panorama = LoadPanorama(key.panorama);
                    m_pixels = panorama.cube.empty() ? GradientCube({}, {}, GradientSize) : panorama.cube;
                    m_pixelsSize = panorama.cube.empty() ? GradientSize : EnvironmentSize;
                    break;
                }
            }
            m_key = key;
            m_hasPixels = true;
            m_lastBuild = now;
            ++m_version;
        }
        if (m_environmentVersions[frameIndex] == m_version && m_environments[frameIndex]) {
            return;
        }
        auto texture = m_device.CreateCubeTexture(RHI::Format::RGBA8Srgb, m_pixelsSize, m_pixels, true);
        if (!texture) {
            Log::Error("ciel : {}", texture.error());
            return; // l'ancienne reste
        }
        m_environments[frameIndex] = std::move(*texture);
        m_environmentVersions[frameIndex] = m_version;
    }

    SkyLighting SkyRenderer::Prepare(const Scene& scene, const Camera& camera, float aspect, float viewportHeight,
                                     const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex) {
        // Un seul ciel compte : s'il y en a plusieurs, l'un d'eux.
        const SkyComponent* component = nullptr;
        for (const auto [entity, sky] : scene.GetRegistry().view<const SkyComponent>().each()) {
            component = &sky;
            break;
        }

        SkyLighting lighting;
        lighting.exposure = Exposure;
        EnvironmentKey key;
        const RHI::Texture* panoramaTexture = m_black.get();
        SkyData data{};
        const float tanY = std::tan(0.5f * camera.verticalFov);
        data.right = ToArray(camera.GetRight() * (tanY * aspect));
        data.up = ToArray(camera.GetUp() * tanY);
        data.forward = ToArray(camera.GetForward(), 2.0f * tanY / std::max(viewportHeight, 1.0f));
        // Soleil posé dans l'espace : sa lumière vient de sa position, vue de la caméra (le premier trouvé).
        std::optional<Math::Vector3f> sunDirection;
        const SunComponent* sun = nullptr;
        for (const auto [entity, candidate] : scene.GetRegistry().view<const SunComponent>().each()) {
            const Math::Vector3d offset = Math::Delta(camera.position, scene.GetWorldPosition(entity));
            if (Math::Length(offset) > 1.0e-3) {
                sun = &candidate;
                sunDirection = Math::Vector3f(Math::Normalize(offset));
            }
            break;
        }
        const auto sunLight = [&](const Math::LinearColor& color, float intensity) {
            return Math::Vector3f{color.r, color.g, color.b} * std::max(intensity, 0.0f);
        };
        // Disque du soleil : son rayon apparent en radians.
        const auto sunRadius = [](float degrees) { return 0.5f * std::clamp(degrees, 0.0f, 45.0f) * RadiansPerDegree; };
        if (!component) {
            // Pas de fond (luminosité nulle) : seulement l'air d'une planète s'il y en a, et le disque d'un Soleil posé.
            lighting.toSun = sunDirection.value_or(SunDirection(DefaultSunElevation, DefaultSunAzimuth));
            lighting.sunColor = sun ? sunLight(sun->lightColor, sun->intensity) : DefaultSunColor;
            data.toSun = ToArray(lighting.toSun, sun ? sunRadius(sun->size) : 0.0f);
            data.sunColor = ToArray(lighting.sunColor, Exposure);
        } else {
            const SkyComponent& sky = *component;
            lighting.toSun = sunDirection.value_or(SunDirection(sky.sunElevation, sky.sunAzimuth));
            lighting.sunColor = sun ? sunLight(sun->lightColor, sun->intensity) : sunLight(sky.sunColor, sky.sunIntensity);
            const float brightness = std::max(sky.brightness, 0.0f);
            lighting.ambient = std::max(sky.ambient, 0.0f) * brightness;

            if (!sky.image.empty() && !assetsDirectory.empty()) {
                key.kind = EnvironmentKey::Kind::Panorama;
                key.panorama = assetsDirectory / Platform::PathFromUtf8(sky.image);
                if (const Panorama& panorama = LoadPanorama(key.panorama); panorama.texture) {
                    panoramaTexture = panorama.texture.get();
                }
            } else {
                key.kind = EnvironmentKey::Kind::Generated;
                key.parameters = MakeSkyParameters(sky);
            }

            const SkyParameters parameters = key.kind == EnvironmentKey::Kind::Generated ? key.parameters : MakeSkyParameters(sky);
            data.toSun = ToArray(lighting.toSun, sunRadius(sun ? sun->size : sky.sunSize));
            data.sunColor = ToArray(lighting.sunColor, Exposure);
            data.nebulaColor = ToArray(parameters.nebulaColor, parameters.nebula);
            data.nebulaColor2 = ToArray(parameters.nebulaColor2, parameters.galaxy);
            data.galaxyAxis = ToArray(parameters.galaxyAxis, std::clamp(sky.stars, 0.0f, 1.0f));
            data.galaxyCenter = ToArray(parameters.galaxyCenter, std::max(sky.starBrightness, 0.0f));
            data.noiseOffset = ToArray(parameters.noiseOffset, brightness);
            data.options = {parameters.seed, key.kind == EnvironmentKey::Kind::Panorama && panoramaTexture != m_black.get() ? 1u : 0u, 0, 0};
        }
        // Le disque d'un Soleil posé a sa propre couleur ; sa lumière, la sienne (blanche par défaut).
        data.discColor = ToArray(sun ? sunLight(sun->color, sun->intensity) : lighting.sunColor, 0.0f);
        data.options[2] = static_cast<std::uint32_t>(sun ? sun->rays : SunRays::Star);
        lighting.atmosphere = PrepareAtmosphere(scene, camera, lighting.sunColor);
        data.atmosphere = lighting.atmosphere;
        m_visible = component != nullptr || lighting.atmosphere.mie[3] > 0.0f;
        if (m_visible) {
            m_buffers[frameIndex]->Write(&data, sizeof(data));
        }

        // Fond généré : recalculé (Bake) pour de nouveaux réglages, lu ensuite tant qu'ils ne changent pas.
        const RHI::Texture* backgroundTexture = m_blackCube.get();
        if (component && key.kind == EnvironmentKey::Kind::Generated) {
            const auto now = std::chrono::steady_clock::now();
            if (!m_bakedParameters || (*m_bakedParameters != key.parameters && now - m_lastBake >= MinRebuildInterval)) {
                m_bakedParameters = key.parameters;
                m_bakePending = true;
                m_lastBake = now;
            }
            backgroundTexture = m_background.get();
        }
        if (m_boundBackground[frameIndex] != backgroundTexture) {
            m_groups[frameIndex]->SetTexture(BackgroundSlot, *backgroundTexture, *m_cubeSampler);
            m_boundBackground[frameIndex] = backgroundTexture;
        }
        if (m_boundPanorama[frameIndex] != panoramaTexture) {
            m_groups[frameIndex]->SetTexture(PanoramaSlot, *panoramaTexture, *m_panoramaSampler);
            m_boundPanorama[frameIndex] = panoramaTexture;
        }
        UpdateEnvironment(key, frameIndex);
        lighting.environment = m_environments[frameIndex].get();
        lighting.environmentSampler = m_cubeSampler.get();
        lighting.environmentMips = MipCount(m_environments[frameIndex] ? m_environments[frameIndex]->GetExtent().width : 1);
        return lighting;
    }

    Planetology::AtmosphereGpu SkyRenderer::PrepareAtmosphere(const Scene& scene, const Camera& camera, const Math::Vector3f& sunColor) {
        // La planète dont l'air est le plus proche (on est dedans, ou c'est celui qu'on approche). Au-delà de 100 000 km,
        // elle ne fait que quelques pixels : ignorée (et les floats du shader n'y suffiraient plus).
        constexpr double MaxDistance = 1.0e8;
        Planetology::AtmosphereGpu gpu;
        std::optional<Planetology::AtmosphereParameters> chosen;
        Math::Vector3d chosenCenter;
        double best = MaxDistance;
        for (const auto [entity, planet] : scene.GetRegistry().view<const PlanetComponent>().each()) {
            if (scene.GetShip(entity) != NullEntity) {
                continue; // une planète dans un vaisseau : un décor
            }
            const Planetology::AtmosphereParameters parameters = Planetology::MakeAtmosphereParameters(planet);
            if (parameters.top <= 0.0f) {
                continue;
            }
            const Math::Vector3d center = Math::Delta(camera.position, scene.GetWorldPosition(entity));
            const double gap = Math::Length(center) - parameters.top;
            if (gap < best) {
                best = gap;
                chosen = parameters;
                chosenCenter = center;
            }
        }
        if (!chosen) {
            return gpu;
        }
        const Planetology::AtmosphereParameters& a = *chosen;
        // Lumière ambiante du ciel : quelques millisecondes, refaite seulement quand l'air ou le soleil changent.
        if (m_ambientParameters != a || m_ambientSunColor != sunColor) {
            m_ambientTable = Planetology::ComputeAtmosphereAmbient(a, sunColor);
            m_ambientParameters = a;
            m_ambientSunColor = sunColor;
        }
        gpu.center = {static_cast<float>(chosenCenter.x), static_cast<float>(chosenCenter.y), static_cast<float>(chosenCenter.z), a.radius};
        gpu.rayleigh = ToArray(a.rayleigh, a.rayleighFalloff);
        gpu.mie = {a.mie, a.mieFalloff, a.mieAnisotropy, a.top};
        gpu.scale = {a.intensity, 0.0f, 0.0f, 0.0f};
        for (std::size_t i = 0; i < m_ambientTable.size(); ++i) {
            gpu.ambient[i] = ToArray(m_ambientTable[i]);
        }
        return gpu;
    }

    void SkyRenderer::Bake(RHI::CommandList& commands, std::uint32_t frameIndex) {
        if (!std::exchange(m_bakePending, false)) {
            return;
        }
        // Les réglages viennent des données de l'image (écrites par Prepare), comme pour le dessin du fond.
        constexpr float size = static_cast<float>(BackgroundSize);
        for (std::uint32_t face = 0; face < 6; ++face) {
            commands.BeginRenderPass({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .colorTarget = m_background.get(), .colorLayer = face});
            commands.SetViewport({0.0f, 0.0f, size, size});
            commands.SetScissor({0, 0, BackgroundSize, BackgroundSize});
            commands.BindPipeline(*m_bakePipeline);
            commands.SetBindGroup(0, *m_groups[frameIndex]);
            commands.PushConstants(&face, sizeof(face));
            commands.Draw(3, 0);
            commands.EndRenderPass();
        }
    }

    void SkyRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        if (!m_visible) {
            return;
        }
        commands.BindPipeline(*m_pipeline);
        commands.SetBindGroup(0, *m_groups[frameIndex]);
        commands.Draw(3, 0);
        commands.BindPipeline(*m_sunPipeline); // mêmes layouts : le groupe reste lié
        commands.Draw(3, 0);
    }
}
