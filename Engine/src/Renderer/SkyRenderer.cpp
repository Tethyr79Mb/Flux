#include "Renderer/SkyRenderer.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Planetology/PlanetRenderer.hpp"
#include "Scene/Photometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>

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
        constexpr std::uint32_t SkyRayleighSlot = 3;
        constexpr std::uint32_t SkyMieSlot = 4;
        constexpr std::uint32_t MultipleSlot = 5;

        // Tailles des tables de l'air : AtmosphereSkySize, puis AtmosphereAirSize x AtmosphereAirTiles dans
        // shaders/Atmosphere.slang. AirNear : AtmosphereAirNear.
        constexpr RHI::Extent SkyTableSize{256, 128};
        constexpr RHI::Extent AirTableSize{64 * 8, 64 * 8};
        constexpr double AirNear = 1000.0;

        // Texture cube du fond généré : environ un texel par pixel en 1080p (champ de 70°). 16 bits flottants par
        // canal : la nébuleuse peut dépasser 1 avant la luminosité, sans bandes dans les dégradés sombres. 48 Mo.
        constexpr std::uint32_t BackgroundSize = 1024;

        // Un réglage qu'on fait glisser change le ciel à chaque image : le fond (et l'environnement tiré de lui) suit
        // au plus à ce rythme.
        constexpr std::chrono::milliseconds MinRebuildInterval{150};

        // Éclairage sans ciel, celui d'avant le ciel : soleil légèrement chaud, ambiante bleutée en haut et plus
        // sombre en bas (le sol renvoie moins que le ciel). Valeurs avant l'exposition et la courbe de tons :
        // 1 donne à peu près le blanc d'une surface blanche face à la lumière.
        constexpr float DefaultSunElevation = 62.9f;
        constexpr float DefaultSunAzimuth = 53.1f;
        const Math::Vector3f DefaultSunColor{1.45f, 1.4f, 1.3f};
        constexpr Math::LinearColor DefaultSkyAmbient{0.42f, 0.45f, 0.5f};
        constexpr Math::LinearColor DefaultGroundAmbient{0.2f, 0.19f, 0.18f};

        constexpr float RadiansPerDegree = std::numbers::pi_v<float> / 180.0f;

        std::array<float, 4> ToArray(const Math::Vector3f& v, float w = 0.0f) { return {v.x, v.y, v.z, w}; }
        std::array<float, 4> ToArray(const Math::LinearColor& c, float w = 0.0f) { return {c.r, c.g, c.b, w}; }

        template <typename T>
        std::span<const std::uint8_t> Bytes(const T& object) {
            return {reinterpret_cast<const std::uint8_t*>(&object), sizeof(object)};
        }
    }

    std::expected<std::unique_ptr<SkyRenderer>, std::string> SkyRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                  RHI::Format motionFormat, RHI::Format depthFormat,
                                                                                  RHI::Format sunFormat) {
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
        // 16 bits flottants : sommes et épaisseurs (en hauteurs d'échelle) tiennent sans perte visible.
        for (auto* table : {&sky->m_skyRayleigh, &sky->m_skyMie, &sky->m_airRayleigh, &sky->m_airMie}) {
            const bool skyTable = table == &sky->m_skyRayleigh || table == &sky->m_skyMie;
            auto texture = device.CreateRenderTarget(RHI::Format::RGBA16Float, skyTable ? SkyTableSize : AirTableSize, true);
            if (!texture) {
                return std::unexpected(texture.error());
            }
            *table = std::move(*texture);
        }
        constexpr std::array<float, 4> nothing{};
        auto multiple = device.CreateTexture(RHI::Format::RGBA32Float, {1, 1}, Bytes(nothing));
        if (!multiple) {
            return std::unexpected(multiple.error());
        }
        sky->m_multiple = std::move(*multiple);

        constexpr std::array bindings{
            RHI::Binding{DataSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Fragment},
            RHI::Binding{PanoramaSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{BackgroundSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{SkyRayleighSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{SkyMieSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{MultipleSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
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
            sky->m_groups[f]->SetTexture(SkyRayleighSlot, *sky->m_skyRayleigh, *sky->m_cubeSampler);
            sky->m_groups[f]->SetTexture(SkyMieSlot, *sky->m_skyMie, *sky->m_cubeSampler);
            sky->m_groups[f]->SetTexture(MultipleSlot, *sky->m_multiple, *sky->m_cubeSampler);
            sky->m_boundMultiple[f] = sky->m_multiple.get();
        }

        // Derrière tout : seulement là où la profondeur est restée celle de l'effacement (0, l'infini en reverse-Z).
        const RHI::BindGroupLayout* layouts[] = {sky->m_layout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Sky.spv";
        desc.bindGroupLayouts = layouts;
        desc.colorFormat = colorFormat;
        desc.secondColorFormat = motionFormat; // le ciel ne bouge pas de lui-même : mouvement laissé à 0
        desc.thirdColorFormat = sunFormat;     // pas de surface éclairée : lumière du soleil laissée à 0
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

        // Tables de l'air : deux textures écrites à la fois.
        RHI::PipelineDesc tableDesc;
        tableDesc.shaderPath = desc.shaderPath;
        tableDesc.bindGroupLayouts = layouts;
        tableDesc.colorFormat = RHI::Format::RGBA16Float;
        tableDesc.secondColorFormat = RHI::Format::RGBA16Float;
        tableDesc.writeSecondColor = true;
        tableDesc.fragmentEntryPoint = "skyTableFragment";
        auto skyTablePipeline = device.CreatePipeline(tableDesc);
        tableDesc.fragmentEntryPoint = "airTableFragment";
        auto airTablePipeline = device.CreatePipeline(tableDesc);
        if (!skyTablePipeline || !airTablePipeline) {
            return std::unexpected(!skyTablePipeline ? skyTablePipeline.error() : airTablePipeline.error());
        }
        sky->m_skyTablePipeline = std::move(*skyTablePipeline);
        sky->m_airTablePipeline = std::move(*airTablePipeline);
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
        return panorama;
    }

    SkyRenderer::InteriorExposure SkyRenderer::GetInteriorExposure(const Scene& scene, const Camera& camera) {
        // Comme InteriorAt dans shaders/Mesh.slang : le volume où la caméra est le plus « dedans » (fondu hors des bords).
        InteriorExposure result;
        for (const auto [entity, interior] : scene.GetRegistry().view<const InteriorComponent>().each()) {
            Math::Vector3f scale{1.0f, 1.0f, 1.0f};
            for (Entity e = entity; e != NullEntity; e = scene.GetParent(e)) {
                const Math::Vector3f s = scene.Get<TransformComponent>(e).scale;
                scale = {scale.x * std::abs(s.x), scale.y * std::abs(s.y), scale.z * std::abs(s.z)};
            }
            const Math::Vector3f toCamera(Math::Delta(scene.GetWorldPosition(entity), camera.position));
            const Math::Vector3f local = scene.GetWorldRotation(entity).Conjugate().Rotate(toCamera);
            const float margin = std::min({0.5f * std::max(interior.size.x, 0.0f) * scale.x - std::abs(local.x),
                                           0.5f * std::max(interior.size.y, 0.0f) * scale.y - std::abs(local.y),
                                           0.5f * std::max(interior.size.z, 0.0f) * scale.z - std::abs(local.z)});
            const float weight = std::clamp(1.0f + margin / std::max(interior.fade, 1.0e-3f), 0.0f, 1.0f);
            if (weight > result.weight) {
                result = {weight, interior.exposure, interior.minEv100, interior.maxEv100};
            }
        }
        result.compensation *= result.weight;
        return result;
    }

    Exposure::Settings SkyRenderer::GetExposureSettings(const Scene& scene, const Camera& camera) {
        for (const auto [entity, sky] : scene.GetRegistry().view<const SkyComponent>().each()) {
            // Dans un Intérieur, ses bornes remplacent celles du Ciel (fondues près de ses bords).
            const InteriorExposure interior = GetInteriorExposure(scene, camera);
            const float minEv = std::lerp(sky.minEv100, interior.minEv100, interior.weight);
            const float maxEv = std::max(std::lerp(sky.maxEv100, interior.maxEv100, interior.weight), minEv);
            // La scène la plus claire donne l'exposition la plus basse.
            return {.automatic = sky.autoExposure, .compensation = sky.exposure + interior.compensation,
                    .minimum = std::log2(Photometry::ExposureFromEv100(maxEv)),
                    .maximum = std::log2(Photometry::ExposureFromEv100(minEv)), .speed = sky.adaptationSpeed};
        }
        return {};
    }

    SkyLighting SkyRenderer::Prepare(const Scene& scene, const Camera& camera, float aspect, float viewportHeight,
                                     const std::filesystem::path& assetsDirectory, float preExposure, std::uint32_t frameIndex) {
        // Un seul ciel compte : s'il y en a plusieurs, l'un d'eux.
        const SkyComponent* component = nullptr;
        for (const auto [entity, sky] : scene.GetRegistry().view<const SkyComponent>().each()) {
            component = &sky;
            break;
        }

        SkyLighting lighting;
        lighting.preExposure = preExposure;
        EnvironmentSource& environment = lighting.environmentSource;
        environment.top = DefaultSkyAmbient;
        environment.bottom = DefaultGroundAmbient;
        std::optional<SkyParameters> generated; // ciel généré : ses réglages
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
        const auto sunLight = [&](const Math::LinearColor& color, float lux) {
            return Math::Vector3f{color.r, color.g, color.b} * Photometry::FromLux(std::max(lux, 0.0f));
        };
        // Disque du soleil : son rayon apparent en radians.
        const auto sunRadius = [](float degrees) { return 0.5f * std::clamp(degrees, 0.0f, 45.0f) * RadiansPerDegree; };
        if (!component) {
            // Pas de fond (luminosité nulle) : seulement l'air d'une planète s'il y en a, et le disque d'un Soleil posé.
            lighting.toSun = sunDirection.value_or(SunDirection(DefaultSunElevation, DefaultSunAzimuth));
            lighting.sunColor = sun ? sunLight(sun->lightColor, sun->illuminance) : DefaultSunColor;
            lighting.sunRadius = sunRadius(sun ? sun->size : SkyComponent{}.sunSize);
            data.toSun = ToArray(lighting.toSun, sun ? lighting.sunRadius : 0.0f);
            data.sunColor = ToArray(lighting.sunColor, preExposure);
        } else {
            const SkyComponent& sky = *component;
            lighting.toSun = sunDirection.value_or(SunDirection(sky.sunElevation, sky.sunAzimuth));
            lighting.sunColor = sun ? sunLight(sun->lightColor, sun->illuminance) : sunLight(sky.sunColor, sky.sunIlluminance);
            const float brightness = Photometry::FromNits(std::max(sky.luminance, 0.0f));
            // Triche voulue : la lumière ambiante et les reflets du ciel gardent l'échelle d'avant les unités physiques
            // (fond par défaut = 1). En vrai, le fond étoilé n'éclaire presque rien (côté à l'ombre noir dans l'espace) ;
            // ainsi, « Lumière ambiante » débouche les ombres, et suit encore la luminance réglée du fond.
            lighting.ambient = std::max(sky.ambient, 0.0f) * std::max(sky.luminance, 0.0f) / SkyComponent{}.luminance;

            const bool usesPanorama = !sky.image.empty() && !assetsDirectory.empty();
            if (usesPanorama) {
                if (const Panorama& panorama = LoadPanorama(assetsDirectory / Platform::PathFromUtf8(sky.image)); panorama.texture) {
                    panoramaTexture = panorama.texture.get();
                    environment.kind = EnvironmentSource::Kind::Panorama;
                    environment.panorama = panoramaTexture;
                } else {
                    environment.top = environment.bottom = {}; // image illisible : ni fond ni lumière du ciel
                }
            } else {
                generated = MakeSkyParameters(sky);
            }

            const SkyParameters parameters = generated ? *generated : MakeSkyParameters(sky);
            lighting.sunRadius = sunRadius(sun ? sun->size : sky.sunSize);
            data.toSun = ToArray(lighting.toSun, lighting.sunRadius);
            data.sunColor = ToArray(lighting.sunColor, preExposure);
            data.nebulaColor = ToArray(parameters.nebulaColor, parameters.nebula);
            data.nebulaColor2 = ToArray(parameters.nebulaColor2, parameters.galaxy);
            data.galaxyAxis = ToArray(parameters.galaxyAxis, std::clamp(sky.stars, 0.0f, 1.0f));
            // Une étoile occupe un pixel : sa luminance y est grande (Sirius, vue à l'œil : une centaine de cd/m² sur
            // une minute d'arc). Les plus claires (éclat 5 dans Sky.slang) à 100 cd/m². C'est encore 8 stops sous
            // une station blanche au soleil, sur laquelle l'exposition se règle : comme sur une photo, plus d'étoiles.
            // Triche voulue (choix de Bruno, comme Star Citizen semble le faire) : un plancher à l'écran, les plus
            // claires jamais sous 1,5 après l'exposition (le blanc est vers 1). Sky.slang les efface devant un air
            // éclairé.
            constexpr float StarNits = 20.0f;
            constexpr float StarScreenFloor = 0.3f;
            const float starScale = std::max(Photometry::FromNits(StarNits), StarScreenFloor / std::max(preExposure, 1.0e-6f));
            data.galaxyCenter = ToArray(parameters.galaxyCenter, std::max(sky.starBrightness, 0.0f) * starScale);
            // Triche voulue (choix de Bruno, comme pour les étoiles) : les nébuleuses, vraies, sont trop faibles pour
            // l'œil réglé sur une station éclairée ; le fond garde un plancher à l'écran (les plus claires vers 0,1
            // après l'exposition avec la luminance par défaut), proportionnel à la luminance réglée : elle reste le
            // réglage de l'éclat du fond. Le fond seul : la lumière ambiante du ciel (lighting.ambient) reste réelle.
            // Pas de boucle avec l'exposition : dans l'espace le ciel n'est pas mesuré, et sur une planète il reste
            // sous le gris moyen.
            // Le plancher ne grandit pas au-delà de la luminance par défaut : un panorama de jour (milliers de cd/m²)
            // est assez lumineux par lui-même, et le plancher multiplié d'autant le brûlait en blanc.
            constexpr float BackgroundScreenFloor = 0.3f;
            const float floor = BackgroundScreenFloor * std::clamp(sky.luminance, 0.0f, SkyComponent{}.luminance)
                              / SkyComponent{}.luminance;
            data.noiseOffset = ToArray(parameters.noiseOffset, std::max(brightness, floor / std::max(preExposure, 1.0e-6f)));
            data.options = {parameters.seed, panoramaTexture != m_black.get() ? 1u : 0u, 0, 0};
        }
        // Le disque d'un Soleil posé a sa propre couleur ; sa lumière, la sienne (blanche par défaut).
        data.discColor = ToArray(sun ? sunLight(sun->color, sun->illuminance) : lighting.sunColor, 0.0f);
        data.options[2] = static_cast<std::uint32_t>(sun ? sun->rays : SunRays::Star);
        lighting.atmosphere = PrepareAtmosphere(scene, camera, lighting.sunColor);
        data.atmosphere = lighting.atmosphere;
        m_visible = component != nullptr || lighting.atmosphere.mie[3] > 0.0f;
        m_tablesWanted = lighting.atmosphere.scale[1] > 0.0f;
        lighting.airRayleigh = m_airRayleigh.get();
        lighting.airMie = m_airMie.get();
        lighting.airSampler = m_cubeSampler.get();
        lighting.atmosphereMultiple = m_multiple.get();
        if (m_boundMultiple[frameIndex] != m_multiple.get()) {
            m_groups[frameIndex]->SetTexture(MultipleSlot, *m_multiple, *m_cubeSampler);
            m_boundMultiple[frameIndex] = m_multiple.get();
        }
        if (m_visible || m_tablesWanted || !m_tablesFilled) {
            m_buffers[frameIndex]->Write(&data, sizeof(data));
        }

        // Fond généré : recalculé (Bake) pour de nouveaux réglages, lu ensuite tant qu'ils ne changent pas.
        const RHI::Texture* backgroundTexture = m_blackCube.get();
        if (generated) {
            const auto now = std::chrono::steady_clock::now();
            if (!m_bakedParameters || (*m_bakedParameters != *generated && now - m_lastBake >= MinRebuildInterval)) {
                m_bakedParameters = generated;
                m_bakePending = true;
                m_lastBake = now;
                ++m_backgroundRevision; // calculé à cette image, avant l'environnement
            }
            backgroundTexture = m_background.get();
            environment.kind = EnvironmentSource::Kind::Generated;
            environment.background = m_background.get();
            environment.revision = m_backgroundRevision;
        }
        if (m_boundBackground[frameIndex] != backgroundTexture) {
            m_groups[frameIndex]->SetTexture(BackgroundSlot, *backgroundTexture, *m_cubeSampler);
            m_boundBackground[frameIndex] = backgroundTexture;
        }
        if (m_boundPanorama[frameIndex] != panoramaTexture) {
            m_groups[frameIndex]->SetTexture(PanoramaSlot, *panoramaTexture, *m_panoramaSampler);
            m_boundPanorama[frameIndex] = panoramaTexture;
        }
        return lighting;
    }

    Planetology::AtmosphereGpu SkyRenderer::PrepareAtmosphere(const Scene& scene, const Camera& camera, const Math::Vector3f& sunColor) {
        // La planète dont l'air est le plus proche (on est dedans, ou c'est celui qu'on approche). Au-delà de 100 000 km,
        // elle ne fait que quelques pixels : ignorée (et les floats du shader n'y suffiraient plus).
        constexpr double MaxDistance = 1.0e8;
        Planetology::AtmosphereGpu gpu;
        std::optional<Planetology::AtmosphereParameters> chosen;
        Math::Vector3d chosenCenter;
        double chosenGround = 0.0;
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
                // Le sol le plus bas possible (la mer, caméra au-dessus d'elle) : le rayon du ciel n'est pas le sol. Sous
                // l'horizon de ce rayon, une plaine plus basse laissait voir le ciel peint en sol noir, en bande.
                chosenGround = std::max(static_cast<double>(parameters.radius) - planet.relief, 1.0);
                if (const double sea = Planetology::PlanetRenderer::GetSeaRadius(planet);
                    planet.ocean && sea > chosenGround && Math::Length(center) > sea) {
                    chosenGround = sea;
                }
            }
        }
        if (!chosen) {
            return gpu;
        }
        const Planetology::AtmosphereParameters& a = *chosen;
        // Diffusion multiple : quelques millisecondes réparties sur les fils, refaite seulement quand l'air change (pas
        // avec le soleil). Gardée en table CPU pour la lumière ambiante, et envoyée au GPU (16 Ko).
        if (m_multipleParameters != a) {
            m_multipleTable = Planetology::ComputeAtmosphereMultiple(a, m_jobs);
            m_multipleParameters = a;
            std::array<std::array<float, 4>, Planetology::AtmosphereMultipleSize * Planetology::AtmosphereMultipleSize> texels{};
            for (std::size_t i = 0; i < texels.size(); ++i) {
                texels[i] = ToArray(m_multipleTable[i]);
            }
            constexpr std::uint32_t size = Planetology::AtmosphereMultipleSize;
            if (auto texture = m_device.CreateTexture(RHI::Format::RGBA32Float, {size, size}, Bytes(texels))) {
                m_device.Retire([old = std::move(m_multiple)]() mutable { old.reset(); });
                m_multiple = std::move(*texture);
            } else {
                Log::Error("ciel : diffusion multiple : {}", texture.error());
            }
            m_ambientParameters.reset(); // la lumière ambiante en dépend
        }
        // Lumière ambiante du ciel : quelques millisecondes, refaite seulement quand l'air ou le soleil changent.
        if (m_ambientParameters != a || m_ambientSunColor != sunColor) {
            m_ambientTable = Planetology::ComputeAtmosphereAmbient(a, sunColor, &m_multipleTable);
            m_ambientParameters = a;
            m_ambientSunColor = sunColor;
        }
        gpu.center = {static_cast<float>(chosenCenter.x), static_cast<float>(chosenCenter.y), static_cast<float>(chosenCenter.z), a.radius};
        gpu.rayleigh = ToArray(a.rayleigh, a.rayleighFalloff);
        gpu.mie = {a.mie, a.mieFalloff, a.mieAnisotropy, a.top};
        // Tables de l'air seulement dans l'air : vu de l'espace, il ne couvre qu'un anneau trop fin pour elles.
        const double top = static_cast<double>(a.top);
        const double ground = static_cast<double>(a.radius);
        const bool inAir = Math::Length(chosenCenter) < top;
        const double longest = 2.0 * std::sqrt(std::max(top * top - ground * ground, 1.0)); // le long du sol, d'un bord à l'autre
        gpu.scale = {a.intensity, inAir ? 1.0f : 0.0f, static_cast<float>(chosenGround), static_cast<float>(std::log1p(longest / AirNear))};
        for (std::size_t i = 0; i < m_ambientTable.size(); ++i) {
            gpu.ambient[i] = ToArray(m_ambientTable[i]);
        }
        return gpu;
    }

    void SkyRenderer::BakeTables(RHI::CommandList& commands, std::uint32_t frameIndex) {
        const auto table = [&](const RHI::Pipeline& pipeline, RHI::Texture& rayleigh, RHI::Texture& mie) {
            const RHI::Extent size = rayleigh.GetExtent();
            commands.BeginRenderPass({.clearColor = {}, .colorTarget = &rayleigh, .secondColorTarget = &mie});
            commands.SetViewport({0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height)});
            commands.SetScissor({0, 0, size.width, size.height});
            commands.BindPipeline(pipeline);
            commands.SetBindGroup(0, *m_groups[frameIndex]);
            commands.Draw(3, 0);
            commands.EndRenderPass();
        };
        table(*m_skyTablePipeline, *m_skyRayleigh, *m_skyMie);
        table(*m_airTablePipeline, *m_airRayleigh, *m_airMie);
        m_tablesFilled = true;
    }

    void SkyRenderer::BakeBackground(RHI::CommandList& commands, std::uint32_t frameIndex) {
        m_bakePending = false;
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
