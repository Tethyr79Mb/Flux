// Mips des images (BuildMipChain) : rangement des niveaux, couverture gardée pour le feuillage à seuil d'alpha.
#include "Check.hpp"

#include "Flux/Assets/Image.hpp"

#include <cstdint>
#include <vector>

using namespace Flux;

namespace {
    // Part des pixels du niveau (offset, taille side x side) dont l'alpha atteint le seuil.
    float LevelCoverage(const std::vector<std::uint8_t>& chain, std::size_t offset, int side, float cutoff) {
        int passed = 0;
        for (int i = 0; i < side * side; ++i) {
            passed += static_cast<float>(chain[offset + static_cast<std::size_t>(i) * 4 + 3]) / 255.0f >= cutoff ? 1 : 0;
        }
        return static_cast<float>(passed) / static_cast<float>(side * side);
    }
}

bool RunImageTests() {
    // Feuillage clairsemé : 30 % de pixels opaques verts, le reste transparent noir, en désordre (comme des aiguilles).
    constexpr int Side = 64;
    ImageData image;
    image.size = {Side, Side};
    image.rgba.resize(Side * Side * 4);
    std::uint32_t state = 12345;
    int opaque = 0;
    for (int i = 0; i < Side * Side; ++i) {
        state = state * 1664525u + 1013904223u;
        const bool leaf = (state >> 8) % 100 < 30;
        opaque += leaf ? 1 : 0;
        image.rgba[static_cast<std::size_t>(i) * 4 + 1] = leaf ? 200 : 0;
        image.rgba[static_cast<std::size_t>(i) * 4 + 3] = leaf ? 255 : 0;
    }
    const float coverage = static_cast<float>(opaque) / (Side * Side);

    std::uint32_t levels = 0;
    const std::vector<std::uint8_t> chain = BuildMipChain(image, 0.5f, levels);
    CHECK(levels == 7); // 64, 32, 16, 8, 4, 2, 1
    CHECK(chain.size() == (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4);
    CHECK(std::vector<std::uint8_t>(chain.begin(), chain.begin() + Side * Side * 4) == image.rgba); // niveau 0 tel quel

    // Moyenne simple (sans seuil) : l'alpha tombe vers 0,3 partout et presque plus rien ne passe 0,5 dès le niveau 3.
    std::uint32_t plainLevels = 0;
    const std::vector<std::uint8_t> plain = BuildMipChain(image, 0.0f, plainLevels);
    std::size_t offset = Side * Side * 4;
    int side = Side / 2;
    for (int level = 1; level <= 4; ++level) {
        const float kept = LevelCoverage(chain, offset, side, 0.5f);
        CHECK(kept > coverage * 0.6f && kept < coverage * 1.5f);
        if (level == 3) {
            CHECK(LevelCoverage(plain, offset, side, 0.5f) < coverage * 0.3f);
        }
        offset += static_cast<std::size_t>(side) * side * 4;
        side /= 2;
    }

    // Couleur moyennée par l'alpha : les pixels transparents noirs n'assombrissent pas le vert du feuillage.
    const std::size_t level1 = Side * Side * 4;
    for (int i = 0; i < 32 * 32; ++i) {
        const std::size_t p = level1 + static_cast<std::size_t>(i) * 4;
        if (chain[p + 3] > 0) {
            CHECK(chain[p + 1] >= 199 && chain[p + 1] <= 201 && chain[p] == 0);
        }
    }

    // Image vide ou abîmée : rien.
    ImageData broken;
    broken.size = {4, 4};
    CHECK(BuildMipChain(broken, 0.5f, levels).empty() && levels == 0);
    // Côtés différents : niveaux jusqu'à 1 x 1 sur le plus grand.
    ImageData wide;
    wide.size = {8, 2};
    wide.rgba.assign(8 * 2 * 4, 255);
    CHECK(BuildMipChain(wide, 0.5f, levels).size() == (8 * 2 + 4 * 1 + 2 * 1 + 1 * 1) * 4 && levels == 4);
    return true;
}
