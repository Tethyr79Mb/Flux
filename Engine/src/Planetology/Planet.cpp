#include "Flux/Planetology/Planet.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <numbers>
#include <random>

namespace Flux::Planetology {
    namespace {
        // Repère de chaque face : sa normale et ses axes s et t, avec s x t = normale (sommets dans le sens
        // trigonométrique vu de dehors, comme le cube du moteur).
        struct Face
        {
            Math::Vector3d normal;
            Math::Vector3d s;
            Math::Vector3d t;
        };
        constexpr std::array<Face, 6> Faces{{
            {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
            {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
            {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
            {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
            {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
        }};

        constexpr double QuarterPi = 0.25 * std::numbers::pi;

        // Fréquence du bruit sur la sphère unité : 1,5 donne quelques continents.
        constexpr double BaseFrequency = 1.5;
        // Octaves au plus (la plus fine fait quelques mètres sur une planète comme la Terre).
        constexpr std::uint32_t MaxOctaves = 22;
        // Relief de détail (bosses, rochers) à partir de l'octave 6 (environ 4 km de longueur d'onde sur une planète
        // de 60 km), x 0,65 par octave (contre 0,5 pour les grandes formes) : le sol reste accidenté de près. Son
        // amplitude (fraction du relief) dépend de l'endroit : presque rien en plaine, plus en collines et en montagne.
        constexpr std::uint32_t DetailFirstOctave = 6;
        constexpr double DetailGain = 0.58;
        constexpr double DetailPlains = 0.012;
        constexpr double DetailHills = 0.03;
        constexpr double DetailMountains = 0.065;
        // Largeur des chaînes de montagnes : là où 1 - |bruit de zone| dépasse ChainStart, pleines au-delà de ChainEnd.
        constexpr double ChainStart = 0.75;
        constexpr double ChainEnd = 0.95;
        // Part de la hauteur d'une chaîne donnée par sa bande elle-même (un dos continu le long de la chaîne), le reste
        // par les sommets : sans elle, les montagnes seraient des pics isolés dans la plaine.
        constexpr double ChainBase = 0.2;
        constexpr double PeakHeight = 0.85; // les sommets, de 0 à 1, au-dessus de la bande
        // Sommets (Terrain::Peaks) : au plus un par maille d'un réseau 3D (maille d'environ largeur des montagnes / 2π),
        // présent avec la chance PeakChance (sur 256), décalé au hasard dans sa maille. Seules comptent les mailles dont
        // le sommet tombe près de la sphère (à moins de PeakSlabInner maille, effacé à PeakSlabOuter) : une couche
        // d'une maille d'épaisseur, donc des sommets aussi serrés partout.
        constexpr std::uint32_t PeakChance = 218;
        // Sommets secondaires (réseau deux fois plus fin) : leur hauteur, en part de celle des grands.
        constexpr double PeakSecondary = 0.45;
        constexpr double PeakSlabInner = 0.35;
        constexpr double PeakSlabOuter = 0.5;
        // Hauteur d'un sommet : de PeakLowest à 1 (plus souvent bas que haut). Rayon de son pied (en mailles) :
        // PeakRadius pour le plus haut, 60 % pour le plus bas, ± 20 % au hasard. Il reste sous une maille avec ses
        // versants allongés (voir PeakSpurs) : les 3 x 3 x 3 mailles autour du point suffisent.
        constexpr double PeakLowest = 0.35;
        constexpr double PeakRadius = 0.75;
        // Profil d'un versant, t de 0 (sommet) à 1 (pied) : (1 - t)^PeakShape x (1 + PeakTop x PeakShape x t), concave en
        // bas comme une vraie montagne, au sommet arrondi (PeakTop 1 : plat ; 0 : pointu).
        constexpr double PeakShape = 1.6;
        constexpr double PeakTop = 0.5;
        constexpr double PeakTip = 0.05;
        // Le pied s'éloigne ou se rapproche selon la direction autour du sommet : un côté plus doux que l'autre
        // (PeakAsymmetry), une montagne allongée (PeakElongation), et PeakSpurCount contreforts (arêtes qui descendent
        // du sommet, d'autant plus fines que PeakSpurSharpness est haut).
        constexpr double PeakAsymmetry = 0.2;
        constexpr double PeakElongation = 0.1;
        constexpr double PeakSpurs = 0.25;
        constexpr double PeakSpurSharpness = 6.0;
        constexpr std::uint32_t PeakSpurCount = 4;
        constexpr double PeakStretchSoftness = 0.2; // largeur du minimum doux qui borne l'allongement
        // Union douce des sommets voisins (k x log(somme des exp(h / k))) : un col arrondi entre eux, pas une arête vive.
        constexpr double PeakBlend = 0.04;
        // Ravines (Terrain::Gullies) : amplitude de la première octave (fraction du relief) ; pentes (m/m) où elles
        // commencent et où elles sont pleines. Rien sur le plat : les plaines restent lisses pour se poser, et les
        // sommets et cols, où la pente n'a pas de direction nette, ne tournent pas en tourbillons.
        constexpr double GullyAmplitude = 0.09;
        constexpr double GullyStartSlope = 0.08;
        constexpr double GullyFullSlope = 0.35;
        constexpr double GullyGain = 0.5; // amplitude d'une octave à la suivante
        // Les ravines déjà posées comptent ce nombre de fois dans la pente que suivent les suivantes : plus haut, elles
        // bifurquent davantage (des branches plutôt que des sillons parallèles).
        constexpr double GullyBranch = 1.5;
        // Décalage des points des cellules (fraction de cellule) et netteté de leur poids (exp(-netteté x d²)) : avec
        // 0,25 et 4, les cellules hors du voisinage 3 x 3 x 3 pèsent moins de 0,2 % (pas de marche visible).
        constexpr double GullyJitter = 0.5;
        constexpr double GullySharpness = 4.0;
        // Part du relief des montagnes et des collines dans leur zone.
        constexpr double MountainWeight = 0.85;
        constexpr double HillWeight = 0.15;

        // Érosion : chaque octave est atténuée là où la couche est déjà en pente (1 / (1 + force x pente²), la pente
        // en mètres par mètre). Les versants restent lisses et creusés de ravines, le détail se garde pour les sommets
        // et les replats, comme un relief usé par l'eau (méthode d'Inigo Quilez). Seule une octave plus grande ou la
        // même compte : une tuile sans les octaves fines garde le même terrain que ses enfants, au détail près.
        constexpr double ErosionHills = 4.0;
        constexpr double ErosionDetail = 3.0;
        // Déformation du domaine : le point où se lit une couche est poussé (en radians, sur la sphère) le long de la
        // pente d'un bruit lent. Côtes découpées, chaînes de montagnes allongées et tordues au lieu de taches rondes.
        // La grande déformation touche les continents et les zones, la petite s'y ajoute pour les montagnes et les
        // collines ; le détail fin n'est pas déformé.
        constexpr double WarpLarge = 0.06;
        constexpr double WarpSmall = 0.012;

        // Couches du relief (voir GetHeight), dans l'ordre de TileGpuInputs::cells et de shaders/PlanetTile.slang.
        struct Layer
        {
            std::uint32_t first; // octaves [first, last)
            std::uint32_t last;
            double gain;
            Math::Vector3d offset; // sépare les couches (sinon elles suivraient les mêmes formes)
            // Suit la largeur des montagnes (Terrain::m_scale) plutôt que le rayon : montagnes et collines gardent
            // leur taille en mètres sur une grande planète. Le détail reste à l'échelle de la planète : plus fin de
            // m_scale à la même hauteur, il rendait toutes les plaines trop raides pour se poser. Les zones, les
            // continents et les déformations aussi : des zones à la largeur des montagnes faisaient de minces bandes
            // sinueuses, des « racines » vues d'orbite. Une zone large et aux bords doux (ChainStart, ChainEnd, et
            // ChainBase bas pour ne pas faire de plateau) porte plusieurs sommets, comme un vrai massif.
            bool scaled;
            // Perd ses octaves Terrain::m_shift niveaux plus tôt (couches à l'échelle des montagnes).
            bool shifted;
            double erosion;        // force de l'érosion, 0 : aucune
            // Octaves dont la pente compte pour l'érosion (les suivantes sont atténuées selon la pente déjà connue) : la
            // pente coûte autant que le bruit, et les octaves fines du détail sont les plus nombreuses.
            std::uint32_t erodedOctaves;
        };
        enum LayerIndex { Continents, MountainZone, HillZone, PeakField, Hills, Detail, WarpLargeField, WarpSmallField, GullyField };
        constexpr std::array<Layer, 9> Layers{{
            {0, 5, 0.5, {0.0, 0.0, 0.0}, false, false, 0.0, 0},
            {1, 3, 0.5, {31.7, -12.3, 7.1}, false, false, 0.0, 0},
            {2, 5, 0.5, {-8.9, 44.2, -19.6}, false, false, 0.0, 0},
            // Sommets : deux réseaux, les grands sommets puis les sommets secondaires (voir Terrain::Peaks).
            {2, 4, 0.5, {17.3, -41.7, 8.9}, true, true, 0.0, 0},
            {4, 9, 0.5, {5.3, 23.9, -33.1}, true, true, ErosionHills, 5},
            {DetailFirstOctave, MaxOctaves, DetailGain, {-23.1, 5.7, 31.3}, false, false, ErosionDetail, 6},
            // Déformations : toutes leurs octaves, quel que soit le niveau de la tuile (sinon le terrain glisserait
            // d'un niveau à l'autre).
            {0, 3, 0.5, {-37.9, 13.3, 21.7}, false, false, 0.0, 0},
            {3, 6, 0.5, {11.1, -29.3, -45.7}, false, false, 0.0, 0},
            // Ravines : cellules de 1/12 de la largeur des montagnes (une maille du réseau = longueur d'onde / 2π) et plus
            // fines (voir Terrain::Gullies).
            {3, 7, 0.5, {-2.9, 37.1, 15.3}, true, true, 0.0, 0},
        }};
        static_assert([] {
            std::uint32_t count = 0;
            for (const Layer& layer : Layers) {
                count += layer.last - layer.first;
            }
            return count;
        }() == TileGpuInputs::Octaves);

        // Partie d'un vecteur tangente à la sphère en direction (unitaire).
        Math::Vector3d Tangent(const Math::Vector3d& v, const Math::Vector3d& direction) {
            return v - direction * Math::Dot(v, direction);
        }

        double SmoothStep(double edge0, double edge1, double x) {
            const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

        double Fade(double t) {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        // Dérivée de Fade.
        double FadeSlope(double t) {
            return 30.0 * t * t * (t * (t - 2.0) + 1.0);
        }

        // Vecteur du gradient de Perlin choisi par hash : Gradient(hash, x, y, z) = Dot(GradientVector(hash), (x, y, z)).
        Math::Vector3d GradientVector(std::uint8_t hash) {
            const std::uint8_t h = hash & 15u;
            const double su = (h & 1u) ? -1.0 : 1.0;
            const double sv = (h & 2u) ? -1.0 : 1.0;
            Math::Vector3d g{};
            (h < 8 ? g.x : g.y) += su;
            (h < 4 ? g.y : (h == 12 || h == 14 ? g.x : g.z)) += sv;
            return g;
        }

        // Coordonnée de la grille de toute la face (de 0 à TileQuads x 2^niveau) en s ou t de -1 à 1 : calculée depuis
        // un entier, donc identique pour un sommet partagé par deux tuiles voisines.
        double GridToFace(std::int64_t index, std::uint32_t level) {
            const double cells = static_cast<double>(TileQuads) * static_cast<double>(std::uint64_t{1} << level);
            return -1.0 + 2.0 * static_cast<double>(index) / cells;
        }

        // Profondeur des jupes d'une tuile (bord de longueur `edge`) : assez pour couvrir l'écart avec une voisine
        // plus grossière (le relief qu'elle n'a pas).
        double SkirtDepth(const Terrain& terrain, std::uint32_t octaves, double edge) {
            return 0.05 * edge + 2.0 * terrain.GetOmittedRelief(octaves) + 1.0;
        }
    }

    Math::Vector3d DirectionFromFace(std::uint32_t face, double s, double t) {
        const Face& f = Faces[face % 6];
        const double a = std::tan(s * QuarterPi);
        const double b = std::tan(t * QuarterPi);
        return Math::Normalize(f.normal + f.s * a + f.t * b);
    }

    void FaceFromDirection(const Math::Vector3d& direction, std::uint32_t& face, double& s, double& t) {
        const double ax = std::abs(direction.x);
        const double ay = std::abs(direction.y);
        const double az = std::abs(direction.z);
        if (ax >= ay && ax >= az) {
            face = direction.x >= 0.0 ? 0 : 1;
        } else if (ay >= az) {
            face = direction.y >= 0.0 ? 2 : 3;
        } else {
            face = direction.z >= 0.0 ? 4 : 5;
        }
        const Face& f = Faces[face];
        const double along = Math::Dot(direction, f.normal);
        if (along <= 0.0) {
            s = t = 0.0; // direction nulle
            return;
        }
        const Math::Vector3d onCube = direction / along;
        s = std::atan(Math::Dot(onCube, f.s)) / QuarterPi;
        t = std::atan(Math::Dot(onCube, f.t)) / QuarterPi;
    }

    Terrain::Terrain(double radius, double relief, std::int32_t seed, double mountainWidth)
        : m_radius(std::max(radius, 1.0)), m_relief(std::max(relief, 0.0)) {
        // Largeur des montagnes : la longueur d'onde du réseau des sommets (2π mailles), qui fait sinon environ un rayon.
        // Seulement plus fines : sur une petite planète, elles gardent la taille que permet son rayon.
        const double natural = m_radius * 2.0 * std::numbers::pi / (BaseFrequency * std::ldexp(1.0, static_cast<int>(Layers[PeakField].first)));
        m_scale = mountainWidth > 0.0 ? std::clamp(natural / mountainWidth, 1.0, 65536.0) : 1.0;
        m_shift = static_cast<std::uint32_t>(std::ceil(std::log2(m_scale) - 1e-9));
        // Environ 1,5 m entre deux sommets au niveau le plus fin.
        const double faceEdge = 0.5 * std::numbers::pi * m_radius;
        const double levels = std::ceil(std::log2(faceEdge / (1.5 * static_cast<double>(TileQuads))));
        m_maxLevel = static_cast<std::uint32_t>(std::clamp(levels, 0.0, static_cast<double>(MaxLevel)));

        // Permutation tirée de la graine, mélangée à la main : std::shuffle ne donne pas le même ordre selon le
        // compilateur, std::mt19937 si.
        std::array<std::uint8_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            values[i] = static_cast<std::uint8_t>(i);
        }
        std::mt19937 random(static_cast<std::uint32_t>(seed));
        for (std::uint32_t i = 255; i > 0; --i) {
            std::swap(values[i], values[random() % (i + 1)]);
        }
        for (std::uint32_t i = 0; i < 512; ++i) {
            m_permutation[i] = values[i & 255u];
        }
    }

    std::uint32_t Terrain::GetOctaves(std::uint32_t level) const {
        // L'octave k a une longueur d'onde de 2π/(BaseFrequency.2^k) rayons ; les sommets d'un niveau sont espacés
        // de (π/2)/(TileQuads.2^niveau) rayons. On garde les octaves au moins deux fois plus grandes que cet écart.
        const double spacing = 0.5 * std::numbers::pi / (static_cast<double>(TileQuads) * std::ldexp(1.0, static_cast<int>(level)));
        const double wavelength = 2.0 * std::numbers::pi / BaseFrequency;
        const double octaves = std::floor(std::log2(wavelength / (2.0 * spacing))) + 1.0;
        // Une octave d'une couche à la largeur des montagnes compte comme m_shift octaves plus loin (voir Fbm).
        return static_cast<std::uint32_t>(std::clamp(octaves, 1.0, static_cast<double>(MaxOctaves + m_shift)));
    }

    Terrain::Sample Terrain::Noise(const Math::Vector3d& p, bool withGradient) const {
        // Bruit de Perlin et sa pente exacte (dérivée de la courbe d'atténuation Fade comprise) : le mélange trilinéaire
        // des valeurs aux 8 coins, écrit en polynôme de u, v, w pour se dériver.
        const double fx = std::floor(p.x);
        const double fy = std::floor(p.y);
        const double fz = std::floor(p.z);
        const auto X = static_cast<std::uint32_t>(static_cast<std::int64_t>(fx) & 255);
        const auto Y = static_cast<std::uint32_t>(static_cast<std::int64_t>(fy) & 255);
        const auto Z = static_cast<std::uint32_t>(static_cast<std::int64_t>(fz) & 255);
        const Math::Vector3d f{p.x - fx, p.y - fy, p.z - fz};
        const Math::Vector3d u{Fade(f.x), Fade(f.y), Fade(f.z)};
        const Math::Vector3d du{FadeSlope(f.x), FadeSlope(f.y), FadeSlope(f.z)};
        const auto& P = m_permutation;
        const std::uint32_t A = P[X] + Y;
        const std::uint32_t AA = P[A] + Z;
        const std::uint32_t AB = P[A + 1] + Z;
        const std::uint32_t B = P[X + 1] + Y;
        const std::uint32_t BA = P[B] + Z;
        const std::uint32_t BB = P[B + 1] + Z;
        // Coins a (0,0,0), b (1,0,0), c (0,1,0), d (1,1,0), e (0,0,1), f (1,0,1), g (0,1,1), h (1,1,1).
        const Math::Vector3d ga = GradientVector(P[AA]), gb = GradientVector(P[BA]), gc = GradientVector(P[AB]),
                             gd = GradientVector(P[BB]), ge = GradientVector(P[AA + 1]), gf = GradientVector(P[BA + 1]),
                             gg = GradientVector(P[AB + 1]), gh = GradientVector(P[BB + 1]);
        const double va = Math::Dot(ga, f), vb = Math::Dot(gb, f - Math::Vector3d{1, 0, 0}),
                     vc = Math::Dot(gc, f - Math::Vector3d{0, 1, 0}), vd = Math::Dot(gd, f - Math::Vector3d{1, 1, 0}),
                     ve = Math::Dot(ge, f - Math::Vector3d{0, 0, 1}), vf = Math::Dot(gf, f - Math::Vector3d{1, 0, 1}),
                     vg = Math::Dot(gg, f - Math::Vector3d{0, 1, 1}), vh = Math::Dot(gh, f - Math::Vector3d{1, 1, 1});
        const double k1 = vb - va, k2 = vc - va, k3 = ve - va;
        const double k4 = va - vb - vc + vd, k5 = va - vc - ve + vg, k6 = va - vb - ve + vf;
        const double k7 = -va + vb + vc - vd + ve - vf - vg + vh;
        Sample sample;
        sample.value = va + u.x * k1 + u.y * k2 + u.z * k3 + u.x * u.y * k4 + u.y * u.z * k5 + u.z * u.x * k6
                     + u.x * u.y * u.z * k7;
        if (!withGradient) {
            sample.gradient = {};
            return sample;
        }
        sample.gradient = ga + (gb - ga) * u.x + (gc - ga) * u.y + (ge - ga) * u.z + (ga - gb - gc + gd) * (u.x * u.y)
                        + (ga - gc - ge + gg) * (u.y * u.z) + (ga - gb - ge + gf) * (u.z * u.x)
                        + (-ga + gb + gc - gd + ge - gf - gg + gh) * (u.x * u.y * u.z);
        sample.gradient += Math::Vector3d{du.x * (k1 + u.y * k4 + u.z * k6 + u.y * u.z * k7),
                                          du.y * (k2 + u.z * k5 + u.x * k4 + u.z * u.x * k7),
                                          du.z * (k3 + u.x * k6 + u.y * k5 + u.x * u.y * k7)};
        return sample;
    }

    // Somme des octaves [first, min(last, octaves)) d'une couche lue au point `point` (la direction, déformée ou non),
    // octave k à la fréquence BaseFrequency x 2^k, chacune `gain` fois plus basse que la précédente, divisée par la
    // somme des amplitudes de toutes ses octaves [first, last) : de -1 à 1 environ (le plus souvent entre -0,5 et
    // 0,5), à la même échelle quel que soit le nombre d'octaves d'une tuile. slopeScale : mètres par unité de la
    // couche, divisés par le rayon (pente réelle de l'érosion).
    double Terrain::Fbm(const Math::Vector3d& point, const Math::Vector3d& direction, std::uint32_t layerIndex,
                        std::uint32_t octaves, double slopeScale) const {
        const Layer& layer = Layers[layerIndex];
        const double gain = layer.gain;
        const double total = (1.0 - std::pow(gain, static_cast<double>(layer.last - layer.first))) / (1.0 - gain);
        // Une couche plus fine de m_scale perd ses octaves m_shift niveaux plus tôt (m_shift arrondi au-dessus : le
        // GPU fait le même choix sans calcul en virgule).
        const std::uint32_t shift = layer.shifted ? m_shift : 0u;
        const std::uint32_t last = std::min(layer.last, octaves > shift ? octaves - shift : 0u);
        double sum = 0.0;
        double amplitude = 1.0 / total;
        double frequency = BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first)) * (layer.scaled ? m_scale : 1.0);
        Math::Vector3d p = point * frequency + layer.offset;
        Math::Vector3d slope{}; // pente de la couche (par radian), octaves déjà sommées et celle-ci
        for (std::uint32_t k = layer.first; k < last; ++k) {
            const bool eroded = k - layer.first < layer.erodedOctaves;
            const Sample n = Noise(p, eroded);
            const double value = n.value;
            if (eroded) {
                slope += Tangent(n.gradient, direction) * (frequency * amplitude);
            }
            const double steep = slopeScale * Math::Length(slope);
            sum += amplitude * value / (1.0 + layer.erosion * steep * steep);
            amplitude *= gain;
            frequency *= 2.0;
            p *= 2.0;
        }
        return sum;
    }

    double Terrain::Peaks(const Math::Vector3d& point, const Math::Vector3d& direction, std::uint32_t octaves,
                          Math::Vector3d& slope) const {
        slope = {};
        const Layer& layer = Layers[PeakField];
        // Une tuile trop grossière perd un réseau dont l'écart entre sommets ne tient plus entre ses sommets à elle.
        const std::uint32_t last = std::min(layer.last, octaves > m_shift ? octaves - m_shift : 0u);
        const auto& P = m_permutation;
        double sum = 0.0;     // somme des exp(h / PeakBlend) - 1 : 0 loin de tout sommet
        Math::Vector3d gradient{}; // somme des exp(h / PeakBlend) x pente de h, par radian
        double frequency = BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first)) * m_scale;
        double scale = 1.0; // hauteur des sommets du réseau
        Math::Vector3d p = point * frequency + layer.offset;
        for (std::uint32_t k = layer.first; k < last; ++k, frequency *= 2.0, p *= 2.0, scale *= PeakSecondary) {
            const Math::Vector3d base{std::floor(p.x), std::floor(p.y), std::floor(p.z)};
            const Math::Vector3d f = p - base;
            const auto cx = static_cast<std::int64_t>(base.x), cy = static_cast<std::int64_t>(base.y), cz = static_cast<std::int64_t>(base.z);
            for (int i = -1; i <= 1; ++i) {
                for (int j = -1; j <= 1; ++j) {
                    for (int l = -1; l <= 1; ++l) {
                        const std::uint32_t h = P[P[P[(cx + i) & 255] + ((cy + j) & 255)] + ((cz + l) & 255)];
                        if (P[h + 3] >= PeakChance) {
                            continue;
                        }
                        const auto random = [&](std::uint32_t index) { return P[h + index] / 255.0; };
                        const auto vector = [&](std::uint32_t index) {
                            return Math::Vector3d{random(index), random(index + 1), random(index + 2)} - Math::Vector3d{0.5, 0.5, 0.5};
                        };
                        // Du sommet au point : sa part le long de la verticale décide si le sommet est dans la couche,
                        // le reste (tangent) donne la distance et la direction autour du sommet.
                        const Math::Vector3d e = f - Math::Vector3d{double(i), double(j), double(l)} - vector(0) * 0.7 - Math::Vector3d{0.5, 0.5, 0.5};
                        const double radial = Math::Dot(e, direction);
                        const double slab = SmoothStep(PeakSlabOuter, PeakSlabInner, std::abs(radial));
                        const Math::Vector3d flat = e - direction * radial;
                        const double r2 = Math::Dot(flat, flat);
                        if (slab <= 0.0 || r2 >= 1.0) {
                            continue;
                        }
                        const double raw = random(4);
                        const double height = (PeakLowest + (1.0 - PeakLowest) * raw) * slab * scale;
                        const double radius = PeakRadius * (0.6 + 0.4 * raw) * (0.8 + 0.4 * random(5));
                        const double r = std::sqrt(r2);
                        Math::Vector3d slopeOf{}; // pente de t (par maille)
                        double t = 0.0;
                        if (r > 1e-9) {
                            const Math::Vector3d u = flat / r;
                            // Allongement du pied selon la direction u, et sa dérivée par rapport à u (stretchSlope).
                            const Math::Vector3d a1 = vector(6);
                            const Math::Vector3d a2 = vector(9);
                            const double along2 = Math::Dot(u, a2);
                            double stretch = 1.0 + 2.0 * PeakAsymmetry * Math::Dot(u, a1) + PeakElongation * (4.0 * along2 * along2 - 0.5)
                                           - 0.4 * PeakSpurs;
                            Math::Vector3d stretchSlope = a1 * (2.0 * PeakAsymmetry) + a2 * (8.0 * PeakElongation * along2);
                            for (std::uint32_t n = 0; n < PeakSpurCount; ++n) {
                                const Math::Vector3d spur = vector(12 + 3 * n);
                                const double length = Math::Length(spur);
                                const double c = length > 0.0 ? Math::Dot(u, spur) / length : 0.0;
                                if (c > 0.0) {
                                    const double lobe = std::pow(c, PeakSpurSharpness - 1.0);
                                    stretch += PeakSpurs * lobe * c;
                                    stretchSlope += spur * (PeakSpurs * PeakSpurSharpness * lobe / length);
                                }
                            }
                            // Le pied reste à moins d'une maille (voir PeakRadius) : minimum doux avec 1 / radius, sans
                            // cassure de la pente (les ravines la suivent, elles sauteraient).
                            const double gap = stretch - 1.0 / radius;
                            const double root = std::sqrt(gap * gap + PeakStretchSoftness * PeakStretchSoftness);
                            const double clamped = 0.5 * (stretch + 1.0 / radius - root);
                            t = r / (radius * clamped);
                            if (t >= 1.0) {
                                continue;
                            }
                            // Pente de l'allongement : sa dérivée par rapport à u, ramenée au plan tangent, divisée par r.
                            const Math::Vector3d g = stretchSlope - u * Math::Dot(u, stretchSlope) - direction * Math::Dot(direction, stretchSlope);
                            slopeOf = u / (radius * clamped) - g * (0.5 * (1.0 - gap / root) * t / (clamped * r));
                        }
                        // Pointe arrondie sur PeakTip (en part du rayon) : sans elle, la pente changerait d'un coup de sens au
                        // sommet, et les ravines qui la suivent y feraient une marche.
                        const double tip = std::sqrt(t * t + PeakTip * PeakTip);
                        const double norm = std::sqrt(1.0 + PeakTip * PeakTip) - PeakTip;
                        const double rounded = (tip - PeakTip) / norm;
                        slopeOf *= t / (tip * norm);
                        const double rest = 1.0 - rounded;
                        const double lower = std::pow(rest, PeakShape - 1.0);
                        const double shape = lower * rest * (1.0 + PeakTop * PeakShape * rounded);
                        const double shapeSlope = PeakShape * lower * (rest * PeakTop - (1.0 + PeakTop * PeakShape * rounded));
                        const double weight = std::exp(height * shape / PeakBlend);
                        sum += weight - 1.0;
                        gradient += slopeOf * (height * shapeSlope * weight * frequency);
                    }
                }
            }
        }
        slope = gradient / (1.0 + sum);
        return PeakBlend * std::log1p(sum);
    }

    double Terrain::Gullies(const Math::Vector3d& point, const Math::Vector3d& direction, std::uint32_t octaves,
                            Math::Vector3d slope, double amount) const {
        // Bruit d'érosion (méthode de Clay John et Fewes) : dans chaque cellule d'un réseau 3D, des ondes cos le long
        // de la perpendiculaire à la pente, donc des sillons dans le sens de la pente ; les cellules voisines se
        // mélangent par un poids gaussien autour d'un point décalé au hasard. Chaque octave suit la pente totale (le
        // massif et les ravines déjà posées) : les petites ravines partent des grandes, en arbre.
        const Layer& layer = Layers[GullyField];
        const std::uint32_t last = std::min(layer.last, octaves > m_shift ? octaves - m_shift : 0u);
        const auto& P = m_permutation;
        double sum = 0.0;
        double amplitude = GullyAmplitude * amount;
        double frequency = BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first)) * m_scale;
        Math::Vector3d p = point * frequency + layer.offset;
        for (std::uint32_t k = layer.first; k < last; ++k) {
            const double steep = Math::Length(slope);
            const double fade = std::clamp((steep - GullyStartSlope) / (GullyFullSlope - GullyStartSlope), 0.0, 1.0);
            if (fade > 0.0) {
                const Math::Vector3d along = Math::Cross(direction, slope) / steep; // unitaire, tangent, perpendiculaire à la pente
                const Math::Vector3d nearest{std::floor(p.x + 0.5), std::floor(p.y + 0.5), std::floor(p.z + 0.5)};
                const Math::Vector3d f = p - nearest;
                const auto cx = static_cast<std::int64_t>(nearest.x), cy = static_cast<std::int64_t>(nearest.y),
                           cz = static_cast<std::int64_t>(nearest.z);
                double value = 0.0, change = 0.0, weights = 0.0;
                for (int i = -1; i <= 1; ++i) {
                    for (int j = -1; j <= 1; ++j) {
                        for (int l = -1; l <= 1; ++l) {
                            const std::uint32_t h = P[P[P[(cx + i) & 255] + ((cy + j) & 255)] + ((cz + l) & 255)];
                            const Math::Vector3d jitter{P[h] / 255.0 - 0.5, P[h + 1] / 255.0 - 0.5, P[h + 2] / 255.0 - 0.5};
                            const Math::Vector3d d = f - Math::Vector3d{double(i), double(j), double(l)} - jitter * GullyJitter;
                            const double w = std::exp(-GullySharpness * Math::Dot(d, d));
                            const double phase = 2.0 * std::numbers::pi * Math::Dot(d, along);
                            value += std::cos(phase) * w;
                            change -= std::sin(phase) * w;
                            weights += w;
                        }
                    }
                }
                sum += amplitude * fade * value / weights;
                // Pente des ravines (m/m) : dérivée de la phase le long de `along`, cellules par radian / rayon.
                slope += along * (GullyBranch * m_relief * amplitude * fade * change / weights * 2.0 * std::numbers::pi * frequency / m_radius);
            }
            amplitude *= GullyGain;
            frequency *= 2.0;
            p *= 2.0;
        }
        return sum;
    }

