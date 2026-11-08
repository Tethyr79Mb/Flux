#include "Planetology/TreeSowing.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::Planetology {
    namespace {
        // Bord des tuiles qui servent de plaines, au plus (m).
        constexpr double MaxPlainEdge = 400.0;

        double Saturate(double x) {
            return std::clamp(x, 0.0, 1.0);
        }

        // Comme smoothstep en Slang : aussi pour low > high (descend).
        double SmoothStep(double low, double high, double x) {
            const double t = Saturate((x - low) / (high - low));
            return t * t * (3.0 - 2.0 * t);
        }

        double Lerp(double a, double b, double t) {
            return a + (b - a) * t;
        }

        // --- Comme shaders/PlanetClimate.slang ---

        double ClimateHash(std::int32_t cx, std::int32_t cy, std::int32_t cz) {
            std::uint32_t x = static_cast<std::uint32_t>(cx) * 0x8da6b343u ^ static_cast<std::uint32_t>(cy) * 0xd8163841u
                            ^ static_cast<std::uint32_t>(cz) * 0xcb1ab31fu;
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return static_cast<double>(x >> 8) * (1.0 / 16777216.0);
        }

        double ClimateNoise(const Math::Vector3d& p) {
            const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
            const auto ix = static_cast<std::int32_t>(fx), iy = static_cast<std::int32_t>(fy), iz = static_cast<std::int32_t>(fz);
            const auto smooth = [](double t) { return t * t * (3.0 - 2.0 * t); };
            const double tx = smooth(p.x - fx), ty = smooth(p.y - fy), tz = smooth(p.z - fz);
            const auto h = [&](std::int32_t a, std::int32_t b, std::int32_t c) { return ClimateHash(ix + a, iy + b, iz + c); };
            return Lerp(Lerp(Lerp(h(0, 0, 0), h(1, 0, 0), tx), Lerp(h(0, 1, 0), h(1, 1, 0), tx), ty),
                        Lerp(Lerp(h(0, 0, 1), h(1, 0, 1), tx), Lerp(h(0, 1, 1), h(1, 1, 1), tx), ty), tz);
        }

        double LatitudeHumidity(double degrees) {
            const double equator = (degrees / 12.0) * (degrees / 12.0);
            const double tropics = ((degrees - 27.0) / 10.0) * ((degrees - 27.0) / 10.0);
            const double temperate = ((degrees - 55.0) / 13.0) * ((degrees - 55.0) / 13.0);
            return 0.3 * std::exp(-equator) - 0.35 * std::exp(-tropics) + 0.12 * std::exp(-temperate) - 0.15 * SmoothStep(65.0, 90.0, degrees);
        }

        struct Climate
        {
            double temperature;
            double humidity;
        };

        Climate ClimateAt(double radius, double relief, const std::array<float, 4>& climate, const Math::Vector3d& local) {
            relief = std::max(relief, 1.0);
            const double waterRadius = climate[3];
            const double distance = Math::Length(local);
            const double altitude = distance - radius;
            const double latitude = std::abs(local.y) / std::max(distance, 1.0);
            const double temperature = Lerp(climate[0], climate[1], (latitude * latitude) * (latitude * latitude))
                                     - 0.0065 * std::max(altitude, 0.0) + (ClimateNoise(local * (100.0 / radius)) - 0.5) * 8.0;
            const double coast = waterRadius > 0.0 ? SmoothStep(0.02 * relief, 0.0, distance - waterRadius) : 0.0;
            const Math::Vector3d offset31{31.0, 31.0, 31.0}, offset17{17.0, 17.0, 17.0}, offset5{5.0, 5.0, 5.0};
            const double wave = (ClimateNoise(local * (1.5 / radius) + offset31) - 0.5) * 16.0;
            const double humidity = climate[2] + LatitudeHumidity(std::abs(std::asin(Saturate(latitude)) * 57.29578 + wave))
                                  + (ClimateNoise(local * (2.0 / radius) + offset17) - 0.5) * 0.5
                                  + (ClimateNoise(local * (1.0 / 30000.0) + offset5) - 0.5) * 0.3 + coast * 0.3;
            return {temperature, humidity};
        }

        double ForestNoise(const Math::Vector3d& flatPoint) {
            return 0.5 + (ClimateNoise(flatPoint * (1.0 / 600.0)) - 0.5) * 0.65
                 + (ClimateNoise(flatPoint * (1.0 / 150.0) + Math::Vector3d{7.0, 7.0, 7.0}) - 0.5) * 0.35;
        }

        double ForestMask(double noise, double cover) {
            const double threshold = 0.8 - 0.6 * cover;
            constexpr double width = 0.03;
            return SmoothStep(threshold - width, threshold + width, noise);
        }

        double BiomeBand(double x, double low, double high, double soft) {
            return SmoothStep(low - soft, low + soft, x) * (1.0 - SmoothStep(high - soft, high + soft, x));
        }

        double BiomeFit(const BiomeGpu& biome, const Math::Vector3d& zoneShares, double altitude, const Climate& climate) {
            const double zones = biome.zones[0] * zoneShares.x + biome.zones[1] * zoneShares.y + biome.zones[2] * zoneShares.z;
            return Saturate(zones) * BiomeBand(altitude, biome.ranges[0], biome.ranges[1], 0.02)
                 * BiomeBand(climate.temperature, biome.ranges[2], biome.ranges[3], 2.0)
                 * BiomeBand(climate.humidity, biome.humidity[0], biome.humidity[1], 0.05);
        }

        // --- Comme shaders/PlanetTile.slang ---

        std::uint32_t TreeHash(std::uint32_t x) {
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return x;
        }

        double TreeRandom(std::uint32_t& state) {
            state = TreeHash(state + 0x9e3779b9u);
            return static_cast<double>(state >> 8) * (1.0 / 16777216.0);
        }

        // Le bloc de côté side qui contient la place (x, y) de la face la garde-t-il ? state : son hasard, qui place
        // l'arbre dans le bloc.
        bool TreeBlockKeeps(std::uint32_t seed, std::uint32_t face, std::uint32_t x, std::uint32_t y, std::uint32_t side,
                            std::uint32_t& state) {
            state = TreeHash(seed ^ TreeHash(face * 0x632be5abu ^ TreeHash((x / side) * 0x27d4eb2fu + side)
                                             ^ TreeHash(y / side) * 0x85ebca6bu) ^ 0x165667b1u);
            return x % side == state % side && y % side == (state >> 16) % side;
        }
    }

    std::uint32_t GetPlainLevel(const Terrain& terrain) {
        // Bord d'une tuile de niveau n : environ (π/2) R / 2^n.
        const double edge = 0.5 * std::numbers::pi * terrain.GetRadius();
        const double level = std::ceil(std::log2(std::max(edge / MaxPlainEdge, 1.0)));
        return std::min(static_cast<std::uint32_t>(level), terrain.GetMaxLevel());
    }

    SowSettings GetSowSettings(const Terrain& terrain, const PlanetComponent& planet) {
        const float relief = std::max(planet.relief, 0.0f);
        const float seaAltitude = planet.seaLevel * 0.01f * relief;
        SowSettings settings;
        settings.seed = static_cast<std::uint32_t>(planet.seed) * 0x9e3779b1u;
        settings.shape = {static_cast<float>(terrain.GetRadius()), relief, planet.snowLevel * 0.01f * relief,
                          seaAltitude + std::max(planet.beachHeight, 0.0f)};
        settings.climate = {planet.temperature, planet.poleTemperature, std::clamp(planet.humidity, 0.0f, 1.0f),
                            planet.ocean ? static_cast<float>(terrain.GetRadius()) + seaAltitude : 0.0f};
        return settings;
    }

    TreeBlocks GetTreeBlocks(const Terrain& terrain, const VegetationSet& set) {
        // Côté d'une place : celui d'une plaine (comme GetPlainLevel) sur TreeGrid.
        const double cell = 0.5 * std::numbers::pi * terrain.GetRadius() / std::ldexp(1.0, static_cast<int>(GetPlainLevel(terrain)))
                          / TreeGrid;
        TreeBlocks blocks;
        for (std::size_t b = 0; b < std::min<std::size_t>(set.biomes.size(), VegetationSet::MaxBiomes); ++b) {
            const double factor = b < set.biomeSpacing.size() ? set.biomeSpacing[b] : 1.0;
            for (std::size_t s = 0; s < VegetationSet::MaxSpecies; ++s) {
                const double spacing = s < set.species.size() ? set.species[s].spacing * factor : 0.0;
                const auto side = static_cast<std::uint32_t>(std::clamp(std::round(spacing / cell), 1.0, static_cast<double>(MaxTreeBlock)));
                blocks.sides[b] |= side << (4 * s);
                // Seules les espèces que le biome peut tirer comptent pour écarter des places.
                for (std::uint32_t e : set.biomeSpecies[b]) {
                    if (s >= (e & 255u) && s < (e & 255u) + std::max(e >> 8, 1u)) {
                        blocks.used |= 1u << side;
                        break;
                    }
                }
            }
        }
        return blocks;
    }

    void SowPlain(const Terrain& terrain, const SowSettings& settings, const VegetationSet& set, TileKey plain,
                  std::vector<SownTree>& trees) {
        trees.clear();
        const auto biomeCount = static_cast<std::uint32_t>(std::min<std::size_t>(set.biomes.size(), VegetationSet::MaxBiomes));
        if (biomeCount == 0) {
            return;
        }
        const double radius = settings.shape[0];
        const double relief = std::max(static_cast<double>(settings.shape[1]), 1.0);
        const double planetRadius = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(terrain.GetMaxLevel());
        constexpr double step = static_cast<double>(TileQuads) / static_cast<double>(TreeGrid);
        const TreeBlocks blocks = GetTreeBlocks(terrain, set);
        // Le sol à la place (u, v) de la grille de la plaine (repère de la planète).
        const auto ground = [&](double u, double v) {
            const Math::Vector3d direction = DirectionInTile(plain, u, v);
            return direction * (planetRadius + terrain.GetHeight(direction, octaves));
        };
        for (std::uint32_t c = 0; c < TreeGrid * TreeGrid; ++c) {
            const std::uint32_t gi = c % TreeGrid;
            const std::uint32_t gj = c / TreeGrid;
            const std::uint32_t x = plain.x * TreeGrid + gi;
            const std::uint32_t y = plain.y * TreeGrid + gj;
            bool candidate = false;
            for (std::uint32_t side = 1; side <= MaxTreeBlock && !candidate; ++side) {
                std::uint32_t unused;
                candidate = (blocks.used >> side & 1u) != 0 && (side == 1 || TreeBlockKeeps(settings.seed, plain.face, x, y, side, unused));
            }
            if (!candidate) {
                continue;
            }
            std::uint32_t state = TreeHash(settings.seed ^ TreeHash(plain.face * 0x632be5abu ^ TreeHash(x) ^ TreeHash(y) * 0x85ebca6bu));
            // Décalage d'au plus 30 % de la case : deux voisins restent à 40 % de l'espacement au moins.
            double u = (gi + 0.5 + (TreeRandom(state) - 0.5) * 0.6) * step;
            double v = (gj + 0.5 + (TreeRandom(state) - 0.5) * 0.6) * step;
            const double keep = TreeRandom(state);
            const double pick = TreeRandom(state);
            const double jitter = TreeRandom(state);
            Math::Vector3d direction = DirectionInTile(plain, u, v);
            const double noise = ForestNoise(direction * radius);
            double densest = 0.0;
            for (std::uint32_t b = 0; b < biomeCount; ++b) {
                densest = std::max(densest, set.biomes[b].humidity[2] * std::max(ForestMask(noise, set.biomes[b].zones[3]), 0.03));
            }
            if (keep >= densest) {
                continue;
            }
            Terrain::Zones zones;
            double height = terrain.GetHeight(direction, octaves, &zones);
            if (height < settings.shape[3] + 2.0 || height > settings.shape[2] - 0.1 * relief) {
                continue;
            }
            Math::Vector3d local = direction * (planetRadius + height);
            double altitude = height / relief;
            Climate climate = ClimateAt(radius, relief, settings.climate, local);
            const double shift = (jitter - 0.5) * 2.0;
            altitude += shift * 0.02;
            climate.temperature += shift * 2.0;
            climate.humidity += shift * 0.05;
            const Math::Vector3d zoneShares{(1.0 - zones.hills) * (1.0 - zones.mountains), zones.hills * (1.0 - zones.mountains),
                                            zones.mountains};
            std::uint32_t biome = VegetationSet::MaxBiomes;
            double remaining = 1.0;
            double total = 0.0;
            for (std::uint32_t b = 0; b < biomeCount; ++b) {
                const BiomeGpu& candidate = set.biomes[b];
                const double weight = BiomeFit(candidate, zoneShares, altitude, climate) * remaining;
                remaining -= weight;
                total += weight * candidate.humidity[2] * std::max(ForestMask(noise, candidate.zones[3]), 0.03);
                if (biome == VegetationSet::MaxBiomes && keep < total) {
                    biome = b;
                }
            }
            if (biome == VegetationSet::MaxBiomes) {
                continue;
            }
            const std::array<float, 4>& shares = set.biomeShares[biome];
            const std::uint32_t choice = pick < shares[0] ? 0 : pick < shares[1] ? 1 : pick < shares[2] ? 2 : 3;
            const double low = choice == 0 ? 0.0 : shares[choice - 1];
            const std::uint32_t entry = set.biomeSpecies[biome][choice];
            const std::uint32_t variants = std::max(entry >> 8, 1u);
            const auto variant = std::min(static_cast<std::uint32_t>((pick - low) / std::max(shares[choice] - low, 1.0e-6) * variants), variants - 1);
            const std::uint32_t species = std::min((entry & 255u) + variant, VegetationSet::MaxSpecies - 1);
            if (species >= set.species.size()) {
                continue; // le GPU lui donne une taille nulle
            }
            // Espacement : seule la place gardée par le bloc de l'espèce a l'arbre, posé au hasard dans le bloc
            // (biome et espèce restent ceux de la place).
            const std::uint32_t side = blocks.sides[biome] >> (4 * species) & 15u;
            if (side > 1) {
                std::uint32_t blockState;
                if (!TreeBlockKeeps(settings.seed, plain.face, x, y, side, blockState)) {
                    continue;
                }
                const double i = static_cast<double>(static_cast<std::int32_t>(x / side * side - plain.x * TreeGrid));
                const double j = static_cast<double>(static_cast<std::int32_t>(y / side * side - plain.y * TreeGrid));
                u = std::clamp((i + side * (0.5 + (TreeRandom(blockState) - 0.5) * 0.6)) * step, 0.0, static_cast<double>(TileQuads));
                v = std::clamp((j + side * (0.5 + (TreeRandom(blockState) - 0.5) * 0.6)) * step, 0.0, static_cast<double>(TileQuads));
                direction = DirectionInTile(plain, u, v);
                height = terrain.GetHeight(direction, octaves);
                if (height < settings.shape[3] + 2.0 || height > settings.shape[2] - 0.1 * relief) {
                    continue;
                }
                local = direction * (planetRadius + height);
            }
            const VegetationSet::Species& traits = set.species[species];
            // Comme VegetationRenderer::SetPlanet : le cosinus en float.
            const double maxSlope = std::cos(traits.maxSlope * (std::numbers::pi_v<float> / 180.0f));
            constexpr double e = 0.25;
            const Math::Vector3d du = ground(u + e, v) - local;
            const Math::Vector3d dv = ground(u, v + e) - local;
            if (std::abs(Math::Dot(Math::Normalize(Math::Cross(du, dv)), direction)) < maxSlope) {
                continue;
            }
            const double scale = Lerp(traits.minScale, traits.maxScale, TreeRandom(state)) * set.biomes[biome].humidity[3];
            trees.push_back({local, static_cast<float>(scale), species});
        }
    }
}
