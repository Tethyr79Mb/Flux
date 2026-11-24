#include "Planetology/TreeSowing.hpp"
#include "Planetology/ReliefWetness.hpp"

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

        // Roche-mère : granit, calcaire, grès (le reste basalte), et profondeur de la terre en w.
        struct Geology
        {
            double granite, limestone, sandstone, soil;
        };

        Math::Vector3d Lithology(const Math::Vector3d& flatPoint, double mountains) {
            const Math::Vector3d p = flatPoint * (1.0 / 45000.0);
            const auto offset = [](double v) { return Math::Vector3d{v, v, v}; };
            const Math::Vector3d warp = Math::Vector3d{ClimateNoise(p * 3.0 + offset(13.0)), ClimateNoise(p * 3.0 + offset(57.0)),
                                                       ClimateNoise(p * 3.0 + offset(91.0))} - offset(0.5);
            const double a = ClimateNoise(p + warp * 0.6 + offset(101.0));
            const double b = ClimateNoise(p * 1.7 + warp * 0.8 + offset(211.0));
            const double basalt = SmoothStep(0.66, 0.72, b);
            const double granite = SmoothStep(0.55, 0.62, a + 0.3 * mountains) * (1.0 - basalt);
            const double sediment = 1.0 - basalt - granite;
            const double sandstone = SmoothStep(0.42, 0.5, b + (a - 0.5) * 0.3);
            return {granite, sediment * (1.0 - sandstone), sediment * sandstone};
        }

        double SoilDepth(double wet, const Math::Vector3d& lithology) {
            const double basalt = Saturate(1.0 - lithology.x - lithology.y - lithology.z);
            return Saturate(0.55 + 0.45 * wet - 0.2 * lithology.y - 0.1 * lithology.z + 0.1 * basalt);
        }

        double BiomeBand(double x, double low, double high, double soft) {
            return SmoothStep(low - soft, low + soft, x) * (1.0 - SmoothStep(high - soft, high + soft, x));
        }

        double BiomeFit(const BiomeGpu& biome, const Math::Vector3d& zoneShares, double altitude, const Climate& climate,
                        const Geology& geology) {
            const double zones = biome.zones[0] * zoneShares.x + biome.zones[1] * zoneShares.y + biome.zones[2] * zoneShares.z;
            const double basalt = Saturate(1.0 - geology.granite - geology.limestone - geology.sandstone);
            const double rocks = biome.rocks[0] * geology.granite + biome.rocks[1] * geology.limestone
                               + biome.rocks[2] * geology.sandstone + biome.rocks[3] * basalt;
            return Saturate(zones) * BiomeBand(altitude, biome.ranges[0], biome.ranges[1], 0.02)
                 * BiomeBand(climate.temperature, biome.ranges[2], biome.ranges[3], 2.0)
                 * BiomeBand(climate.humidity, biome.humidity[0], biome.humidity[1], 0.05)
                 * Saturate(rocks) * BiomeBand(geology.soil, biome.soil[0], biome.soil[1], 0.05);
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

        // Valeur d'un bosquet, de 0 à 1 et uniforme : la même dans toute une cellule de size m, aux bords ondulés.
        // Comme GroveValue (shaders/PlanetTile.slang).
        double GroveValue(const Math::Vector3d& flatPoint, double size, std::int32_t salt) {
            const Math::Vector3d p = flatPoint * (1.0 / size);
            const auto warp = [&](double offset) {
                return ClimateNoise(p * 1.5 + Math::Vector3d{offset, offset, offset}) - 0.5;
            };
            return ClimateHash(static_cast<std::int32_t>(std::floor(p.x + warp(3.0))) + salt,
                               static_cast<std::int32_t>(std::floor(p.y + warp(41.0))),
                               static_cast<std::int32_t>(std::floor(p.z + warp(79.0))));
        }

        // Comme GroveBiome, GroveSpecies et GroveSide (shaders/PlanetTile.slang).
        std::uint32_t GroveBiome(const Math::Vector3d& flatPoint, double size, const std::array<double, VegetationSet::MaxBiomes>& weights,
                                 std::uint32_t biomeCount) {
            const double pick = GroveValue(flatPoint, size, 0);
            double total = 0.0;
            for (std::uint32_t b = 0; b < biomeCount; ++b) {
                total += weights[b];
                if (pick < total) {
                    return b;
                }
            }
            return VegetationSet::MaxBiomes;
        }

        std::uint32_t GroveSpecies(const VegetationSet& set, const Math::Vector3d& flatPoint, std::uint32_t biome) {
            const double pick = GroveValue(flatPoint, set.biomes[biome].color[3], SpeciesSalt);
            const std::array<float, 4>& shares = set.biomeShares[biome];
            return pick < shares[0] ? 0 : pick < shares[1] ? 1 : pick < shares[2] ? 2 : 3;
        }

        std::uint32_t GroveSide(const VegetationSet& set, const TreeBlocks& blocks, std::uint32_t biome, std::uint32_t choice,
                                std::uint32_t& first, std::uint32_t& variants) {
            const std::uint32_t entry = set.biomeSpecies[biome][choice];
            first = entry & 255u;
            variants = std::max(entry >> 8, 1u);
            return blocks.sides[biome] >> (4 * choice) & 15u;
        }

        // Place de l'arbre du bloc de côté side qui contient (x, y), dans la grille de la plaine (u, v de 0 à TileQuads) ;
        // state : le hasard du bloc (TreeBlockKeeps), tiré ici de la place pour side 1. Comme TreeInBlock.
        void TreeInBlock(std::uint32_t seed, TileKey plain, std::uint32_t x, std::uint32_t y, std::uint32_t side,
                         std::uint32_t& state, double& u, double& v) {
            constexpr double step = static_cast<double>(TileQuads) / static_cast<double>(TreeGrid);
            if (side == 1) {
                state = TreeHash(seed ^ TreeHash(plain.face * 0x632be5abu ^ TreeHash(x) ^ TreeHash(y) * 0x85ebca6bu));
                // Décalage d'au plus 30 % de la case : deux voisins restent à 40 % de l'espacement au moins.
                u = (x - plain.x * TreeGrid + 0.5 + (TreeRandom(state) - 0.5) * 0.6) * step;
                v = (y - plain.y * TreeGrid + 0.5 + (TreeRandom(state) - 0.5) * 0.6) * step;
                return;
            }
            const double i = static_cast<double>(static_cast<std::int32_t>(x / side * side - plain.x * TreeGrid));
            const double j = static_cast<double>(static_cast<std::int32_t>(y / side * side - plain.y * TreeGrid));
            // Dans la plaine de la place : un bloc à cheval sur deux plaines n'en sort pas son arbre.
            u = std::clamp((i + side * (0.5 + (TreeRandom(state) - 0.5) * 0.6)) * step, 0.0, static_cast<double>(TileQuads));
            v = std::clamp((j + side * (0.5 + (TreeRandom(state) - 0.5) * 0.6)) * step, 0.0, static_cast<double>(TileQuads));
        }

        // Espèce (0 à 3) du sous-bois du biome, par bosquets plus petits que ceux des arbres. Comme UnderstoryChoice.
        std::uint32_t UnderstoryChoice(const VegetationSet& set, const Math::Vector3d& flatPoint, std::uint32_t biome) {
            const double pick = GroveValue(flatPoint, set.biomes[biome].color[3] * UnderstoryGrove, UnderstorySalt);
            const std::array<float, 4> shares = biome < set.understoryShares.size() ? set.understoryShares[biome] : std::array{1.0f, 1.0f, 1.0f, 1.0f};
            return pick < shares[0] ? 0 : pick < shares[1] ? 1 : pick < shares[2] ? 2 : 3;
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
        double groves = 0.0;
        std::uint32_t wooded = 0;
        for (std::size_t b = 0; b < std::min<std::size_t>(set.biomes.size(), VegetationSet::MaxBiomes); ++b) {
            if (set.biomes[b].humidity[2] > 0.0f) {
                groves += set.biomes[b].color[3];
                ++wooded;
            }
            const double factor = b < set.biomeSpacing.size() ? set.biomeSpacing[b] : 1.0;
            for (std::uint32_t choice = 0; choice < VegetationSet::MaxSharesPerBiome; ++choice) {
                const std::uint32_t first = set.biomeSpecies[b][choice] & 255u;
                const double spacing = first < set.species.size() ? set.species[first].spacing * factor : 0.0;
                const auto side = static_cast<std::uint32_t>(std::clamp(std::round(spacing / cell), 1.0, static_cast<double>(MaxTreeBlock)));
                blocks.sides[b] |= side << (4 * choice);
                // Seuls les biomes qui ont des arbres écartent des places.
                if (set.biomes[b].humidity[2] > 0.0f) {
                    blocks.used |= 1u << side;
                }
            }
        }
        if (wooded > 0) {
            blocks.biomeGrove = static_cast<float>(groves / wooded);
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
        WetnessGrid wetness;
        BuildWetnessGrid(terrain, plain, wetness);
        // Le sol à la place (u, v) de la grille de la plaine (repère de la planète).
        const auto ground = [&](double u, double v) {
            const Math::Vector3d direction = DirectionInTile(plain, u, v);
            return direction * (planetRadius + terrain.GetHeight(direction, octaves));
        };
        // Une place : sa forêt d'abord (avant le relief, cher), puis le sol et le biome qui y pousse (comme SowSite et
        // SiteBiome, shaders/PlanetTile.slang).
        struct Site
        {
            double u = 0.0, v = 0.0;
            Math::Vector3d direction, flat, local;
            double noise = 0.0, wet = 0.0, moreForest = 0.0;
            std::array<double, VegetationSet::MaxBiomes> weights{};
        };
        const auto prepare = [&](double u, double v) {
            Site site;
            site.u = u;
            site.v = v;
            site.direction = DirectionInTile(plain, u, v);
            site.flat = site.direction * radius;
            site.noise = ForestNoise(site.flat);
            // Humidité du relief : plus de forêts au fond des vallons, moins sur les crêtes.
            site.wet = SampleWetness(wetness, u, v);
            site.moreForest = ReliefForest * site.wet;
            return site;
        };
        // Biome qui pousse sur la place (MaxBiomes : aucun, ou plage, neige).
        const auto grow = [&](Site& site) {
            Terrain::Zones zones;
            const double height = terrain.GetHeight(site.direction, octaves, &zones);
            if (height < settings.shape[3] + 2.0 || height > settings.shape[2] - 0.1 * relief) {
                return VegetationSet::MaxBiomes;
            }
            site.local = site.direction * (planetRadius + height);
            const double altitude = height / relief;
            const double wet = site.wet;
            Climate climate = ClimateAt(radius, relief, settings.climate, site.local);
            climate.humidity += ReliefHumidity * wet;
            const Math::Vector3d zoneShares{(1.0 - zones.hills) * (1.0 - zones.mountains), zones.hills * (1.0 - zones.mountains),
                                            zones.mountains};
            const Math::Vector3d lithology = Lithology(site.flat, zones.mountains);
            const Geology geology{lithology.x, lithology.y, lithology.z, SoilDepth(wet, lithology)};
            double remaining = 1.0;
            for (std::uint32_t b = 0; b < biomeCount; ++b) {
                site.weights[b] = BiomeFit(set.biomes[b], zoneShares, altitude, climate, geology) * remaining;
                remaining -= site.weights[b];
            }
            return GroveBiome(site.flat, blocks.biomeGrove, site.weights, biomeCount);
        };
        // Variante tirée parmi celles de l'espèce, pente, taille : l'arbre s'il pousse. Faux si la pente le refuse (un
        // autre côté de bloc peut encore poser le sien).
        const auto plant = [&](const Site& site, std::uint32_t firstVariant, std::uint32_t variants, double scaleFactor,
                               std::uint32_t& state, bool understory) {
            const auto variant = std::min(static_cast<std::uint32_t>(TreeRandom(state) * variants), variants - 1);
            const std::uint32_t species = std::min(firstVariant + variant, VegetationSet::MaxSpecies - 1);
            if (species >= set.species.size()) {
                return true; // le GPU lui donne une taille nulle
            }
            const VegetationSet::Species& traits = set.species[species];
            // Comme VegetationRenderer::SetPlanet : le cosinus en float.
            const double maxSlope = std::cos(traits.maxSlope * (std::numbers::pi_v<float> / 180.0f));
            constexpr double e = 0.25;
            const Math::Vector3d du = ground(site.u + e, site.v) - site.local;
            const Math::Vector3d dv = ground(site.u, site.v + e) - site.local;
            // Comme SowPlace : <= et borné à 1 (une pente maximale nulle n'en laisse aucun).
            const Math::Vector3d normal = Math::Normalize(Math::Cross(du, dv));
            const double cosine = Math::Dot(normal, site.direction);
            if (std::min(std::abs(cosine), 1.0) <= maxSlope) {
                return false;
            }
            // Une plante du sous-bois garde sa taille : la réduction du sous-bois fait des jeunes arbres.
            const double factor = understory && traits.plant ? 1.0 : scaleFactor;
            const double scale = Lerp(traits.minScale, traits.maxScale, TreeRandom(state)) * factor;
            const Math::Vector3d below = cosine < 0.0 ? -normal : normal;
            trees.push_back({site.local, static_cast<float>(scale), species, understory,
                             Math::Vector3f(static_cast<float>(below.x), static_cast<float>(below.y), static_cast<float>(below.z))});
            return true;
        };
        for (std::uint32_t c = 0; c < TreeGrid * TreeGrid; ++c) {
            const std::uint32_t gi = c % TreeGrid;
            const std::uint32_t gj = c / TreeGrid;
            const std::uint32_t x = plain.x * TreeGrid + gi;
            const std::uint32_t y = plain.y * TreeGrid + gj;
            for (std::uint32_t side = 1; side <= MaxTreeBlock; ++side) {
                if ((blocks.used >> side & 1u) == 0) {
                    continue;
                }
                std::uint32_t state;
                if (side != 1 && !TreeBlockKeeps(settings.seed, plain.face, x, y, side, state)) {
                    continue;
                }
                double u, v;
                TreeInBlock(settings.seed, plain, x, y, side, state, u, v);
                const double keep = TreeRandom(state);
                Site site = prepare(u, v);
                double densest = 0.0;
                for (std::uint32_t b = 0; b < biomeCount; ++b) {
                    densest = std::max(densest, set.biomes[b].humidity[2] * std::max(ForestMask(site.noise, set.biomes[b].zones[3] + site.moreForest), 0.03));
                }
                if (keep >= densest) {
                    continue;
                }
                const std::uint32_t biome = grow(site);
                if (biome == VegetationSet::MaxBiomes
                    || keep >= set.biomes[biome].humidity[2] * std::max(ForestMask(site.noise, set.biomes[biome].zones[3] + site.moreForest), 0.03)) {
                    continue;
                }
                std::uint32_t firstVariant, variants;
                if (GroveSide(set, blocks, biome, GroveSpecies(set, site.flat, biome), firstVariant, variants) != side) {
                    continue;
                }
                // Bord d'un bosquet d'une autre espèce : écarté à moins d'un demi-espacement.
                bool alone = true;
                const double reach = 0.5 * side * step;
                for (std::uint32_t k = 0; k < 4 && alone; ++k) {
                    const double du = k == 0 ? reach : k == 1 ? -reach : 0.0;
                    const double dv = k == 2 ? reach : k == 3 ? -reach : 0.0;
                    const Math::Vector3d beside = DirectionInTile(plain, u + du, v + dv) * radius;
                    const std::uint32_t other = GroveBiome(beside, blocks.biomeGrove, site.weights, biomeCount);
                    if (other != VegetationSet::MaxBiomes && set.biomes[other].humidity[2] > 0.0f) {
                        std::uint32_t otherFirst, otherVariants;
                        const std::uint32_t otherSide = GroveSide(set, blocks, other, GroveSpecies(set, beside, other), otherFirst, otherVariants);
                        alone = otherSide == side && otherFirst == firstVariant;
                    }
                }
                if (!alone) {
                    continue;
                }
                if (plant(site, firstVariant, variants, set.biomes[biome].humidity[3], state, false)) {
                    break;
                }
            }
        }
        // Sous-bois : une place par case de sa propre grille, sans les blocs d'espacement des arbres ; écartée près du
        // pied d'un arbre possible.
        double densestUnderstory = 0.0;
        for (std::uint32_t b = 0; b < biomeCount; ++b) {
            densestUnderstory = std::max(densestUnderstory, static_cast<double>(set.biomes[b].soil[2]));
        }
        if (densestUnderstory <= 0.0) {
            return;
        }
        for (std::uint32_t c = 0; c < TreeGrid * TreeGrid; ++c) {
            const std::uint32_t gi = c % TreeGrid;
            const std::uint32_t gj = c / TreeGrid;
            const std::uint32_t x = plain.x * TreeGrid + gi;
            const std::uint32_t y = plain.y * TreeGrid + gj;
            std::uint32_t state = TreeHash(settings.seed ^ TreeHash(plain.face * 0x632be5abu ^ TreeHash(x) ^ TreeHash(y) * 0x85ebca6bu)
                                           ^ UnderstorySeed);
            const double u = (gi + 0.5 + (TreeRandom(state) - 0.5) * UnderstoryJitter) * step;
            const double v = (gj + 0.5 + (TreeRandom(state) - 0.5) * UnderstoryJitter) * step;
            const double keep = TreeRandom(state);
            Site site = prepare(u, v);
            double densest = 0.0;
            for (std::uint32_t b = 0; b < biomeCount; ++b) {
                densest = std::max(densest, set.biomes[b].soil[2] * std::max(ForestMask(site.noise, set.biomes[b].zones[3] + site.moreForest), UnderstoryOpen));
            }
            if (keep >= densest) {
                continue;
            }
            // Pied d'un arbre : celui que chaque bloc employé pourrait poser, qu'il ait poussé ou non.
            bool clear = true;
            for (std::uint32_t side = 1; side <= MaxTreeBlock && clear; ++side) {
                if ((blocks.used >> side & 1u) == 0) {
                    continue;
                }
                std::uint32_t treeState = 0;
                if (side != 1) {
                    TreeBlockKeeps(settings.seed, plain.face, x, y, side, treeState);
                }
                double tu, tv;
                TreeInBlock(settings.seed, plain, x, y, side, treeState, tu, tv);
                clear = (tu - u) * (tu - u) + (tv - v) * (tv - v) >= UnderstoryClearance * UnderstoryClearance * step * step;
            }
            if (!clear) {
                continue;
            }
            const std::uint32_t biome = grow(site);
            if (biome == VegetationSet::MaxBiomes
                || keep >= set.biomes[biome].soil[2] * std::max(ForestMask(site.noise, set.biomes[biome].zones[3] + site.moreForest), UnderstoryOpen)) {
                continue;
            }
            if (biome >= set.understorySpecies.size()) {
                continue;
            }
            const std::uint32_t entry = set.understorySpecies[biome][UnderstoryChoice(set, site.flat, biome)];
            plant(site, entry & 255u, std::max(entry >> 8, 1u), set.biomes[biome].soil[3], state, true);
        }
    }
}
