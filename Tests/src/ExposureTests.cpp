// Exposition automatique et courbe de tons : la mesure tirée de l'histogramme (Exposure.cpp), l'adaptation de l'œil,
// AgX et son inverse (ToneCurve.cpp, couleur de fond des scènes sans ciel) ; puis l'histogramme calculé par le GPU
// (ExposureRenderer, shaders/Exposure.slang) sur une image unie. La partie GPU est ignorée (réussie) sur une machine
// sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Flux/Renderer/RHI/RHI.hpp"
#include "Renderer/Exposure.hpp"
#include "Renderer/ExposureRenderer.hpp"
#include "Renderer/Photometry.hpp"
#include "Renderer/ToneCurve.hpp"

#include <cmath>
#include <cstdio>

using namespace Flux;

namespace {
    // Case de l'histogramme d'une luminance, comme BinOf dans Exposure.slang.
    std::uint32_t BinOf(float luminance) {
        const float t = (std::log2(luminance) - Exposure::MinLog2) / (Exposure::MaxLog2 - Exposure::MinLog2);
        return 1 + static_cast<std::uint32_t>(std::min(t, 0.99999f) * static_cast<float>(Exposure::Bins - 1));
    }

    bool NearStops(float value, float expected, float stops) {
        if (value > 0.0f && std::abs(std::log2(value / expected)) <= stops) {
            return true;
        }
        std::printf("       %.4f au lieu de %.4f (à %.2f stop près)\n", value, expected, stops);
        return false;
    }
}

bool RunExposureTests() {
    constexpr float HalfBin = 0.5f * (Exposure::MaxLog2 - Exposure::MinLog2) / static_cast<float>(Exposure::Bins - 1);
    // Image noire (case 0 seulement) : rien à mesurer.
    Exposure::Histogram histogram{};
    histogram[0] = 1000;
    CHECK(!Exposure::MeteredLuminance(histogram).has_value());
    // Image unie : sa luminance, au milieu de sa case.
    histogram[BinOf(0.5f)] = 400;
    CHECK(NearStops(Exposure::MeteredLuminance(histogram).value_or(0.0f), 0.5f, HalfBin));
    // Le plus sombre (40 % du poids) et le plus clair (10 %) ne comptent pas : seule la luminance du milieu reste.
    histogram = {};
    histogram[BinOf(0.001f)] = 40;
    histogram[BinOf(0.2f)] = 50;
    histogram[BinOf(500.0f)] = 10;
    CHECK(NearStops(Exposure::MeteredLuminance(histogram).value_or(0.0f), 0.2f, HalfBin));

    // Exposition visée : le gris moyen, décalé de la compensation, dans les bornes.
    Exposure::Settings settings{.automatic = true, .compensation = 0.0f, .minimum = -4.0f, .maximum = 1.0f, .speed = 3.0f};
    CHECK(NearStops(Exposure::Target(0.18f, settings), 1.0f, 0.001f));
    CHECK(NearStops(Exposure::Target(0.36f, settings), 0.5f, 0.001f));
    CHECK(NearStops(Exposure::Target(1.0e-4f, settings), 2.0f, 0.001f)); // plafond : +1 stop
    CHECK(NearStops(Exposure::Target(1000.0f, settings), 1.0f / 16.0f, 0.001f)); // plancher : -4 stops
    settings.compensation = 1.0f;
    CHECK(NearStops(Exposure::Target(0.36f, settings), 1.0f, 0.001f));

    // Adaptation : vers plus clair (exposition qui baisse), 95 % du chemin en une seconde à la vitesse 3 ; vers plus
    // sombre, trois fois moins vite ; jamais au-delà de la cible.
    float exposure = 1.0f;
    for (int i = 0; i < 10; ++i) {
        exposure = Exposure::Adapt(exposure, 0.25f, 0.1f, settings);
    }
    CHECK(NearStops(exposure, 0.25f * std::exp2(2.0f * std::exp(-3.0f)), 0.01f));
    const float darker = Exposure::Adapt(1.0f, 4.0f, 0.1f, settings);
    const float brighter = Exposure::Adapt(1.0f, 0.25f, 0.1f, settings);
    CHECK(darker > 1.0f && brighter < 1.0f && std::log2(darker) < -std::log2(brighter) * 0.5f);
    CHECK(NearStops(Exposure::Adapt(0.5f, 0.5f, 0.1f, settings), 0.5f, 0.0001f));

    // Unités physiques : le soleil d'avant (1,45) vaut 116 000 lux ; 1600 lumens éclairent de 127 lux à 1 m ;
    // l'exposition 1 est celle d'un appareil réglé à EV100 14,4, chaque EV de plus la divise par deux.
    CHECK(std::abs(Photometry::FromLux(116000.0f) - 1.45f) < 1.0e-5f);
    CHECK(std::abs(Photometry::FromLumens(1600.0f) * Photometry::LuxPerUnit - 127.32f) < 0.01f);
    CHECK(NearStops(Photometry::ExposureFromEv100(14.373f), 1.0f, 0.001f));
    CHECK(NearStops(Photometry::ExposureFromEv100(15.373f), 0.5f, 0.001f));
    // Une surface blanche au plein soleil (EV100 15) reste sous le blanc de la courbe de tons.
    CHECK(Photometry::FromLux(116000.0f) * Photometry::ExposureFromEv100(15.0f) < 1.0f);

    // AgX : noir reste noir, gris moyen à 0,17 (ACES avant lui : 0,14), blanc pur jamais atteint.
    const Math::LinearColor black = ToneCurve::ToneMap({0.0f, 0.0f, 0.0f}, 1.0f);
    const Math::LinearColor gray = ToneCurve::ToneMap({0.18f, 0.18f, 0.18f}, 1.0f);
    const Math::LinearColor bright = ToneCurve::ToneMap({100.0f, 100.0f, 100.0f}, 1.0f);
    CHECK(black.r < 0.001f);
    CHECK(gray.r > 0.15f && gray.r < 0.19f && std::abs(gray.g - gray.r) < 0.001f);
    CHECK(bright.r > 0.98f && bright.r <= 1.0f);
    // Une couleur saturée très forte tourne au blanc : ses composantes se rapprochent.
    const Math::LinearColor red = ToneCurve::ToneMap({50.0f, 0.5f, 0.5f}, 1.0f);
    CHECK(red.g > 0.3f * red.r);
    // Inverse : la couleur de fond choisie ressort de la courbe telle quelle, à toute exposition.
    for (const Math::LinearColor color : {Math::LinearColor{0.1f, 0.12f, 0.15f}, Math::LinearColor{0.6f, 0.3f, 0.2f},
                                          Math::LinearColor{0.9f, 0.9f, 0.9f}}) {
        for (const float scale : {0.25f, 1.0f, 2.0f}) {
            const Math::LinearColor back = ToneCurve::ToneMap(ToneCurve::InverseToneMap(color, scale), scale);
            if (std::abs(back.r - color.r) > 0.002f || std::abs(back.g - color.g) > 0.002f || std::abs(back.b - color.b) > 0.002f) {
                std::printf("       (%.3f, %.3f, %.3f) redonne (%.4f, %.4f, %.4f) à l'exposition %.2f\n", color.r, color.g,
                            color.b, back.r, back.g, back.b, scale);
                return false;
            }
        }
    }
    return true;
}

