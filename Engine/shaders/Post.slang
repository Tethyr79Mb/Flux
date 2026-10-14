// Après la scène : bloom puis courbe de tons, vers l'image de la fenêtre (PostRenderer.cpp).
// La scène est dessinée en HDR (RGBA16Float, lumière linéaire sans plafond) ; le bloom la réduit de moitié en
// moitié (fragmentDownsample), puis remonte en additionnant chaque niveau flou au niveau au-dessus
// (fragmentUpsample) : la somme d'une demi-douzaine de flous de plus en plus larges. Les zones très lumineuses
// (soleil, flammes, émissifs) débordent ainsi autour d'elles, d'autant plus qu'elles sont fortes ; aucun seuil, donc
// pas de bord net entre ce qui brille et ce qui ne brille pas. Méthode de Jimenez (Call of Duty, SIGGRAPH 2014).

// Doit correspondre à PostParams dans PostRenderer.cpp.
struct PostParams
{
    float4 texel;   // xy : taille d'un texel de la source (1 / largeur, 1 / hauteur) ; z : rayon du flou de remontée
    float4 options; // x : force du bloom (0 : aucun) ; y : exposition ; z : 1 / nombre de niveaux du bloom
};
[[vk::push_constant]] ConstantBuffer<PostParams> post;

[[vk::binding(0, 0)]] Sampler2D source;
[[vk::binding(1, 0)]] Sampler2D other; // remontée : le niveau réduit de même taille ; image finale : le bloom

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD; // 0 à 1, origine en haut à gauche comme les textures
};

// Un triangle qui couvre toute la zone (viewport) : la scène dans la fenêtre, ou toute la texture du niveau.
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VulkanVertexID)
{
    const float2 clip = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;
    VertexOutput output;
    output.position = float4(clip, 0.0, 1.0);
    output.uv = clip * 0.5 + 0.5;
    return output;
}

