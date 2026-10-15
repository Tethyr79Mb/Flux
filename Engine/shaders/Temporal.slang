// Anticrénelage temporel (TAA), entre la scène et le bloom (PostRenderer::ResolveTemporal).
// Chaque image est dessinée décalée d'une fraction de pixel différente (TemporalView::jitter) : un bord oblique tombe
// tantôt d'un côté du centre d'un pixel, tantôt de l'autre. Ce shader mélange l'image de la scène à l'historique
// (le résultat de l'image précédente, retrouvé là où le même point se trouvait alors) : 10 % de nouveau, 90 % d'ancien,
// soit la moyenne d'une dizaine d'images. Les bords crénelés deviennent des dégradés, et les bruits qui changent à
// chaque image (grain des nuages) s'effacent.
// Ce qui était caché à l'image précédente (un bord qui se découvre) ou qui a changé (une lumière qui s'allume) donne
// un historique faux : il est ramené dans les couleurs du voisinage 3 x 3 de l'image actuelle (Salvi, « An excursion
// in temporal supersampling », GDC 2016 ; Karis, « High quality temporal supersampling », SIGGRAPH 2014), sinon des
// traînées suivraient tout ce qui bouge.

// Doit correspondre à TemporalParams dans PostRenderer.hpp.
struct TemporalParams
{
    // Par colonnes : clip space d'un pixel de cette image, sans décalage (x, y, sa profondeur, 1) -> clip space de
    // l'image précédente. Profondeur 0 (ciel, reverse-Z infini) : w passe à 0, seule la direction compte.
    float4 reprojection[4];
    float4 texel; // xy : 1 / taille de l'image ; z : part de l'image actuelle ; w : 1 si l'historique est valable
};
[[vk::push_constant]] ConstantBuffer<TemporalParams> temporal;

[[vk::binding(0, 0)]] Sampler2D current; // la scène de cette image (HDR)
[[vk::binding(1, 0)]] Sampler2D history; // le résultat de l'image précédente (HDR)
[[vk::binding(2, 0)]] Sampler2D depth;   // profondeur de la scène (reverse-Z : 1 près, 0 à l'infini)
[[vk::binding(3, 0)]] Sampler2D motion;  // mouvement propre des objets (uv), en plus de celui de la caméra

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD; // 0 à 1, origine en haut à gauche comme les textures
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.uv = clip * 0.5 + 0.5;
    return output;
}

// Un pixel infini ou NaN resterait à jamais dans l'historique : ramené à une valeur finie avant tout mélange.
float3 Finite(float3 color)
{
    return any(isnan(color)) ? float3(0.0) : clamp(color, float3(0.0), float3(60000.0));
}

// Couleurs HDR ramenées entre 0 et 1 (c / (1 + max)) avant les moyennes et les mélanges : un reflet du soleil 1 000
// fois plus fort que son voisinage pèserait sinon sur toute la moyenne et clignoterait. Défait par Expand.
float3 Compress(float3 color)
{
    return color / (1.0 + max(max(color.r, color.g), color.b));
}

float3 Expand(float3 color)
{
    return color / max(1.0 - max(max(color.r, color.g), color.b), 1.0e-4);
}

// Luminance et deux écarts de couleur : la boîte du voisinage y serre mieux les couleurs qu'en RGB.
float3 ToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 FromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Historique lu par un filtre de Catmull-Rom (9 points ramenés à 5 lectures bilinéaires, les coins négligés) : une
// lecture bilinéaire simple flouterait un peu plus l'image à chaque image, jusqu'à la rendre floue.
float3 SampleHistory(float2 uv)
{
    const float2 size = 1.0 / temporal.texel.xy;
    const float2 position = uv * size;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (center - 1.0) * temporal.texel.xy;
    const float2 t3 = (center + 2.0) * temporal.texel.xy;
    const float2 t12 = (center + w2 / w12) * temporal.texel.xy;
    float3 sum = history.SampleLevel(float2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y);
    sum += history.SampleLevel(float2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y);
    sum += history.SampleLevel(t12, 0.0).rgb * (w12.x * w12.y);
    sum += history.SampleLevel(float2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y);
    sum += history.SampleLevel(float2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    const float total = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / total, float3(0.0));
}

// Ramène `value` dans la boîte [low, high] le long du segment qui le relie au centre de la boîte (plutôt que de
// couper chaque composante à part, qui change la teinte).
float3 ClipToBox(float3 value, float3 low, float3 high)
{
    const float3 center = 0.5 * (high + low);
    const float3 extent = max(0.5 * (high - low), float3(1.0e-5));
    const float3 offset = value - center;
    const float3 units = abs(offset / extent);
    const float largest = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? center + offset / largest : value;
}

[shader("fragment")]
float4 fragmentTemporal(VertexOutput input) : SV_Target
{
    const float2 uv = input.uv;
    const float2 texel = temporal.texel.xy;

    // Voisinage 3 x 3 : moyenne et écart type des couleurs, et le pixel le plus proche de la caméra (le mouvement
    // pris là garde nets les bords d'un objet qui bouge devant le fond).
    float3 sum = float3(0.0);
    float3 squares = float3(0.0);
    float3 center = float3(0.0);
    float closest = depth.SampleLevel(uv, 0.0).r; // à égalité (le ciel, une surface plane), le pixel lui-même
    float2 closestUv = uv;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 at = uv + float2(x, y) * texel;
            const float3 color = ToYCoCg(Compress(Finite(current.SampleLevel(at, 0.0).rgb)));
            sum += color;
            squares += color * color;
            if (x == 0 && y == 0) {
                center = color;
            }
            const float d = depth.SampleLevel(at, 0.0).r;
            if (d > closest) {
                closest = d;
                closestUv = at;
            }
        }
    }
    const float3 mean = sum / 9.0;
    const float3 deviation = sqrt(max(squares / 9.0 - mean * mean, float3(0.0)));

    // Où ce point était à l'image précédente : le déplacement du pixel le plus proche, dû à la caméra (par la
    // profondeur) plus le mouvement propre de l'objet, appliqué à ce pixel-ci.
    const float2 ndc = closestUv * 2.0 - 1.0;
    const float4 m[4] = temporal.reprojection;
    const float4 previous = m[0] * ndc.x + m[1] * ndc.y + m[2] * closest + m[3];
    const float2 velocity = previous.xy / previous.w * 0.5 + 0.5 + motion.SampleLevel(closestUv, 0.0).rg - closestUv;
    const float2 previousUv = uv + velocity;
    const bool inside = previous.w > 0.0 && all(previousUv >= 0.0) && all(previousUv <= 1.0);
    if (temporal.texel.w <= 0.0 || !inside) {
        return float4(Expand(FromYCoCg(center)), 1.0); // rien à reprendre : l'image telle quelle
    }

    // Boîte de la moyenne plus ou moins un écart type, agrandie jusqu'au pixel lui-même : un point isolé (une étoile,
    // un reflet) serait sinon ramené vers le fond sombre de ses voisins et pâlirait.
    const float3 low = min(mean - deviation, center);
    const float3 high = max(mean + deviation, center);
    const float3 old = ClipToBox(ToYCoCg(Compress(Finite(SampleHistory(previousUv)))), low, high);
    const float3 blended = lerp(old, center, temporal.texel.z);
    return float4(Expand(FromYCoCg(blended)), 1.0);
}
