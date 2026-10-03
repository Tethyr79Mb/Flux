#include "Renderer/SkyRenderer.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

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
            std::array<float, 4> nebulaColor;  // w : densité
            std::array<float, 4> nebulaColor2; // w : bande galactique
            std::array<float, 4> galaxyAxis;   // w : quantité d'étoiles
            std::array<float, 4> galaxyCenter; // w : éclat des étoiles
            std::array<float, 4> noiseOffset;  // w : luminosité
            std::array<std::uint32_t, 4> options; // x : graine ; y : panorama
        };
        static_assert(sizeof(SkyData) == 176);

        constexpr std::uint32_t DataSlot = 0;
        constexpr std::uint32_t PanoramaSlot = 1;

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
                                                                                  RHI::Format depthFormat) {
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

        constexpr std::array bindings{
            RHI::Binding{DataSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Fragment},
            RHI::Binding{PanoramaSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
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
        }

        // Derrière tout : seulement là où la profondeur est restée celle de l'effacement (0, l'infini en reverse-Z).
        const RHI::BindGroupLayout* layouts[] = {sky->m_layout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Sky.spv";
        desc.bindGroupLayouts = layouts;
        desc.colorFormat = colorFormat;
        desc.depthFormat = depthFormat;
        desc.depthTest = true;
        desc.depthWrite = false;
        desc.depthCompare = RHI::CompareOp::GreaterOrEqual;
        auto pipeline = device.CreatePipeline(desc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        sky->m_pipeline = std::move(*pipeline);
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
        m_visible = component != nullptr;

        SkyLighting lighting;
        lighting.exposure = Exposure;
        EnvironmentKey key;
        const RHI::Texture* panoramaTexture = m_black.get();
        SkyData data{};
        if (!component) {
            lighting.toSun = SunDirection(DefaultSunElevation, DefaultSunAzimuth);
            lighting.sunColor = DefaultSunColor;
        } else {
            const SkyComponent& sky = *component;
            lighting.toSun = SunDirection(sky.sunElevation, sky.sunAzimuth);
            const float intensity = std::max(sky.sunIntensity, 0.0f);
            lighting.sunColor = Math::Vector3f{sky.sunColor.r, sky.sunColor.g, sky.sunColor.b} * intensity;
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
            const float tanY = std::tan(0.5f * camera.verticalFov);
            data.right = ToArray(camera.GetRight() * (tanY * aspect));
            data.up = ToArray(camera.GetUp() * tanY);
            data.forward = ToArray(camera.GetForward(), 2.0f * tanY / std::max(viewportHeight, 1.0f));
            data.toSun = ToArray(lighting.toSun, 0.5f * std::clamp(sky.sunSize, 0.0f, 45.0f) * RadiansPerDegree);
            data.sunColor = ToArray(lighting.sunColor, Exposure);
            data.nebulaColor = ToArray(parameters.nebulaColor, parameters.nebula);
            data.nebulaColor2 = ToArray(parameters.nebulaColor2, parameters.galaxy);
            data.galaxyAxis = ToArray(parameters.galaxyAxis, std::clamp(sky.stars, 0.0f, 1.0f));
            data.galaxyCenter = ToArray(parameters.galaxyCenter, std::max(sky.starBrightness, 0.0f));
            data.noiseOffset = ToArray(parameters.noiseOffset, brightness);
            data.options = {parameters.seed, key.kind == EnvironmentKey::Kind::Panorama && panoramaTexture != m_black.get() ? 1u : 0u, 0, 0};
            m_buffers[frameIndex]->Write(&data, sizeof(data));
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

    void SkyRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        if (!m_visible) {
            return;
        }
        commands.BindPipeline(*m_pipeline);
        commands.SetBindGroup(0, *m_groups[frameIndex]);
        commands.Draw(3, 0);
    }
}
