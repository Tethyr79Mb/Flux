// Courbe de tons AgX (Troy Sobotka ; approximation polynomiale de Benjamin Wrensch, 2023) : de la lumière HDR de la
// scène à la couleur de l'écran. Les couleurs très fortes tournent au blanc au lieu de virer à la teinte pure, comme
// sur une pellicule. Même calcul que ToneCurve.cpp (couleur de fond des scènes sans ciel, tests) : les deux changent
// ensemble.

static const float AgxMinEv = -12.47393; // plage de la courbe, en stops autour du gris moyen
static const float AgxMaxEv = 4.026069;
// Primaires resserrées vers le blanc à l'entrée (les couleurs fortes blanchissent), puis leur inverse à la sortie.
// Matrices écrites ligne par ligne : mul(m, v), ligne i fois v.
static const float3x3 AgxInset = float3x3(0.842479062253094, 0.0784335999999992, 0.0792237451477643,
                                          0.0423282422610123, 0.878468636469772, 0.0791661274605434,
                                          0.0423756549057051, 0.0784336, 0.879142973793104);
static const float3x3 AgxOutset = float3x3(1.19687900512017, -0.0980208811401368, -0.0990297440797205,
                                           -0.0528968517574562, 1.15190312990417, -0.0989611768448433,
                                           -0.0529716355144438, -0.0980434501171241, 1.15107367264116);

// Le contraste d'AgX en polynôme de degré 6, croissant de 0 à 1.
float3 AgxContrast(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Réglage « Punchy » d'AgX (comme dans Blender) : AgX de base délave les couleurs (une planète vue de l'espace tourne
// au blanc) ; un peu plus de contraste et de saturation leur rend leur force, le blanc des très fortes lumières reste.
static const float AgxLookPower = 1.35;
static const float AgxLookSaturation = 1.4;
static const float3 AgxLuma = float3(0.2126, 0.7152, 0.0722);

float3 AgxLook(float3 v)
{
    v = pow(max(v, 0.0), AgxLookPower);
    const float luma = dot(v, AgxLuma);
    return luma + AgxLookSaturation * (v - luma);
}

// Couleur affichée, linéaire : la courbe donne une couleur pour un écran de gamma 2,2, la fenêtre (sRGB) l'encode
// elle-même.
float3 ToneMap(float3 color, float exposure)
{
    const float3 ev = clamp(log2(max(mul(AgxInset, color * exposure), 1.0e-10)), AgxMinEv, AgxMaxEv);
    const float3 display = mul(AgxOutset, AgxLook(AgxContrast((ev - AgxMinEv) / (AgxMaxEv - AgxMinEv))));
    return pow(saturate(display), 2.2);
}

// Lumière qui, à l'exposition 1, ressort de la courbe en `color` (0 à 1, linéaire). Contraste inversé par dichotomie
// (16 pas, à 1/65536 près) : réservé aux petites surfaces (flammes).
float3 InverseToneMap(float3 color)
{
    const float3 looked = mul(AgxInset, pow(saturate(color), 1.0 / 2.2));
    // AgxLook défait : la saturation garde la luminance, puis la puissance.
    const float luma = dot(looked, AgxLuma);
    const float3 curved = pow(max(luma + (looked - luma) / AgxLookSaturation, 0.0), 1.0 / AgxLookPower);
    float3 low = float3(0.0);
    float3 high = float3(1.0);
    for (int i = 0; i < 16; ++i) {
        const float3 middle = 0.5 * (low + high);
        const bool3 below = AgxContrast(middle) < curved;
        low = select(below, middle, low);
        high = select(below, high, middle);
    }
    const float3 inset = exp2(AgxMinEv + 0.5 * (low + high) * (AgxMaxEv - AgxMinEv));
    return max(mul(AgxOutset, inset), 0.0);
}
