#include "Flux/Graphics/Atmosphere.hpp"

#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux {
    namespace {
        // Terre : épaisseur optique verticale de l'air pour le bleu (diffusion de Rayleigh 33,1e-6 / m sur une hauteur
        // d'échelle de 8 km), et des poussières (Mie 21e-6 / m sur 1,2 km). Une planète garde ces épaisseurs (le même
        // ciel), répartie sur la hauteur de son atmosphère.
        constexpr float EarthRayleighDepth = 33.1e-6f * 8000.0f;
        constexpr float EarthMieDepth = 0.6f * 21e-6f * 1200.0f; // un peu moins de brume
        // Presque tout l'air est sous le huitième de la hauteur donnée (e^-8 en haut) ; les poussières plus bas encore.
        constexpr float ScaleHeights = 8.0f;
        constexpr float MieHeightRatio = 0.15f;
        // Les poussières absorbent un peu en plus de diffuser.
        constexpr float MieExtinction = 1.1f;
        // Une seule diffusion sous-estime le ciel (la lumière diffusée plusieurs fois l'éclaire aussi) : relevée pour
        // retrouver un ciel de jour à peu près aussi clair que sur Terre, à côté d'un sol au soleil. Pi : unités du
        // moteur (voir Atmosphere.hpp).
        constexpr float Brightness = 3.0f * std::numbers::pi_v<float>;

        constexpr float Pi = std::numbers::pi_v<float>;

        // Entrée et sortie (distances le long de `direction`) de la sphère de centre l'origine ; false si elle est manquée.
        bool RaySphere(const Math::Vector3f& origin, const Math::Vector3f& direction, float radius, float& t0, float& t1) {
            const float b = Math::Dot(origin, direction);
            const float c = Math::Dot(origin, origin) - radius * radius;
            const float discriminant = b * b - c;
            if (discriminant < 0.0f) {
                return false;
            }
            const float root = std::sqrt(discriminant);
            t0 = -b - root;
            t1 = -b + root;
            return true;
        }

        Math::Vector3f Extinction(const AtmosphereParameters& a, float rayleighDepth, float mieDepth) {
            const Math::Vector3f tau = a.rayleigh * rayleighDepth + Math::Vector3f{1.0f, 1.0f, 1.0f} * (a.mie * MieExtinction * mieDepth);
            return {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};
        }

        // Épaisseur d'air (en mètres d'air au sol) d'un point à `height` mètres du sol jusqu'à l'infini, dans une
        // direction faisant un angle de cosinus `cosine` avec sa verticale : fonction de Chapman, approchée comme
        // Schüler (GPU Pro 3) : continue, sans marche (pas de bandes au terminateur). falloff : 1 / hauteur d'échelle.
        float Column(float radius, float height, float falloff, float cosine) {
            const float x = radius * falloff;
            const float h = std::max(height, 0.0f) * falloff;
            const float c = std::sqrt(x + h);
            float depth = 0.0f;
            if (cosine >= 0.0f) {
                depth = c / (c * cosine + 1.0f) * std::exp(-h);
            } else {
                // Sous l'horizon : l'air jusqu'au point le plus bas du trajet, deux fois, moins la part derrière soi.
                const float x0 = std::sqrt(1.0f - cosine * cosine) * (x + h);
                const float c0 = std::sqrt(x0);
                depth = 2.0f * c0 * std::exp(std::min(x - x0, 80.0f)) - c / (1.0f - c * cosine) * std::exp(-h);
            }
            return depth / falloff;
        }

        // Soleil couché au sol sur cet angle (radians, environ 3°) : voir SunDepth.
        constexpr float SunsetAngle = 0.05f;

        // Épaisseurs (Rayleigh, Mie) de `position` vers le soleil, et la part de soleil non cachée par le sol (sphère du
        // rayon du sol, ou plus bas que le point : fond d'une vallée). Le soleil se couche quand son cosinus passe sous
        // celui de l'horizon de cette sphère, puis s'éteint en douceur (le test ne se fait que côté nuit : un fondu
        // commencé avant ferait une marche sur la ligne jour/nuit) : sur 2 x `soft` mètres sous le sol (dans l'air, une
        // hauteur d'échelle, sinon les pas de la marche le long du regard dessinent des arcs sur l'ombre de la planète),
        // ou si `angle` > 0, sur `angle` radians de soleil sous l'horizon (au sol : un coucher net dessinait une ligne
        // droite, la même pour tous les creux, là où les pentes tournées vers le soleil étaient encore éclairées).
        float SunDepth(const AtmosphereParameters& a, const Math::Vector3f& position, const Math::Vector3f& toSun, float soft,
                       float angle, float& rayleigh, float& mie) {
            rayleigh = mie = 0.0f;
            const float distance = std::max(Math::Length(position), 1.0f);
            const float cosine = Math::Dot(position, toSun) / distance;
            float visible = 1.0f;
            if (cosine < 0.0f) {
                const float ground = std::min(a.radius, distance);
                float t = 0.0f;
                if (angle > 0.0f) {
                    const float ratio = ground / distance;
                    const float horizon = -std::sqrt(std::max(0.0f, 1.0f - ratio * ratio)); // cosinus du soleil à l'horizon
                    t = std::clamp((cosine - (horizon - angle)) / angle, 0.0f, 1.0f);
                } else {
                    // Plus courte distance au centre le long du rayon vers le soleil.
                    const float closest = distance * std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
                    t = std::clamp((closest - (ground - 2.0f * soft)) / (2.0f * soft), 0.0f, 1.0f);
                }
                visible = t * t * (3.0f - 2.0f * t);
                if (visible <= 0.0f) {
                    return 0.0f;
                }
            }
            const float height = distance - a.radius;
            rayleigh = Column(a.radius, height, a.rayleighFalloff, cosine);
            mie = Column(a.radius, height, a.mieFalloff, cosine);
            return visible;
        }
    }

    AtmosphereParameters MakeAtmosphereParameters(const PlanetComponent& planet) {
        AtmosphereParameters a;
        const float density = std::max(planet.atmosphere, 0.0f);
        if (density <= 0.0f) {
            return a;
        }
        const float height = std::max(planet.atmosphereHeight, 100.0f);
        const float rayleighHeight = height / ScaleHeights;
        const float mieHeight = rayleighHeight * MieHeightRatio;
        a.radius = std::max(planet.radius, 1.0f);
        a.top = a.radius + height;
        const Math::Vector3f color{std::max(planet.skyColor.r, 0.0f), std::max(planet.skyColor.g, 0.0f), std::max(planet.skyColor.b, 0.0f)};
        a.rayleigh = color * (EarthRayleighDepth / rayleighHeight * density);
        a.rayleighFalloff = 1.0f / rayleighHeight;
        a.mie = EarthMieDepth / mieHeight * density;
        a.mieFalloff = 1.0f / mieHeight;
        a.intensity = Brightness;
        return a;
    }

    Math::Vector3f SunTransmittance(const AtmosphereParameters& a, const Math::Vector3f& position, const Math::Vector3f& toSun) {
        float rayleigh = 0.0f;
        float mie = 0.0f;
        const float visible = SunDepth(a, position, toSun, 0.0f, SunsetAngle, rayleigh, mie);
        if (visible <= 0.0f) {
            return {};
        }
        return Extinction(a, rayleigh, mie) * visible;
    }

    Math::Vector3f Scatter(const AtmosphereParameters& a, const Math::Vector3f& origin, const Math::Vector3f& direction, float distance,
                           const Math::Vector3f& toSun, const Math::Vector3f& sunColor, int steps, Math::Vector3f& transmittance) {
        transmittance = {1.0f, 1.0f, 1.0f};
        float t0 = 0.0f;
        float t1 = 0.0f;
        if (a.top <= 0.0f || !RaySphere(origin, direction, a.top, t0, t1) || t1 <= 0.0f) {
            return {};
        }
        const float start = std::max(t0, 0.0f);
        const float end = std::min(t1, distance);
        if (end <= start) {
            return {};
        }
        const float step = (end - start) / static_cast<float>(steps);
        float viewRayleigh = 0.0f;
        float viewMie = 0.0f;
        Math::Vector3f sumRayleigh;
        Math::Vector3f sumMie;
        for (int i = 0; i < steps; ++i) {
            const Math::Vector3f point = origin + direction * (start + (static_cast<float>(i) + 0.5f) * step);
            const float h = Math::Length(point) - a.radius;
            const float rayleigh = std::exp(-h * a.rayleighFalloff) * step;
            const float mie = std::exp(-h * a.mieFalloff) * step;
            // Épaisseur jusqu'au milieu de ce pas.
            viewRayleigh += 0.5f * rayleigh;
            viewMie += 0.5f * mie;
            float sunRayleigh = 0.0f;
            float sunMie = 0.0f;
            const float visible = SunDepth(a, point, toSun, 1.0f / a.rayleighFalloff, 0.0f, sunRayleigh, sunMie);
            if (visible > 0.0f) {
                const Math::Vector3f light = Extinction(a, viewRayleigh + sunRayleigh, viewMie + sunMie) * visible;
                sumRayleigh += light * rayleigh;
                sumMie += light * mie;
            }
            viewRayleigh += 0.5f * rayleigh;
            viewMie += 0.5f * mie;
        }
        transmittance = Extinction(a, viewRayleigh, viewMie);
        const float mu = Math::Dot(direction, toSun);
        const float rayleighPhase = 3.0f / (16.0f * Pi) * (1.0f + mu * mu);
        const float g = a.mieAnisotropy;
        const float mieDenominator = 1.0f + g * g - 2.0f * g * mu;
        const float miePhase = (1.0f - g * g) / (4.0f * Pi * mieDenominator * std::sqrt(mieDenominator));
        const Math::Vector3f scattered{sumRayleigh.x * a.rayleigh.x * rayleighPhase, sumRayleigh.y * a.rayleigh.y * rayleighPhase,
                                       sumRayleigh.z * a.rayleigh.z * rayleighPhase};
        const Math::Vector3f light = scattered + sumMie * (a.mie * miePhase);
        return Math::Vector3f{light.x * sunColor.x, light.y * sunColor.y, light.z * sunColor.z} * a.intensity;
    }

    std::array<Math::Vector3f, AtmosphereAmbientSize> ComputeAtmosphereAmbient(const AtmosphereParameters& a,
                                                                               const Math::Vector3f& sunColor) {
        std::array<Math::Vector3f, AtmosphereAmbientSize> table{};
        if (a.top <= 0.0f) {
            return table;
        }
        // Au sol (un mètre au-dessus), directions de la demi-sphère du haut tirées selon le cosinus : leur simple
        // moyenne est la lumière reçue par une surface horizontale.
        constexpr int Rings = 6;
        constexpr int Segments = 12;
        const Math::Vector3f origin{0.0f, a.radius + 1.0f, 0.0f};
        for (int entry = 0; entry < AtmosphereAmbientSize; ++entry) {
            const float cosine = -1.0f + 2.0f * static_cast<float>(entry) / static_cast<float>(AtmosphereAmbientSize - 1);
            const Math::Vector3f toSun{std::sqrt(std::max(0.0f, 1.0f - cosine * cosine)), cosine, 0.0f};
            Math::Vector3f sum;
            for (int ring = 0; ring < Rings; ++ring) {
                const float sine = std::sqrt((static_cast<float>(ring) + 0.5f) / static_cast<float>(Rings));
                const float up = std::sqrt(1.0f - sine * sine);
                for (int segment = 0; segment < Segments; ++segment) {
                    const float angle = 2.0f * Pi * (static_cast<float>(segment) + 0.5f * static_cast<float>(ring & 1)) / static_cast<float>(Segments);
                    const Math::Vector3f direction{sine * std::cos(angle), up, sine * std::sin(angle)};
                    Math::Vector3f transmittance;
                    sum += Scatter(a, origin, direction, 1.0e30f, toSun, sunColor, AtmosphereSurfaceSteps, transmittance);
                }
            }
            table[static_cast<std::size_t>(entry)] = sum * (1.0f / static_cast<float>(Rings * Segments));
        }
        return table;
    }
}