bool RunExposureGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("exposition", device.error().c_str());
    }
    auto exposure = ExposureRenderer::Create(**device);
    auto scene = (**device).CreateRenderTarget(RHI::Format::RGBA16Float, {64, 36}, true);
    if (!exposure || !scene) {
        std::printf("ÉCHEC %s\n", !exposure ? exposure.error().c_str() : scene.error().c_str());
        return false;
    }
    (*exposure)->Bind(**scene);
    // L'image écrite telle que la scène l'écrirait : la lumière multipliée par la pré-exposition.
    const auto measure = [&](Math::LinearColor color, float preExposure = 1.0f) {
        return (**device).Submit([&](RHI::CommandList& commands) {
            const Math::LinearColor written{color.r * preExposure, color.g * preExposure, color.b * preExposure};
            commands.BeginRenderPass({.clearColor = written, .colorTarget = scene->get()});
            commands.EndRenderPass();
            commands.Use(**scene, RHI::TextureAccess::Sampled);
            (*exposure)->Measure(commands, 0, preExposure);
        });
    };
    const Exposure::Settings settings{.automatic = true, .compensation = 0.0f, .minimum = -4.0f, .maximum = 1.0f, .speed = 3.0f};
    constexpr float HalfBin = 0.5f * (Exposure::MaxLog2 - Exposure::MinLog2) / static_cast<float>(Exposure::Bins - 1);

    // Image unie de luminance 0,735 (rouge, vert, bleu pondérés par 0,2126, 0,7152 et 0,0722) : exposition
    // 0,18 / 0,735, environ 1/4, prise d'un coup à la première mesure.
    CHECK(measure({0.9f, 0.7f, 0.6f}));
    const float luminance = 0.2126f * 0.9f + 0.7152f * 0.7f + 0.0722f * 0.6f;
    const float first = (*exposure)->Update(settings, 0.016f, false, 0);
    std::printf("       luminance mesurée %.4f, exposition %.4f\n", (*exposure)->GetLuminance(), first);
    CHECK(NearStops((*exposure)->GetLuminance(), luminance, HalfBin + 0.01f));
    CHECK(NearStops(first, 0.18f / luminance, HalfBin + 0.01f));
    // La même image écrite avec une pré-exposition de 4 : la même luminance mesurée.
    // (Pas tout à fait la même case de l'histogramme : l'exposition s'adapte d'un rien, d'où `steady`.)
    CHECK(measure({0.9f, 0.7f, 0.6f}, 4.0f));
    const float steady = (*exposure)->Update(settings, 0.016f, false, 0);
    CHECK(NearStops(steady, first, HalfBin + 0.01f));
    CHECK(NearStops((*exposure)->GetLuminance(), luminance, HalfBin + 0.01f));
    // Sans nouvelle mesure, l'exposition ne bouge pas.
    CHECK(NearStops((*exposure)->Update(settings, 0.016f, false, 0), steady, 0.0001f));
    // Image noire (le fond de l'espace) : rien de mesurable, l'œil s'ouvre vers sa limite, doucement.
    CHECK(measure({0.0f, 0.0f, 0.0f}));
    const float opening = (*exposure)->Update(settings, 0.016f, false, 0);
    CHECK(opening > steady && opening < std::exp2(settings.maximum));
    CHECK((*exposure)->GetLuminance() == 0.0f);
    // Image plus sombre : l'exposition monte encore.
    CHECK(measure({0.05f, 0.05f, 0.05f}));
    const float adapting = (*exposure)->Update(settings, 0.016f, false, 0);
    CHECK(adapting > opening && adapting < 0.18f / 0.05f);
    return true;
}
