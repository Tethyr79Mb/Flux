#include "Renderer/Exposure.hpp"

#include <algorithm>
#include <cmath>

namespace Flux::Exposure {
    namespace {
        constexpr double LowFraction = 0.4;
        constexpr double HighFraction = 0.9;
        constexpr float MiddleGray = 0.18f;
        constexpr float DarkeningSlowdown = 3.0f;
        // Au-delà, une image figée (chargement, point d'arrêt) ferait sauter l'exposition d'un coup.
        constexpr float MaxStep = 0.25f;
    }

    std::optional<float> MeteredLuminance(const Histogram& histogram) {
        double total = 0.0;
        for (std::uint32_t bin = 1; bin < Bins; ++bin) {
            total += histogram[bin];
        }
        if (total <= 0.0) {
            return std::nullopt;
        }
        // Part de chaque case comprise dans la tranche [low, high] du poids cumulé.
        const double low = total * LowFraction;
        const double high = total * HighFraction;
        double below = 0.0;
        double weight = 0.0;
        double sum = 0.0;
        constexpr double BinWidth = (MaxLog2 - MinLog2) / static_cast<double>(Bins - 1);
        for (std::uint32_t bin = 1; bin < Bins; ++bin) {
            const double count = histogram[bin];
            const double kept = std::min(below + count, high) - std::max(below, low);
            below += count;
            if (kept > 0.0) {
                const double log2 = MinLog2 + (static_cast<double>(bin) - 0.5) * BinWidth; // milieu de la case
                weight += kept;
                sum += kept * log2;
            }
        }
        return static_cast<float>(std::exp2(sum / weight));
    }

    float Target(float luminance, const Settings& settings) {
        const float stops = std::log2(MiddleGray / std::max(luminance, 1.0e-6f)) + settings.compensation;
        return std::exp2(std::clamp(stops, settings.minimum, std::max(settings.minimum, settings.maximum)));
    }

    float Adapt(float current, float target, float seconds, const Settings& settings) {
        const float from = std::log2(std::max(current, 1.0e-6f));
        const float to = std::log2(std::max(target, 1.0e-6f));
        // Exposition qui baisse : la scène s'éclaire, l'œil suit vite ; qui monte : il lui faut plus de temps.
        const float speed = std::max(settings.speed, 0.0f) * (to < from ? 1.0f : 1.0f / DarkeningSlowdown);
        const float t = 1.0f - std::exp(-speed * std::clamp(seconds, 0.0f, MaxStep));
        return std::exp2(from + (to - from) * t);
    }
}
