#include "Flux/Planetology/Atmosphere.hpp"

#include "Flux/Core/JobSystem.hpp"
#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::Planetology {
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
        // Pi : unités du moteur (voir Atmosphere.hpp). Relevée de 2 pour garder le ciel de jour à côté d'un sol au
        // soleil tel qu'il était réglé (3 avant la diffusion multiple, qui éclaircit le ciel de 1,2 à 2,4 fois).
        constexpr float Brightness = 2.0f * std::numbers::pi_v<float>;

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

        Math::Vector3f Multiply(const Math::Vector3f& a, const Math::Vector3f& b) {
            return {a.x * b.x, a.y * b.y, a.z * b.z};
        }

        // Diffusion multiple (ComputeAtmosphereMultiple) : le sol renvoie cette part de la lumière, diffusément (0,3 :
        // moyenne des terres, comme Hillaire) ; regards en 8 x 8 directions sur la sphère, 20 pas chacun.
        constexpr float GroundAlbedo = 0.3f;
        constexpr int MultipleRings = 8;
        constexpr int MultipleSegments = 8;
        constexpr int MultipleSteps = 20;
        // L'air très épais renvoie presque tout (1 / (1 - f) sans fin) : borne de la part rediffusée.
        constexpr float MultipleTransferMax = 0.95f;

        // Lecture bilinéaire de la table, comme AtmosphereMultipleLight dans shaders/Atmosphere.slang.
        Math::Vector3f MultipleLight(const AtmosphereParameters& a, const AtmosphereMultipleTable& table, float height, float cosine) {
            constexpr int Size = AtmosphereMultipleSize;
            constexpr float Last = static_cast<float>(Size - 1);
            const float x = std::clamp(cosine * 0.5f + 0.5f, 0.0f, 1.0f) * Last;
            const float y = std::clamp(height / (a.top - a.radius), 0.0f, 1.0f) * Last;
            const int x0 = std::min(static_cast<int>(x), Size - 2);
            const int y0 = std::min(static_cast<int>(y), Size - 2);
            const float fx = x - static_cast<float>(x0);
            const float fy = y - static_cast<float>(y0);
            const auto at = [&](int row, int column) { return table[static_cast<std::size_t>(row * Size + column)]; };
            const Math::Vector3f low = at(y0, x0) * (1.0f - fx) + at(y0, x0 + 1) * fx;
            const Math::Vector3f high = at(y0 + 1, x0) * (1.0f - fx) + at(y0 + 1, x0 + 1) * fx;
            return low * (1.0f - fy) + high * fy;
        }
    }

    AtmosphereMultipleTable ComputeAtmosphereMultiple(const AtmosphereParameters& a, JobSystem* jobs) {
        AtmosphereMultipleTable table{};
        if (a.top <= 0.0f) {
            return table;
        }
        constexpr int Size = AtmosphereMultipleSize;
        const float thickness = a.top - a.radius;
        // Le plan vertical qui contient le soleil est un plan de symétrie : la moitié des directions, comptée deux fois.
        constexpr int HalfSegments = MultipleSegments / 2;
        constexpr float Weight = 1.0f / static_cast<float>(MultipleRings * HalfSegments);
        const auto rows = [&a, &table, thickness](std::uint32_t first, std::uint32_t last) {
            for (int row = static_cast<int>(first); row < static_cast<int>(last); ++row) {
                // Un mètre au-dessus du sol au moins, un sous le haut au plus : un regard horizontal y traverse encore de l'air.
                const float height = std::clamp(static_cast<float>(row) / static_cast<float>(Size - 1) * thickness, 1.0f, thickness - 1.0f);
                const Math::Vector3f origin{0.0f, a.radius + height, 0.0f};
                for (int column = 0; column < Size; ++column) {
                    const float cosine = -1.0f + 2.0f * static_cast<float>(column) / static_cast<float>(Size - 1);
                    const Math::Vector3f toSun{std::sqrt(std::max(0.0f, 1.0f - cosine * cosine)), cosine, 0.0f};
                    // light : la lumière diffusée une fois qui arrive de chaque regard (phase isotrope), sol compris ;
                    // transfer : la part d'une lumière isotrope rediffusée vers le point. Hillaire, équations 5 à 9.
                    Math::Vector3f light;
                    Math::Vector3f transfer;
                    for (int ring = 0; ring < MultipleRings; ++ring) {
                        const float up = 1.0f - 2.0f * (static_cast<float>(ring) + 0.5f) / static_cast<float>(MultipleRings);
                        const float sine = std::sqrt(std::max(0.0f, 1.0f - up * up));
                        for (int segment = 0; segment < HalfSegments; ++segment) {
                            const float angle = Pi * (static_cast<float>(segment) + 0.5f) / static_cast<float>(HalfSegments);
                            const Math::Vector3f direction{sine * std::cos(angle), up, sine * std::sin(angle)};
                            float t0 = 0.0f;
                            float end = 0.0f;
                            RaySphere(origin, direction, a.top, t0, end);
                            float g0 = 0.0f;
                            float g1 = 0.0f;
                            const bool ground = RaySphere(origin, direction, a.radius, g0, g1) && g0 > 0.0f;
                            if (ground) {
                                end = g0;
                            }
                            const float step = end / static_cast<float>(MultipleSteps);
                            float viewRayleigh = 0.0f;
                            float viewMie = 0.0f;
                            for (int i = 0; i < MultipleSteps; ++i) {
                                const Math::Vector3f point = origin + direction * ((static_cast<float>(i) + 0.5f) * step);
                                const float h = Math::Length(point) - a.radius;
                                const float rayleigh = std::exp(-h * a.rayleighFalloff) * step;
                                const float mie = std::exp(-h * a.mieFalloff) * step;
                                viewRayleigh += 0.5f * rayleigh;
                                viewMie += 0.5f * mie;
                                const Math::Vector3f view = Extinction(a, viewRayleigh, viewMie);
                                const Math::Vector3f scattered = Multiply(view, a.rayleigh * rayleigh + Math::Vector3f{1.0f, 1.0f, 1.0f} * (a.mie * mie));
                                transfer += scattered;
                                float sunRayleigh = 0.0f;
                                float sunMie = 0.0f;
                                const float visible = SunDepth(a, point, toSun, 1.0f / a.rayleighFalloff, 0.0f, sunRayleigh, sunMie);
                                if (visible > 0.0f) {
                                    light += Multiply(scattered, Extinction(a, sunRayleigh, sunMie)) * (visible / (4.0f * Pi));
                                }
                                viewRayleigh += 0.5f * rayleigh;
                                viewMie += 0.5f * mie;
                            }
                            if (ground) {
                                const Math::Vector3f point = origin + direction * end;
                                const float facing = Math::Dot(point, toSun) / Math::Length(point);
                                if (facing > 0.0f) {
                                    light += Multiply(Extinction(a, viewRayleigh, viewMie), SunTransmittance(a, point, toSun))
                                             * (facing * GroundAlbedo / Pi);
                                }
                            }
                        }
                    }
                    // Moyennes sur la sphère (phase isotrope) ; 1 / (1 - f) : la somme de toutes les diffusions suivantes.
                    const Math::Vector3f mean = light * Weight;
                    const Math::Vector3f f = transfer * Weight;
                    const auto sum = [](float v) { return 1.0f / (1.0f - std::min(v, MultipleTransferMax)); };
                    table[static_cast<std::size_t>(row * Size + column)] = {mean.x * sum(f.x), mean.y * sum(f.y), mean.z * sum(f.z)};
                }
            }
        };
        if (jobs) {
            jobs->ParallelFor(Size, 2, rows);
        } else {
            rows(0, Size);
        }
        return table;
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
                           const Math::Vector3f& toSun, const Math::Vector3f& sunColor, int steps, Math::Vector3f& transmittance,
                           const AtmosphereMultipleTable* multiple) {
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
        Math::Vector3f sumMultiple; // diffusion multiple, isotrope : sans phase
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
            if (multiple) {
                const Math::Vector3f scattering = a.rayleigh * rayleigh + Math::Vector3f{1.0f, 1.0f, 1.0f} * (a.mie * mie);
                const Math::Vector3f received = MultipleLight(a, *multiple, h, Math::Dot(point, toSun) / Math::Length(point));
                sumMultiple += Multiply(Multiply(Extinction(a, viewRayleigh, viewMie), scattering), received);
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
        const Math::Vector3f light = scattered + sumMie * (a.mie * miePhase) + sumMultiple;
        return Math::Vector3f{light.x * sunColor.x, light.y * sunColor.y, light.z * sunColor.z} * a.intensity;
    }

    std::array<Math::Vector3f, AtmosphereAmbientSize> ComputeAtmosphereAmbient(const AtmosphereParameters& a,
                                                                               const Math::Vector3f& sunColor,
                                                                               const AtmosphereMultipleTable* multiple) {
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
                    sum += Scatter(a, origin, direction, 1.0e30f, toSun, sunColor, AtmosphereSurfaceSteps, transmittance, multiple);
                }
            }
            table[static_cast<std::size_t>(entry)] = sum * (1.0f / static_cast<float>(Rings * Segments));
        }
        return table;
    }
}
