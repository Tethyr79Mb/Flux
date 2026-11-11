// Ombres du soleil des modèles fixes en rayons (RayShadowRenderer), après la passe de la scène. Elle a écrit à part la
// lumière directe du soleil de chaque pixel opaque (MeshRays.slang, SV_Target2), sans l'ombre des modèles tracés :
// 1. computeTrace : un rayon par pixel qui reçoit du soleil, vers un point du disque du soleil tiré par pixel et par
//    image ; visibilité (0 ou 1) et distance de l'obstacle.
// 2. computeFilter, deux fois (lignes puis colonnes) : moyenne pondérée des voisins de même profondeur, sur une
//    largeur qui suit la pénombre (distance de l'obstacle x tangente du rayon apparent du soleil) : nette au contact,
//    large loin de l'obstacle. L'anticrénelage temporel lisse ce qui reste.
// 3. fragmentComposite : retire de l'image de la scène la lumière du soleil qui n'arrive pas (mélange additif d'une
//    valeur négative). Exact : la lumière directe du soleil s'ajoute au reste.
// Compilé seulement pour un GPU qui a les ray queries (RHI::Device::HasRayTracing).

// Doit correspondre à RayShadowParams dans RayShadowRenderer.cpp.
struct RayShadowParams
{
    float4 right;   // xyz : droite de la caméra à l'échelle du champ ; w : décalage d'un sous-pixel en x (clip)
    float4 up;      // xyz : haut, de même ; w : décalage en y
    float4 forward; // xyz : avant (unitaire) ; w : plan proche (profondeur = proche / distance devant la caméra)
    float4 toSun;   // xyz : vers le soleil (unitaire) ; w : tangente de son rayon apparent
    float4 size;    // xy : taille de l'image ; z : mètres par pixel à 1 m devant la caméra ; w : inutilisé
    uint4 options;  // x : numéro de l'image (bruit) ; y : 0 lignes, 1 colonnes (computeFilter)
};
[[vk::push_constant]] ConstantBuffer<RayShadowParams> params;

