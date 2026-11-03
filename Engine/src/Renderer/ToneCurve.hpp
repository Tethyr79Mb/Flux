#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"

// Courbe de tons AgX (Troy Sobotka, approximation polynomiale de Benjamin Wrensch, 2023) : de la lumière HDR de la
// scène à la couleur de l'écran. Les couleurs très fortes tournent au blanc au lieu de virer à la teinte pure (le
// jaune d'une flamme reste jaune pâle, pas un aplat jaune saturé comme avec ACES). Même calcul que ToneMap dans
// shaders/Post.slang ; ici pour la couleur de fond des scènes sans ciel et pour les tests.
namespace Flux::ToneCurve {
    // Couleur affichée (linéaire, avant l'encodage sRGB de la fenêtre).
    FLUX_API Math::LinearColor ToneMap(Math::LinearColor color, float exposure);
    // Couleur HDR qui donne `color` après la courbe : le fond d'une scène sans ciel garde la couleur choisie, quelle que
    // soit l'exposition. Couleurs de 0 à 1 ; le blanc pur, que la courbe n'atteint jamais, revient à 0,5 % près.
    FLUX_API Math::LinearColor InverseToneMap(Math::LinearColor color, float exposure);
}