    Math::Vector3d Terrain::Warp(const Math::Vector3d& direction, std::uint32_t layerIndex, double amount) const {
        // Pente d'un bruit lent, tangente à la sphère, en radians : un champ de déplacement sans à-coup.
        const Layer& layer = Layers[layerIndex];
        const double total = (1.0 - std::pow(layer.gain, static_cast<double>(layer.last - layer.first))) / (1.0 - layer.gain);
        Math::Vector3d p = direction * (BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first))) + layer.offset;
        Math::Vector3d warp{};
        double amplitude = amount / total;
        for (std::uint32_t k = layer.first; k < layer.last; ++k) {
            warp += Tangent(Noise(p, true).gradient, direction) * amplitude;
            amplitude *= layer.gain;
            p *= 2.0;
        }
        return warp;
    }

    double Terrain::GetHeight(const Math::Vector3d& direction, std::uint32_t octaves) const {
        if (m_relief <= 0.0) {
            return 0.0;
        }
        const double toSlope = m_relief / m_radius; // une unité de hauteur (fraction du relief) par radian, en m/m
        // Points déformés : grandes formes, puis montagnes et collines.
        const Math::Vector3d large = direction + Warp(direction, WarpLargeField, WarpLarge);
        const Math::Vector3d small = large + Warp(direction, WarpSmallField, WarpSmall);
        // Couches, des plus grandes aux plus fines (octave 0 : environ un quart du tour de la planète) :
        // - continents : terres et bassins ;
        const double continents = Fbm(large, direction, Continents, octaves, 0.0);
        const double land = SmoothStep(-0.26, 0.06, continents);
        // - les collines, et les montagnes en chaînes : des bandes le long des lignes où un bruit lent passe par zéro
        //   (tordues par la déformation), au cœur des continents ; le reste est de la plaine ;
        const double hillZone = SmoothStep(0.05, 0.4, Fbm(large, direction, HillZone, octaves, 0.0));
        const double chains = 1.0 - std::abs(Fbm(large, direction, MountainZone, octaves, 0.0));
        const double mountainZone = SmoothStep(ChainStart, ChainEnd, chains) * SmoothStep(0.0, 0.2, continents);
        // - les reliefs eux-mêmes, calculés seulement où ils comptent (une plaine ne paie pas les sommets), érodés
        //   selon leur vraie pente ;
        //   les sommets, leurs versants creusés de ravines ;
        double peaks = 0.0;
        double gullies = 0.0;
        if (mountainZone > 0.0) {
            Math::Vector3d slope; // par radian
            peaks = Peaks(small, direction, octaves, slope);
            const double weight = MountainWeight * mountainZone * PeakHeight;
            gullies = Gullies(large, direction, octaves, slope * (toSlope * weight), mountainZone);
        }
        const double hills = hillZone > 0.0 ? Fbm(small, direction, Hills, octaves, toSlope * HillWeight * hillZone) : 0.0;
        // - le détail fin (bosses, rochers), qui baisse moins vite (DetailGain) : faible dans les plaines.
        const double roughness = DetailPlains + DetailHills * hillZone + DetailMountains * mountainZone;
        const double detail = Fbm(direction, direction, Detail, octaves, toSlope * roughness);
        // Bassins vers -0,4, plaines vers 0,05 à 0,1, montagnes jusqu'au relief.
        const double height = -0.4 + 0.45 * land + 0.15 * continents
                            + MountainWeight * mountainZone * (ChainBase + PeakHeight * peaks) + gullies
                            + HillWeight * hillZone * hills + roughness * detail;
        return m_relief * std::clamp(height, -1.0, 1.0);
    }

    double Terrain::GetOmittedRelief(std::uint32_t octaves) const {
        // Ce que les octaves de rang >= octaves peuvent encore ajouter, au plus fort (montagnes) : la fin de chaque
        // couche, en fraction de la somme de ses amplitudes.
        const auto tail = [](std::uint32_t first, std::uint32_t last, double gain, std::uint32_t octaves) {
            if (octaves >= last) {
                return 0.0;
            }
            const auto from = std::max(octaves, first);
            const double all = (1.0 - std::pow(gain, last - first)) / (1.0 - gain);
            return std::pow(gain, from - first) * (1.0 - std::pow(gain, last - from)) / (1.0 - gain) / all;
        };
        const auto layerTail = [&](LayerIndex index) {
            const Layer& layer = Layers[index];
            const std::uint32_t shift = layer.shifted ? m_shift : 0u;
            return tail(layer.first, layer.last, layer.gain, octaves > shift ? octaves - shift : 0u);
        };
        const double omitted = 0.6 * layerTail(Continents) + MountainWeight * PeakHeight * layerTail(PeakField) + HillWeight * layerTail(Hills)
                             + 2.0 * GullyAmplitude * layerTail(GullyField)
                             + (DetailPlains + DetailHills + DetailMountains) * layerTail(Detail);
        return m_relief * omitted;
    }

    double GetSplitDistance(const Terrain& terrain, std::uint32_t level) {
        // Angle d'une tuile le long des axes de sa face : (π/2) / 2^niveau.
        return LodSplitFactor * 0.5 * std::numbers::pi * terrain.GetRadius() * std::ldexp(1.0, -static_cast<int>(level));
    }

    TileBounds GetTileBounds(const Terrain& terrain, TileKey key) {
        return GetTileBounds(terrain, key, terrain.GetRadius() - 1.5 * terrain.GetRelief() - 1.0, terrain.GetRadius() + terrain.GetRelief());
    }

    TileBounds GetTileBounds(const Terrain& terrain, TileKey key, double lowest, double highest) {
        const double R = terrain.GetRadius();
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads;
        const auto at = [&](std::int64_t i, std::int64_t j) {
            return DirectionFromFace(key.face, GridToFace(x0 + i, key.level), GridToFace(y0 + j, key.level));
        };
        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        constexpr auto full = static_cast<std::int64_t>(TileQuads);
        TileBounds bounds;
        bounds.edge = Math::Length(at(full, 0) - at(0, 0)) * R;
        // Les jupes descendent sous le bord de la tuile.
        lowest -= SkirtDepth(terrain, terrain.GetOctaves(key.level), bounds.edge);
        bounds.origin = at(half, half) * R;
        bounds.center = at(half, half) * (0.5 * (lowest + highest));
        // Les coins et les milieux des bords, au plus bas et au plus haut.
        for (const std::int64_t j : {std::int64_t{0}, half, full}) {
            for (const std::int64_t i : {std::int64_t{0}, half, full}) {
                const Math::Vector3d d = at(i, j);
                for (const double r : {lowest, highest}) {
                    bounds.radius = std::max(bounds.radius, Math::Length(d * r - bounds.center));
                }
            }
        }
        return bounds;
    }

    void BuildTile(const Terrain& terrain, TileKey key, TileMesh& mesh) {
        const double R = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(key.level);
        // Grille avec une rangée de plus tout autour : les normales du bord se calculent comme celles du milieu, et
        // celles des deux tuiles voisines tombent pareil (pas de marche de lumière).
        constexpr std::uint32_t Wide = TileSide + 2;
        thread_local std::vector<Math::Vector3d> positions;
        thread_local std::vector<Math::Vector3d> directions;
        positions.resize(Wide * Wide);
        directions.resize(Wide * Wide);
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads - 1;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads - 1;
        for (std::uint32_t j = 0; j < Wide; ++j) {
            const double t = GridToFace(y0 + j, key.level);
            for (std::uint32_t i = 0; i < Wide; ++i) {
                const Math::Vector3d d = DirectionFromFace(key.face, GridToFace(x0 + i, key.level), t);
                directions[j * Wide + i] = d;
                positions[j * Wide + i] = d * (R + terrain.GetHeight(d, octaves));
            }
        }

        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        mesh.center = DirectionFromFace(key.face, GridToFace(x0 + 1 + half, key.level), GridToFace(y0 + 1 + half, key.level)) * R;
        mesh.vertices.resize(TileVertexCount);
        mesh.radius = 0.0;
        mesh.lowest = std::numeric_limits<double>::max();
        const auto vertex = [&](std::uint32_t i, std::uint32_t j) -> ModelVertex& { return mesh.vertices[j * TileSide + i]; };
        for (std::uint32_t j = 0; j < TileSide; ++j) {
            for (std::uint32_t i = 0; i < TileSide; ++i) {
                const std::uint32_t w = (j + 1) * Wide + (i + 1);
                const Math::Vector3d along = positions[w + 1] - positions[w - 1];
                const Math::Vector3d across = positions[w + Wide] - positions[w - Wide];
                const Math::Vector3d normal = Math::Normalize(Math::Cross(along, across));
                const Math::Vector3d local = positions[w] - mesh.center;
                vertex(i, j) = {Math::Vector3f(local), Math::Vector3f(normal), {0.0f, 0.0f}};
                mesh.radius = std::max(mesh.radius, Math::Length(local));
                mesh.lowest = std::min(mesh.lowest, Math::Length(positions[w]));
            }
        }

        // Forme de la parente : ses sommets (un sur deux de la tuile dans chaque sens) avec son nombre d'octaves, et ses
        // normales, calculées comme elle le fait (voisins à deux pas, une rangée de plus autour). Un sommet entre deux
        // des siens va au milieu du bord de son triangle (la diagonale pour le centre d'un carré : sommets a et c dans
        // BuildTileIndices).
        mesh.morph.resize(TileVertexCount);
        if (key.level == 0) {
            for (std::uint32_t v = 0; v < TileSide * TileSide; ++v) {
                mesh.morph[v] = {{0.0f, 0.0f, 0.0f}, mesh.vertices[v].normal};
            }
        } else {
            constexpr std::uint32_t Coarse = TileQuads / 2 + 3; // de -2 à TileQuads + 2, de deux en deux
            thread_local std::vector<Math::Vector3d> coarse;
            coarse.resize(Coarse * Coarse);
            const std::uint32_t parentOctaves = terrain.GetOctaves(key.level - 1u);
            for (std::uint32_t cj = 0; cj < Coarse; ++cj) {
                const double t = GridToFace(y0 + 1 + 2 * static_cast<std::int64_t>(cj) - 2, key.level);
                for (std::uint32_t ci = 0; ci < Coarse; ++ci) {
                    const Math::Vector3d d = DirectionFromFace(key.face, GridToFace(x0 + 1 + 2 * static_cast<std::int64_t>(ci) - 2, key.level), t);
                    coarse[cj * Coarse + ci] = d * (R + terrain.GetHeight(d, parentOctaves));
                }
            }
            // Sommet de la parente au point (i, j) de la tuile (i et j pairs, de 0 à TileQuads) et sa normale.
            const auto parent = [&](std::uint32_t i, std::uint32_t j) -> const Math::Vector3d& {
                return coarse[(j / 2 + 1) * Coarse + (i / 2 + 1)];
            };
            const auto parentNormal = [&](std::uint32_t i, std::uint32_t j) {
                const std::uint32_t c = (j / 2 + 1) * Coarse + (i / 2 + 1);
                return Math::Normalize(Math::Cross(coarse[c + 1] - coarse[c - 1], coarse[c + Coarse] - coarse[c - Coarse]));
            };
            for (std::uint32_t j = 0; j < TileSide; ++j) {
                for (std::uint32_t i = 0; i < TileSide; ++i) {
                    // Les deux sommets de la parente dont celui-ci est le milieu (le même deux fois s'il est à elle).
                    const std::uint32_t i0 = i & ~1u;
                    const std::uint32_t j0 = j & ~1u;
                    const std::uint32_t i1 = i0 + (i & 1u) * 2;
                    const std::uint32_t j1 = j0 + (j & 1u) * 2;
                    const Math::Vector3d target = (parent(i0, j0) + parent(i1, j1)) * 0.5;
                    const Math::Vector3d normal = Math::Normalize(parentNormal(i0, j0) + parentNormal(i1, j1));
                    const Math::Vector3d position = positions[(j + 1) * Wide + (i + 1)];
                    mesh.morph[j * TileSide + i] = {Math::Vector3f(target - position), Math::Vector3f(normal)};
                    mesh.lowest = std::min(mesh.lowest, Math::Length(target));
                    mesh.radius = std::max(mesh.radius, Math::Length(target - mesh.center));
                }
            }
        }

        // Jupes : le bord parcouru dans le sens trigonométrique (bas, droite, haut, gauche), chaque sommet descendu
        // vers le centre de la planète. Assez profondes pour couvrir l'écart avec une voisine plus grossière (le
        // relief qu'elle n'a pas) ; la normale reste celle du bord, la jupe se confond avec le terrain.
        const double edge = Math::Length(directions[Wide + 1 + TileQuads] - directions[Wide + 1]) * R;
        const double depth = SkirtDepth(terrain, octaves, edge);
        std::uint32_t next = TileSide * TileSide;
        const auto skirt = [&](std::uint32_t i, std::uint32_t j) {
            const std::uint32_t w = (j + 1) * Wide + (i + 1);
            const Math::Vector3d local = positions[w] - directions[w] * depth - mesh.center;
            mesh.morph[next] = mesh.morph[j * TileSide + i]; // glisse avec son bord
            mesh.vertices[next++] = {Math::Vector3f(local), vertex(i, j).normal, {0.0f, 0.0f}};
            mesh.radius = std::max(mesh.radius, Math::Length(local + Math::Vector3d(mesh.morph[j * TileSide + i].offset)));
        };
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(k, 0);
        }
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(TileQuads, k);
        }
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(TileQuads - k, TileQuads);
        }
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(0, TileQuads - k);
        }
    }

    void PrepareTileGpu(const Terrain& terrain, TileKey key, TileGpuInputs& inputs) {
        const Face& f = Faces[key.face % 6];
        const double R = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(key.level);
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads;
        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        const auto tangent = [&](std::int64_t index) { return std::tan(GridToFace(index, key.level) * QuarterPi); };
        const double ac = tangent(x0 + half);
        const double bc = tangent(y0 + half);
        const Math::Vector3d vc = f.normal + f.s * ac + f.t * bc;
        const double lc = Math::Length(vc);
        const Math::Vector3d dc = vc / lc; // = DirectionFromFace au centre : le même que mesh.center / R
        const auto float4 = [](const Math::Vector3d& v, double w) {
            return std::array{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z), static_cast<float>(w)};
        };
        // Profondeur des jupes : comme BuildTile.
        const double edge = Math::Length(DirectionFromFace(key.face, GridToFace(x0 + TileQuads, key.level), GridToFace(y0, key.level))
                                         - DirectionFromFace(key.face, GridToFace(x0, key.level), GridToFace(y0, key.level))) * R;
        const double depth = SkirtDepth(terrain, octaves, edge);
        inputs.center = float4(dc, lc);
        inputs.axisN = float4(f.normal, R);
        inputs.axisS = float4(f.s, terrain.GetRelief());
        inputs.axisT = float4(f.t, depth);
        inputs.options = {octaves, key.level > 0 ? terrain.GetOctaves(key.level - 1u) : octaves, key.level, terrain.GetScaleShift()};
        for (std::uint32_t i = 0; i < TileGpuInputs::Columns; ++i) {
            const double a = tangent(x0 + static_cast<std::int64_t>(i) - 2);
            const double b = tangent(y0 + static_cast<std::int64_t>(i) - 2);
            inputs.columns[i] = {static_cast<float>(a - ac), static_cast<float>(a + ac), static_cast<float>(b - bc), static_cast<float>(b + bc)};
        }
        // Point de chaque octave au centre de la tuile, calculé comme Fbm (doublé d'une octave à l'autre).
        std::uint32_t octave = 0;
        for (const Layer& layer : Layers) {
            const double scale = layer.scaled ? terrain.GetScale() : 1.0;
            Math::Vector3d p = dc * (BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first)) * scale) + layer.offset;
            for (std::uint32_t k = layer.first; k < layer.last; ++k, ++octave) {
                const Math::Vector3d cell{std::floor(p.x), std::floor(p.y), std::floor(p.z)};
                inputs.cells[octave] = {static_cast<std::int32_t>(static_cast<std::int64_t>(cell.x) & 255),
                                        static_cast<std::int32_t>(static_cast<std::int64_t>(cell.y) & 255),
                                        static_cast<std::int32_t>(static_cast<std::int64_t>(cell.z) & 255), 0};
                inputs.fractions[octave] = float4(p - cell, BaseFrequency * std::ldexp(1.0, static_cast<int>(k)) * scale);
                p *= 2.0;
            }
        }
        const auto& permutation = terrain.GetPermutation();
        for (std::uint32_t i = 0; i < inputs.permutation.size(); ++i) {
            inputs.permutation[i] = static_cast<std::uint32_t>(permutation[4 * i]) | (static_cast<std::uint32_t>(permutation[4 * i + 1]) << 8)
                                  | (static_cast<std::uint32_t>(permutation[4 * i + 2]) << 16)
                                  | (static_cast<std::uint32_t>(permutation[4 * i + 3]) << 24);
        }
    }

    void BuildTileSurface(const Terrain& terrain, TileKey key, Math::Vector3d& center, std::vector<Math::Vector3f>& positions) {
        // Mêmes calculs que BuildTile, dans le même ordre : mêmes doubles, donc mêmes floats.
        const double R = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(key.level);
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads;
        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        center = DirectionFromFace(key.face, GridToFace(x0 + half, key.level), GridToFace(y0 + half, key.level)) * R;
        positions.resize(TileSide * TileSide);
        for (std::uint32_t j = 0; j < TileSide; ++j) {
            const double t = GridToFace(y0 + j, key.level);
            for (std::uint32_t i = 0; i < TileSide; ++i) {
                const Math::Vector3d d = DirectionFromFace(key.face, GridToFace(x0 + i, key.level), t);
                positions[j * TileSide + i] = Math::Vector3f(d * (R + terrain.GetHeight(d, octaves)) - center);
            }
        }
    }

    TileKey FindTile(const Math::Vector3d& direction, std::uint32_t level) {
        std::uint32_t face = 0;
        double s = 0.0;
        double t = 0.0;
        FaceFromDirection(direction, face, s, t);
        const double tiles = static_cast<double>(std::uint64_t{1} << level);
        const auto cell = [&](double value) {
            return static_cast<std::uint32_t>(std::clamp(std::floor((value + 1.0) * 0.5 * tiles), 0.0, tiles - 1.0));
        };
        return {static_cast<std::uint8_t>(face), static_cast<std::uint8_t>(level), cell(s), cell(t)};
    }

    std::vector<std::uint32_t> BuildTileIndices() {
        std::vector<std::uint32_t> indices;
        indices.reserve(6 * TileQuads * TileQuads + 4 * 6 * TileQuads);
        for (std::uint32_t j = 0; j < TileQuads; ++j) {
            for (std::uint32_t i = 0; i < TileQuads; ++i) {
                const std::uint32_t a = j * TileSide + i;
                const std::uint32_t b = a + 1;
                const std::uint32_t c = a + TileSide + 1;
                const std::uint32_t d = a + TileSide;
                indices.insert(indices.end(), {a, b, c, a, c, d});
            }
        }
        // Le bord de la grille, dans l'ordre des jupes (BuildTile).
        const auto border = [](std::uint32_t edge, std::uint32_t k) {
            switch (edge) {
                case 0: return k;                                               // bas, s croissant
                case 1: return k * TileSide + TileQuads;                        // droite, t croissant
                case 2: return TileQuads * TileSide + (TileQuads - k);          // haut, s décroissant
                default: return (TileQuads - k) * TileSide;                     // gauche, t décroissant
            }
        };
        for (std::uint32_t edge = 0; edge < 4; ++edge) {
            const std::uint32_t first = TileSide * TileSide + edge * TileSide;
            for (std::uint32_t k = 0; k < TileQuads; ++k) {
                const std::uint32_t top0 = border(edge, k);
                const std::uint32_t top1 = border(edge, k + 1);
                const std::uint32_t low0 = first + k;
                const std::uint32_t low1 = first + k + 1;
                // Tournée vers l'extérieur de la tuile : (bord, jupe, bord suivant) puis (bord suivant, jupe, jupe suivante).
                indices.insert(indices.end(), {top0, low0, top1, top1, low0, low1});
            }
        }
        return indices;
    }
}
