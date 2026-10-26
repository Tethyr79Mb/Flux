// Traçantes des projectiles (TracerRenderer) : chaque projectile est un trait lumineux entre sa tête et sa queue,
// une bande tournée vers la caméra. Toutes en un seul appel de dessin : 6 sommets par traçante, sans buffer de
// sommets, les données lues dans un buffer de stockage. Mélange additif : ni tri ni écriture de profondeur.

// Doit correspondre à TracerData dans TracerRenderer.hpp.
struct TracerData
{
    float4 head;  // xyz : tête relative à la caméra (m) ; w : largeur (m)
    float4 tail;  // xyz : queue relative à la caméra (m) ; w : 1 pour un éclat d'impact (point rond, sans queue)
    float4 color; // rgb : couleur x éclat
};
[[vk::binding(0, 0)]] StructuredBuffer<TracerData> tracers;

// Doit correspondre à TracerParams dans TracerRenderer.hpp.
struct TracerParams
{
    float4 viewToClip[4]; // relatif à la caméra -> clip space, par colonnes (comme Math::Matrix4f)
    float4 options;       // x : angle d'un pixel (rad)
};
[[vk::push_constant]] ConstantBuffer<TracerParams> params;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0; // x : -1 à 1 en travers ; y : 0 à la tête, 1 à la queue
    float3 color    : COLOR;
    float  round    : TEXCOORD1; // 1 : éclat d'impact
};

// Coins des deux triangles de la bande : (côté, 0 tête / 1 queue).
static const float2 Corners[6] = {
    float2(-1.0, 0.0), float2(1.0, 0.0), float2(-1.0, 1.0),
    float2(-1.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0),
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const TracerData tracer = tracers[vertexId / 6];
    const float2 corner = Corners[vertexId % 6];
    const float3 along = tracer.tail.xyz - tracer.head.xyz;
    const float3 point = lerp(tracer.head.xyz, tracer.tail.xyz, corner.y);
    // Sans longueur (éclat) : n'importe quelle direction perpendiculaire à la vue, la bande devient un carré.
    float3 direction = length(along) > 1.0e-4 ? normalize(along) : cross(point, float3(0.0, 1.0, 0.0));
    if (length(direction) < 1.0e-6) {
        direction = cross(point, float3(1.0, 0.0, 0.0));
    }
    direction = normalize(direction);
    // Au moins un pixel et demi de large : plus fine, une traçante lointaine clignoterait entre les pixels. Élargie,
    // elle s'assombrit d'autant (même lumière en tout) : au loin elle s'estompe au lieu de grossir.
    const float distance = max(length(point), 1.0e-3);
    const float width = max(tracer.head.w, distance * params.options.x * 1.5);
    float3 side = cross(direction, point);
    side = length(side) > 1.0e-6 ? normalize(side) : float3(1.0, 0.0, 0.0);
    // Bouts prolongés d'une demi-largeur : vue de face, la traçante reste un point rond et pas une ligne coupée.
    const float3 position = point + side * (corner.x * 0.5 * width) + direction * ((corner.y * 2.0 - 1.0) * 0.5 * width);
    VertexOutput output;
    output.position = params.viewToClip[0] * position.x + params.viewToClip[1] * position.y
                    + params.viewToClip[2] * position.z + params.viewToClip[3];
    output.uv = corner;
    output.color = tracer.color.rgb * (tracer.head.w / width);
    output.round = tracer.tail.w;
    return output;
}

float4 TracerLight(VertexOutput input)
{
    if (input.round > 0.5) {
        // Éclat : disque doux, sans queue.
        const float2 p = float2(input.uv.x, input.uv.y * 2.0 - 1.0);
        const float disc = saturate(1.0 - dot(p, p));
        return float4(input.color * (disc * disc), 0.0);
    }
    // Cœur vif au milieu, bords doux ; la queue s'éteint.
    const float across = 1.0 - input.uv.x * input.uv.x;
    const float fade = lerp(1.0, 0.15, input.uv.y);
    return float4(input.color * (across * across * fade), 0.0);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return TracerLight(input);
}

// Masque réactif de FSR (TracerRenderer::DrawReactive) : une traçante file plus vite que ce que l'historique peut
// suivre ; là où elle brille, FSR prend l'image du moment au lieu d'en laisser une traînée.
[shader("fragment")]
float4 fragmentReactive(VertexOutput input) : SV_Target
{
    const float3 light = TracerLight(input).rgb;
    return float4(saturate(max(light.r, max(light.g, light.b))) * 0.9, 0.0, 0.0, 0.0);
}
