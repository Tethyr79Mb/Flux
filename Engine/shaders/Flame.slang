// Flamme d'un réacteur (ThrusterComponent) : un volume de gaz lumineux, sans particules. On dessine la boîte qui
// l'enferme ; chaque pixel suit le rayon de la caméra dans la boîte (une vingtaine de pas) et additionne la lumière
// du gaz traversé : un cœur chaud, des anneaux de choc (« diamants ») à forte poussée, des bords remués par un bruit
// qui file vers l'arrière. Mélange additif (lumière ajoutée à ce qui est derrière) : ni tri ni écriture de
// profondeur. Le bruit et la forme sont dans un repère normalisé : x et y de -1 à 1 sur la largeur et la hauteur de
// la boîte, z de 0 (la tuyère) à 1 (le bout de la flamme).

// Doit correspondre à FlameParams dans ThrusterRenderer.cpp : 128 octets, une flamme par appel de dessin.
struct FlameParams
{
    float4 boxToClip[4]; // repère normalisé de la boîte -> clip space, par colonnes (comme Math::Matrix4f)
    float4 camera;       // xyz : caméra dans le repère normalisé ; w : temps (s)
    float4 color;        // rgb : couleur x éclat ; w : poussée (0 à 1)
    float4 size;         // xyz : demi-largeur, demi-hauteur et longueur de la boîte (m) ; w : sortie de la tuyère dans la boîte (< 1)
    float4 options;      // x : graine ; y : gain de la lumière du gaz
};
[[vk::push_constant]] ConstantBuffer<FlameParams> flame;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 box      : TEXCOORD; // point de la face dans le repère normalisé
};

// Les 36 sommets des 12 triangles de la boîte, sans buffer : coins de -1 à 1 en x, y et de 0 à 1 en z.
static const uint BoxCorners[36] = {
    0, 2, 1, 1, 2, 3, // -x
    4, 5, 6, 5, 7, 6, // +x
    0, 1, 4, 1, 5, 4, // -y
    2, 6, 3, 3, 6, 7, // +y
    0, 4, 2, 2, 4, 6, // -z
    1, 3, 5, 3, 7, 5, // +z
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const uint corner = BoxCorners[vertexId];
    const float3 box = float3((corner & 4) != 0 ? 1.0 : -1.0, (corner & 2) != 0 ? 1.0 : -1.0, (corner & 1) != 0 ? 1.0 : 0.0);
    VertexOutput output;
    output.position = flame.boxToClip[0] * box.x + flame.boxToClip[1] * box.y + flame.boxToClip[2] * box.z + flame.boxToClip[3];
    output.box = box;
    return output;
}

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float HashCell(int3 c, uint seed)
{
    return float(Hash(uint(c.x) ^ Hash(uint(c.y) ^ Hash(uint(c.z) ^ seed))) >> 8) * (1.0 / 16777216.0);
}

float ValueNoise(float3 p, uint seed)
{
    const float3 cell = floor(p);
    const int3 c = int3(cell);
    const float3 f = p - cell;
    const float3 t = f * f * (3.0 - 2.0 * f);
    const float x00 = lerp(HashCell(c + int3(0, 0, 0), seed), HashCell(c + int3(1, 0, 0), seed), t.x);
    const float x10 = lerp(HashCell(c + int3(0, 1, 0), seed), HashCell(c + int3(1, 1, 0), seed), t.x);
    const float x01 = lerp(HashCell(c + int3(0, 0, 1), seed), HashCell(c + int3(1, 0, 1), seed), t.x);
    const float x11 = lerp(HashCell(c + int3(0, 1, 1), seed), HashCell(c + int3(1, 1, 1), seed), t.x);
    return lerp(lerp(x00, x10, t.y), lerp(x01, x11, t.y), t.z);
}