// Un pixel infini ou NaN (division par zéro dans un shader de la scène) s'étalerait sur tout l'écran par le flou : il
// est ramené à une valeur finie dès la première réduction.
float3 Finite(float3 color)
{
    return any(isnan(color)) ? float3(0.0) : min(color, float3(60000.0));
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Réduction de moitié sur 13 lectures (une boîte centrale de 4 et quatre boîtes en coin) : moins de crénelage qu'une
// simple moyenne de 4. first : depuis l'image de la scène ; chaque boîte y est pondérée par 1 / (1 + luminance)
// (moyenne de Karis), sinon un pixel isolé très brillant (reflet du soleil) clignoterait en gros carré en bougeant.
float3 Downsample(float2 uv, bool first)
{
    const float2 t = post.texel.xy;
    const float3 a = Finite(source.SampleLevel(uv + t * float2(-2.0, -2.0), 0.0).rgb);
    const float3 b = Finite(source.SampleLevel(uv + t * float2(0.0, -2.0), 0.0).rgb);
    const float3 c = Finite(source.SampleLevel(uv + t * float2(2.0, -2.0), 0.0).rgb);
    const float3 d = Finite(source.SampleLevel(uv + t * float2(-2.0, 0.0), 0.0).rgb);
    const float3 e = Finite(source.SampleLevel(uv, 0.0).rgb);
    const float3 f = Finite(source.SampleLevel(uv + t * float2(2.0, 0.0), 0.0).rgb);
    const float3 g = Finite(source.SampleLevel(uv + t * float2(-2.0, 2.0), 0.0).rgb);
    const float3 h = Finite(source.SampleLevel(uv + t * float2(0.0, 2.0), 0.0).rgb);
    const float3 i = Finite(source.SampleLevel(uv + t * float2(2.0, 2.0), 0.0).rgb);
    const float3 j = Finite(source.SampleLevel(uv + t * float2(-1.0, -1.0), 0.0).rgb);
    const float3 k = Finite(source.SampleLevel(uv + t * float2(1.0, -1.0), 0.0).rgb);
    const float3 l = Finite(source.SampleLevel(uv + t * float2(-1.0, 1.0), 0.0).rgb);
    const float3 m = Finite(source.SampleLevel(uv + t * float2(1.0, 1.0), 0.0).rgb);
    const float3 center = (j + k + l + m) * 0.25;
    const float3 topLeft = (a + b + d + e) * 0.25;
    const float3 topRight = (b + c + e + f) * 0.25;
    const float3 bottomLeft = (d + e + g + h) * 0.25;
    const float3 bottomRight = (e + f + h + i) * 0.25;
    if (!first) {
        return center * 0.5 + (topLeft + topRight + bottomLeft + bottomRight) * 0.125;
    }
    const float wc = 0.5 / (1.0 + Luminance(center));
    const float w0 = 0.125 / (1.0 + Luminance(topLeft));
    const float w1 = 0.125 / (1.0 + Luminance(topRight));
    const float w2 = 0.125 / (1.0 + Luminance(bottomLeft));
    const float w3 = 0.125 / (1.0 + Luminance(bottomRight));
    return (center * wc + topLeft * w0 + topRight * w1 + bottomLeft * w2 + bottomRight * w3) / (wc + w0 + w1 + w2 + w3);
}

[shader("fragment")]
float4 fragmentDownsampleFirst(VertexOutput input) : SV_Target
{
    return float4(Downsample(input.uv, true), 1.0);
}

[shader("fragment")]
float4 fragmentDownsample(VertexOutput input) : SV_Target
{
    return float4(Downsample(input.uv, false), 1.0);
}

// Remontée : le niveau plus petit (source), agrandi par un filtre en tente 3 x 3, ajouté au niveau réduit de même
// taille (other).
[shader("fragment")]
float4 fragmentUpsample(VertexOutput input) : SV_Target
{
    const float2 t = post.texel.xy * post.texel.z;
    const float2 uv = input.uv;
    float3 sum = source.SampleLevel(uv, 0.0).rgb * 4.0;
    sum += (source.SampleLevel(uv + float2(-t.x, 0.0), 0.0).rgb + source.SampleLevel(uv + float2(t.x, 0.0), 0.0).rgb
            + source.SampleLevel(uv + float2(0.0, -t.y), 0.0).rgb + source.SampleLevel(uv + float2(0.0, t.y), 0.0).rgb) * 2.0;
    sum += source.SampleLevel(uv + float2(-t.x, -t.y), 0.0).rgb + source.SampleLevel(uv + float2(t.x, -t.y), 0.0).rgb
         + source.SampleLevel(uv + float2(-t.x, t.y), 0.0).rgb + source.SampleLevel(uv + float2(t.x, t.y), 0.0).rgb;
    return float4(other.SampleLevel(uv, 0.0).rgb + sum * (1.0 / 16.0), 1.0);
}

// Courbe de tons ACES (approximation de Narkowicz) : les couleurs trop fortes sont adoucies vers le blanc au lieu
// d'être coupées net. InverseToneMap dans PostRenderer.cpp en est l'inverse (couleur de fond sans ciel).
float3 ToneMap(float3 color, float exposure)
{
    color *= exposure * 0.6;
    return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

// Image finale : la scène et son bloom (other : la somme des niveaux, ramenée à une moyenne), puis la courbe de tons. Le bloom
// prend une petite part de la lumière de chaque pixel pour l'étaler : l'image ne s'éclaircit pas dans l'ensemble.
[shader("fragment")]
float4 fragmentComposite(VertexOutput input) : SV_Target
{
    float3 color = Finite(source.SampleLevel(input.uv, 0.0).rgb);
    if (post.options.x > 0.0) {
        color = lerp(color, other.SampleLevel(input.uv, 0.0).rgb * post.options.z, post.options.x);
    }
    return float4(ToneMap(color, post.options.y), 1.0);
}