[[vk::binding(0, 0)]] Sampler2D sceneDepth;
[[vk::binding(1, 0)]] Sampler2D sceneSun; // lumière directe du soleil (rgb), 0 sans soleil
[[vk::binding(2, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> target;
[[vk::binding(3, 0)]] Sampler2D source; // x : visibilité ; y : distance de l'obstacle (m), 0 sans obstacle
[[vk::binding(4, 0)]] RaytracingAccelerationStructure sceneStructure;

static const uint GroupSize = 8;
// Plus loin, pas de rayon : l'ombre d'un objet sur un point à 20 km ne se voit plus.
static const float MaxDistance = 20000.0;
// Demi-largeur la plus grande du lissage, en pixels (lus un sur deux).
static const int MaxRadius = 16;

// Distance devant la caméra du pixel, 0 pour le fond (reverse-Z infini : profondeur 0 à l'infini).
float ViewDepth(int2 pixel)
{
    const float depth = sceneDepth.Load(int3(pixel, 0)).r;
    return depth > 0.0 ? params.forward.w / depth : 0.0;
}

// Position relative à la caméra du pixel, à `viewDepth` devant elle.
float3 PixelPosition(int2 pixel, float viewDepth)
{
    const float2 uv = (float2(pixel) + 0.5) / params.size.xy;
    const float2 clip = uv * 2.0 - 1.0 - float2(params.right.w, params.up.w); // y vers le bas, sans le sous-pixel
    return (params.forward.xyz + params.right.xyz * clip.x - params.up.xyz * clip.y) * viewDepth;
}

// Bruit à gradient entrelacé, décalé à chaque image (comme ShadowRotation dans Mesh.slang).
float Noise(float2 pixel, float offset)
{
    const float2 p = pixel + float(params.options.x % 64u) * offset;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[shader("compute")]
[numthreads(GroupSize, GroupSize, 1)]
void computeTrace(uint3 id : SV_DispatchThreadID)
{
    const int2 pixel = int2(id.xy);
    if (any(float2(pixel) >= params.size.xy)) {
        return;
    }
    const float viewDepth = ViewDepth(pixel);
    const float3 sun = sceneSun.Load(int3(pixel, 0)).rgb;
    if (viewDepth <= 0.0 || all(sun <= 0.0)) {
        target[pixel] = float2(1.0, 0.0); // fond, ou pas de soleil ici (nuit, ombre du relief) : rien à chercher
        return;
    }
    const float3 position = PixelPosition(pixel, viewDepth);
    const float distance = length(position);
    if (distance > MaxDistance) {
        target[pixel] = float2(1.0, 0.0);
        return;
    }
    const float3 toSun = params.toSun.xyz;
    const float3 side = normalize(abs(toSun.y) < 0.9 ? cross(toSun, float3(0.0, 1.0, 0.0)) : cross(toSun, float3(1.0, 0.0, 0.0)));
    const float3 sunUp = cross(side, toSun);
    const float angle = 6.2831853 * Noise(float2(pixel), 5.588238);
    const float2 disk = sqrt(Noise(float2(pixel.yx), 7.123617)) * float2(cos(angle), sin(angle)) * params.toSun.w;
    RayDesc ray;
    // Départ un peu vers la caméra (la surface ne se touche pas elle-même : précision de la profondeur et des
    // positions, qui baisse avec la distance) et vers le soleil.
    ray.Origin = position * (1.0 - 1.0e-4) + toSun * (0.005 + 2.0e-5 * distance);
    ray.Direction = normalize(toSun + side * disk.x + sunUp * disk.y);
    ray.TMin = 0.0;
    ray.TMax = MaxDistance;
    // Seuil d'alpha : pas opaque dans la structure, son ombre reste dans les cascades.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(sceneStructure, 0, 0xFF, ray);
    query.Proceed();
    const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    target[pixel] = hit ? float2(0.0, query.CommittedRayT()) : float2(1.0, 0.0);
}

[shader("compute")]
[numthreads(GroupSize, GroupSize, 1)]
void computeFilter(uint3 id : SV_DispatchThreadID)
{
    const int2 pixel = int2(id.xy);
    if (any(float2(pixel) >= params.size.xy)) {
        return;
    }
    const float2 center = source.Load(int3(pixel, 0)).xy;
    const float viewDepth = ViewDepth(pixel);
    if (viewDepth <= 0.0) {
        target[pixel] = center;
        return;
    }
    const int2 step = params.options.y == 0 ? int2(2, 0) : int2(0, 2);
    const int2 size = int2(params.size.xy);
    // Distance moyenne des obstacles autour (lignes) ou celle que la passe des lignes a gardée (colonnes) : un pixel
    // éclairé au bord d'une ombre se lisse autant que ses voisins dans l'ombre.
    float blocker = center.y;
    if (params.options.y == 0) {
        float sum = 0.0;
        float count = 0.0;
        for (int i = -MaxRadius / 2; i <= MaxRadius / 2; ++i) {
            const int2 at = clamp(pixel + step * i, int2(0), size - 1);
            const float2 value = source.Load(int3(at, 0)).xy;
            if (value.y > 0.0) {
                sum += value.y;
                count += 1.0;
            }
        }
        blocker = count > 0.0 ? sum / count : 0.0;
    }
    // Largeur de la pénombre au sol, en pixels à cette distance ; au moins un pixel (bord adouci, pas d'escalier).
    const float penumbra = blocker * params.toSun.w / max(params.size.z * viewDepth, 1.0e-6);
    const float radius = clamp(penumbra, 1.0, float(MaxRadius));
    const float sigma = max(radius * 0.5, 0.5);
    float visibility = 0.0;
    float total = 0.0;
    for (int i = -MaxRadius / 2; i <= MaxRadius / 2; ++i) {
        const float offset = float(2 * i);
        if (abs(offset) > radius + 1.0) {
            continue;
        }
        const int2 at = clamp(pixel + step * i, int2(0), size - 1);
        const float depth = ViewDepth(at);
        // Même surface : profondeur à quelques pour cent près (plus loin à mesure qu'on s'écarte, pour un sol rasant).
        const float tolerance = viewDepth * (0.02 + 0.01 * abs(offset));
        const float weight = exp(-0.5 * offset * offset / (sigma * sigma)) * (abs(depth - viewDepth) < tolerance ? 1.0 : 0.0);
        visibility += source.Load(int3(at, 0)).x * weight;
        total += weight;
    }
    target[pixel] = float2(total > 0.0 ? visibility / total : center.x, blocker);
}

struct VertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexComposite(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    return output;
}

// Mélange additif : ce qui sort est ajouté à l'image de la scène.
[shader("fragment")]
float4 fragmentComposite(VertexOutput input) : SV_Target
{
    const int3 pixel = int3(int2(input.position.xy), 0);
    const float visibility = saturate(source.Load(pixel).x);
    const float3 sun = sceneSun.Load(pixel).rgb;
    return float4(-sun * (1.0 - visibility), 0.0);
}

// Vue de débogage (RayShadowRenderer::SetShowMask), à la place de l'image : rouge où le rayon est arrêté (`source` : les
// rayons avant le lissage), vert selon la lumière du soleil reçue. Jaune : ombre juste ; vert seul : soleil reçu que
// rien n'arrête ; noir : pas de soleil (rien à chercher).
[shader("fragment")]
float4 fragmentMask(VertexOutput input) : SV_Target
{
    const int3 pixel = int3(int2(input.position.xy), 0);
    const float3 sun = sceneSun.Load(pixel).rgb;
    const bool lit = any(sun > 0.0);
    const float hit = lit ? 1.0 - saturate(source.Load(pixel).x) : 0.0;
    const float received = lit ? 0.2 + 0.8 * saturate(dot(sun, float3(0.2126, 0.7152, 0.0722))) : 0.0;
    return float4(hit, received, 0.0, 1.0);
}