// Lumière émise par mètre au point p du repère normalisé (avant la couleur) : x cœur blanc, y gaz coloré.
float2 Emission(float3 p, float throttle, float time, uint seed)
{
    const float t = p.z;
    const float nozzle = flame.size.w;
    // Section : la forme de la tuyère (rectangle aux coins arrondis) qui s'arrondit et s'amincit vers le bout.
    const float radius = nozzle * lerp(1.0, 0.6, t) * (1.0 - 0.12 * sin(3.14159 * min(t * 3.0, 1.0)));
    const float exponent = lerp(6.0, 2.0, smoothstep(0.0, 0.5, t));
    const float2 a = abs(p.xy) / radius;
    float d = pow(pow(a.x, exponent) + pow(a.y, exponent), 1.0 / exponent);

    // Turbulence en mètres, qui file vers l'arrière d'autant plus vite que la poussée est forte ; plus forte vers le bout.
    const float3 meters = float3(p.xy * flame.size.xy, t * flame.size.z);
    const float3 flow = float3(0.0, 0.0, time * lerp(6.0, 40.0, throttle));
    const float noise = ValueNoise((meters - flow) * float3(2.0, 2.0, 0.5), seed) * 0.65
                      + ValueNoise((meters - flow * 1.7) * float3(6.0, 6.0, 2.0), seed + 7) * 0.35;
    d += (noise - 0.5) * lerp(0.3, 1.2, t);

    // Le long de la flamme : nulle contre la tuyère (sa face), vive juste derrière, éteinte au bout ; nulle aussi
    // contre les côtés de la boîte (sinon on verrait la flamme coupée net quand le bruit la pousse jusque-là).
    const float sides = 1.0 - smoothstep(0.7, 1.0, max(abs(p.x), abs(p.y)));
    const float along = pow(saturate(1.0 - t), 1.2) * smoothstep(0.0, 0.03, t) * sides;
    // Gaz déchiré par le bruit : des volutes plus ou moins denses.
    const float gas = exp(-d * d * 1.6) * along * (0.25 + 1.5 * noise * noise);
    // Anneaux de choc : des renflements lumineux dans le cœur, plus serrés et plus vifs à forte poussée.
    const float rings = lerp(3.0, 6.0, throttle);
    const float diamond = pow(0.5 + 0.5 * cos(t * rings * 6.28318), 6.0) * smoothstep(0.4, 1.0, throttle) * (1.0 - t);
    const float core = exp(-d * d * 9.0) * along * (0.6 + 2.5 * diamond);
    return float2(core, gas);
}

// La flamme était réglée pour l'image finale (1 - exp(-x) : de 0 au blanc) ; l'image de la scène est en lumière
// linéaire, avant la courbe de tons de Post.slang. On écrit la lumière qui, après cette courbe, redonne la même
// couleur sur un fond noir ; sur un fond clair, elle s'y ajoute en lumière, comme le reste.
float3 InverseToneMap(float3 c)
{
    c = min(c, float3(0.999));
    const float3 a = 2.51 - 2.43 * c;
    const float3 b = 0.03 - 0.59 * c;
    return (-b + sqrt(b * b + 4.0 * a * 0.14 * c)) / (2.0 * a) / 0.6;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // Rayon de la caméra à travers la boîte (repère normalisé : une transformation affine garde les droites).
    const float3 origin = flame.camera.xyz;
    const float3 direction = input.box - origin;
    const float3 lower = float3(-1.0, -1.0, 0.0);
    const float3 upper = float3(1.0, 1.0, 1.0);
    const float3 inverse = 1.0 / direction;
    const float3 t0 = (lower - origin) * inverse;
    const float3 t1 = (upper - origin) * inverse;
    const float3 near = min(t0, t1);
    const float3 far = max(t0, t1);
    const float enter = max(max(near.x, near.y), near.z);
    const float exit = min(min(far.x, far.y), far.z);
    // Le pixel est sur la face où le rayon entre (1 : ce point) ; caméra dans la boîte : sur la face de sortie. Les
    // autres faces de la boîte (derrière) ne comptent pas, sinon la flamme serait ajoutée deux fois.
    if (enter > 0.0 ? abs(enter - 1.0) > 0.01 : abs(exit - 1.0) > 0.01) {
        discard;
    }
    const float start = max(enter, 0.0);
    if (exit <= start) {
        discard;
    }

    const uint seed = asuint(flame.options.x);
    const float throttle = flame.color.w;
    const float time = flame.camera.w;
    // Pas en mètres : la longueur du segment parcouru, avec la taille de la boîte.
    constexpr int Steps = 20;
    const float stepLength = (exit - start) / Steps;
    const float stepMeters = length(direction * flame.size.xyz) * stepLength;
    // Départ décalé d'un pixel à l'autre : pas de bandes entre les pas, un grain fin à la place.
    const uint2 pixel = uint2(input.position.xy);
    const float jitter = float(Hash(pixel.x * 1973u + pixel.y * 9277u + seed) >> 8) * (1.0 / 16777216.0);
    float2 sum = float2(0.0);
    for (int i = 0; i < Steps; ++i) {
        const float3 p = origin + direction * (start + (i + jitter) * stepLength);
        sum += Emission(p, throttle, time, seed);
    }
    sum *= stepMeters;

    // Gaz de la couleur choisie ; cœur qui tire vers le blanc. La courbe 1 - exp(-x) sature doucement : le
    // bleu plafonne en premier, puis le vert et le rouge, d'où un cœur blanc entouré de bleu.
    const float3 light = (flame.color.rgb * (0.45 * sum.y + 0.8 * sum.x) + float3(0.75, 0.85, 1.0) * sum.x * sum.x * 0.6) * flame.options.y;
    return float4(InverseToneMap(1.0 - exp(-light)), 0.0);
}
