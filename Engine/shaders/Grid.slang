// Grille au sol de l'éditeur (plan y = 0 du monde), comme celles d'Unreal et de Godot (PostRenderer::DrawGrid).
// Un triangle couvre la vue 3D ; chaque pixel suit son rayon depuis la caméra jusqu'au plan, dessine les lignes du
// mètre, des 10 m et des 100 m autour du point touché, et s'efface derrière ce que la scène a dessiné plus près
// (profondeur de la scène relue, aucune écrite). Dessinée après la courbe de tons et l'anticrénelage : les lignes
// gardent la même couleur quelle que soit l'exposition, et ne traînent pas quand la caméra bouge.

// Doit correspondre à GridParams dans PostRenderer.hpp.
struct GridParams
{
    float4 inverseViewProjection[4]; // par colonnes : clip sans décalage -> relatif à la caméra
    float4 depthRow;    // ligne z de la projection (relatif à la caméra -> clip)
    float4 wRow;        // ligne w
    float4 camera;      // x, z : position de la caméra modulo 1000 m (motif des lignes) ; y : hauteur au-dessus du plan
    float4 axes;        // x, z : position de la caméra (axes du monde) ; y : distance où la grille s'efface
};
[[vk::push_constant]] ConstantBuffer<GridParams> grid;

[[vk::binding(0, 0)]] Sampler2D sceneDepth;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 clip     : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.clip = clip;
    output.uv = clip * 0.5 + 0.5;
    return output;
}

// Couverture (0 à 1) des lignes de pas `cell`, épaisses d'un pixel et demi, anticrénelées par les dérivées. Elles
// s'effacent quand une case ne fait plus que quelques pixels : sinon un moiré au loin.
float Lines(float2 coord, float cell)
{
    const float2 scaled = coord / cell;
    const float2 perPixel = max(fwidth(scaled), float2(1.0e-6));
    const float2 distance = abs(frac(scaled - 0.5) - 0.5) / perPixel; // en pixels jusqu'à la ligne la plus proche
    const float line = 1.0 - saturate(min(distance.x, distance.y) / 1.5);
    const float fade = saturate((0.25 - max(perPixel.x, perPixel.y)) / 0.15);
    return line * fade;
}

// Couverture de la ligne coord = 0 (un axe du monde), un peu plus épaisse.
float Axis(float coord)
{
    return 1.0 - saturate(abs(coord) / max(fwidth(coord), 1.0e-6) / 2.0);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // Rayon du pixel : le point du plan proche (profondeur 1 en reverse-Z) vu depuis la caméra, à l'origine.
    const float4 m[4] = grid.inverseViewProjection;
    const float4 near = m[0] * input.clip.x + m[1] * input.clip.y + m[2] + m[3];
    const float3 direction = normalize(near.xyz / near.w);
    const float height = grid.camera.y;
    if (direction.y * height >= 0.0) {
        discard; // regard vers le ciel (ou caméra dans le plan) : le rayon ne croise pas la grille
    }
    const float distance = -height / direction.y;
    const float3 hit = direction * distance;

    // Caché par ce que la scène a dessiné devant (reverse-Z : plus proche = plus grand). Une marge relative : un sol
    // posé à y = 0 laisse voir la grille au lieu de clignoter avec elle.
    const float depth = dot(grid.depthRow, float4(hit, 1.0)) / dot(grid.wRow, float4(hit, 1.0));
    if (sceneDepth.SampleLevel(input.uv, 0.0).r > depth * 1.001) {
        discard;
    }

    const float2 coord = hit.xz + grid.camera.xz;
    const float minor = Lines(coord, 1.0);
    const float major = Lines(coord, 10.0);
    const float huge = Lines(coord, 100.0);
    float4 color = float4(0.5, 0.5, 0.5, max(minor * 0.25, max(major * 0.45, huge * 0.6)));
    // Axes du monde aux couleurs du gizmo : X en rouge (ligne z = 0), Z en bleu (ligne x = 0).
    const float2 world = hit.xz + grid.axes.xz;
    const float axisX = Axis(world.y);
    const float axisZ = Axis(world.x);
    color = lerp(color, float4(0.85, 0.2, 0.2, 0.9), axisX);
    color = lerp(color, float4(0.2, 0.4, 0.9, 0.9), axisZ * (1.0 - axisX));
    // Effacée au loin, et à l'horizon où les cases s'écrasent.
    color.a *= saturate(1.0 - distance / grid.axes.y) * saturate(abs(direction.y) * 6.0);
    if (color.a <= 0.002) {
        discard;
    }
    return color;
}
