// Traçantes des projectiles (TracerRenderer) : chaque projectile est un trait lumineux entre sa tête et sa queue,
// une bande tournée vers la caméra. Toutes en un seul appel de dessin : 6 sommets par traçante, sans buffer de
// sommets, les données lues dans un buffer de stockage. Mélange additif : ni tri ni écriture de profondeur.

// Doit correspondre à TracerData dans TracerRenderer.hpp.
struct TracerData
{
    float4 head;  // xyz : tête relative à la caméra (m) ; w : largeur (m)
    float4 tail;  // xyz : queue relative à la caméra (m)
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
    const float3 direction = length(along) > 1.0e-4 ? normalize(along) : float3(0.0, 0.0, 1.0);
    const float3 point = lerp(tracer.head.xyz, tracer.tail.xyz, corner.y);
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
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // Cœur vif au milieu, bords doux ; la queue s'éteint.
    const float across = 1.0 - input.uv.x * input.uv.x;
    const float fade = lerp(1.0, 0.15, input.uv.y);
    return float4(input.color * (across * across * fade), 0.0);
}
