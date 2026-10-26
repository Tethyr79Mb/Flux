#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Mise à l'échelle de l'image (réglage du joueur et de l'éditeur). Natif : la scène est dessinée à la taille de
    // la vue, lissée par notre anticrénelage temporel (shaders/Temporal.slang). FSR : AMD FidelityFX Super Resolution
    // 3.1 ; la scène est dessinée plus petite (GetRenderScale de chaque côté), puis FSR l'agrandit à la taille de la vue
    // en reprenant les images précédentes. Moins de pixels à dessiner : plus d'images/s quand le GPU limite, rien
    // quand c'est le CPU. FSR Natif AA dessine à 100 % : FSR sert seulement d'anticrénelage.
    enum class Upscaling : std::uint8_t { Native, FsrNativeAA, FsrQuality, FsrBalanced, FsrPerformance, FsrUltraPerformance };
    constexpr Upscaling DefaultUpscaling = Upscaling::FsrQuality; // choix de Bruno du 2026-10-25
    constexpr std::size_t UpscalingCount = 6;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, UpscalingCount> UpscalingKeys{"native", "fsr-native-aa", "fsr-quality",
                                                                         "fsr-balanced", "fsr-performance",
                                                                         "fsr-ultra-performance"};
    constexpr std::array<std::string_view, UpscalingCount> UpscalingLabels{"Natif (TAA)", "FSR Natif AA", "FSR Qualité",
                                                                           "FSR Équilibré", "FSR Performance",
                                                                           "FSR Ultra Performance"};
    // Netteté ajoutée par FSR après l'agrandissement (RCAS), de 0 à 1.
    constexpr float DefaultSharpness = 0.5f;

    constexpr bool IsFsr(Upscaling upscaling) {
        return upscaling != Upscaling::Native;
    }

    // Taille dessinée sur taille de la vue, de chaque côté (rapports d'AMD : 1,5 ; 1,7 ; 2 ; 3).
    constexpr float GetRenderScale(Upscaling upscaling) {
        switch (upscaling) {
            case Upscaling::FsrQuality: return 1.0f / 1.5f;
            case Upscaling::FsrBalanced: return 1.0f / 1.7f;
            case Upscaling::FsrPerformance: return 0.5f;
            case Upscaling::FsrUltraPerformance: return 1.0f / 3.0f;
            case Upscaling::Native:
            case Upscaling::FsrNativeAA: break;
        }
        return 1.0f;
    }
}
