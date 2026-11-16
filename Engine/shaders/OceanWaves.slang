// Vagues de l'océan (OceanWavesGpu.cpp) : à chaque image, le spectre de départ (Planetology::BuildOceanSpectrum, calculé
// par le CPU) avance dans le temps, puis une FFT inverse en fait la mer de chaque cascade : déplacement des points de
// l'eau (x, hauteur, z), pentes et écume, dans deux textures par cascade qui se répètent sans couture.
// Huit valeurs réelles par onde, rangées deux par deux dans quatre champs complexes (la FFT inverse d'un spectre
// hermitien est réelle : celle de A + iB rend a en partie réelle, b en partie imaginaire). FFT de Stockham en base 2
// dans la mémoire partagée, une ligne (ou une colonne) par groupe : pas de permutation des indices.
// Coût par cascade de 256² : 2 x 4 x 256 FFT de 256 points, environ 0,1 ms sur une carte de la configuration minimum.

static const uint Size = 256; // Planetology::OceanWaveSize
static const uint Fields = 4;
static const float Pi = 3.14159265;
static const float Gravity = 9.81; // Planetology::OceanGravity
static const float RepeatPeriod = 200.0; // s, Planetology::OceanRepeatPeriod

// Doit correspondre à Constants dans OceanWavesGpu.cpp.
struct Constants
{
    uint cascade;
    uint columns;      // FFT : 0 les lignes (x), 1 les colonnes (z)
    float time;        // s, de 0 à RepeatPeriod
    float length;      // m : côté de la cascade
    float choppiness;  // déplacement horizontal (OceanWaveSettings::choppiness)
    float foamKeep;    // part de l'écume de l'image d'avant gardée (elle s'efface en quelques secondes)
    float foamStart;   // jacobien sous lequel l'eau mousse (1 : surface au repos, 0 : crête repliée sur elle-même)
    float foamFull;
};

[[vk::binding(0, 0)]] StructuredBuffer<float4> spectrum;  // cascades x Size², h0(k) puis conj(h0(-k))
[[vk::binding(1, 0)]] RWStructuredBuffer<float2> fields;  // cascades x Fields x Size², rangés par lignes (z) de x
// x, hauteur, z (m) ; a : écume (0 à 1)
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> displacement;
// d hauteur / dx, d hauteur / dz, d x / dx, d z / dz : la normale et le jacobien du déplacement
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> slopes;
[[vk::push_constant]] ConstantBuffer<Constants> constants;

float2 ComplexMultiply(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float2 TimesI(float2 a)
{
    return float2(-a.y, a.x);
}

uint FieldIndex(uint field, uint row, uint column)
{
    return ((constants.cascade * Fields + field) * Size + row) * Size + column;
}

// Le spectre au temps `time` de chaque onde, et les sept autres valeurs qui s'en déduisent (Tessendorf) : déplacement
// horizontal -i k/|k| h, pentes i k h, dérivées du déplacement k k/|k| h. Le déplacement horizontal pousse l'eau vers
// les crêtes : elles s'affinent, les creux s'élargissent.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeSpectrum(uint3 id : SV_DispatchThreadID)
{
    const uint m = id.x;
    const uint n = id.y;
    const float step = 2.0 * Pi / constants.length;
    const float2 k = (float2(float(m), float(n)) - float(Size / 2)) * step;
    const float magnitude = length(k);
    // Vitesses arrondies à un multiple de 2 pi / RepeatPeriod : la mer revient la même après RepeatPeriod secondes, le
    // temps peut repartir de 0 sans saut et la phase reste précise en float (des heures de jeu la noieraient).
    const float base = 2.0 * Pi / RepeatPeriod;
    const float omega = floor(sqrt(Gravity * magnitude) / base) * base;
    const float phase = omega * constants.time;
    float2 turn;
    sincos(phase, turn.y, turn.x);
    const float4 start = spectrum[constants.cascade * Size * Size + n * Size + m];
    const float2 h = ComplexMultiply(start.xy, float2(turn.x, -turn.y)) + ComplexMultiply(start.zw, turn);
    const float2 ih = TimesI(h);
    const float inverse = magnitude > 0.0 ? 1.0 / magnitude : 0.0;
    const float2 dx = -k.x * inverse * ih;
    const float2 dz = -k.y * inverse * ih;
    const float2 heightDx = k.x * ih;
    const float2 heightDz = k.y * ih;
    const float2 dxDx = k.x * k.x * inverse * h;
    const float2 dzDz = k.y * k.y * inverse * h;
    const float2 dxDz = k.x * k.y * inverse * h;
    fields[FieldIndex(0, n, m)] = dx + TimesI(h);
    fields[FieldIndex(1, n, m)] = dz + TimesI(dxDz);
    fields[FieldIndex(2, n, m)] = heightDx + TimesI(heightDz);
    fields[FieldIndex(3, n, m)] = dxDx + TimesI(dzDz);
}

groupshared float2 lineData[2][Size];

// FFT inverse (somme des e^(+i...)) d'une ligne ou d'une colonne d'un champ, en place. Groupe : Fields x Size lignes.
[shader("compute")]
[numthreads(Size / 2, 1, 1)]
void computeFft(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint field = group.x / Size;
    const uint line = group.x % Size;
    const uint j = thread.x;
    const uint first = constants.columns != 0 ? FieldIndex(field, j, line) : FieldIndex(field, line, j);
    const uint second = constants.columns != 0 ? FieldIndex(field, j + Size / 2, line) : FieldIndex(field, line, j + Size / 2);
    lineData[0][j] = fields[first];
    lineData[0][j + Size / 2] = fields[second];
    GroupMemoryBarrierWithGroupSync();
    uint source = 0;
    // Stockham (Govindaraju et al. 2008) : à chaque étape, des FFT de 2 x span points faites de deux de span points.
    for (uint span = 1; span < Size; span *= 2) {
        const uint position = j % span;
        float2 twiddle;
        sincos(Pi * float(position) / float(span), twiddle.y, twiddle.x);
        const float2 a = lineData[source][j];
        const float2 b = ComplexMultiply(lineData[source][j + Size / 2], twiddle);
        const uint target = (j / span) * span * 2 + position;
        lineData[1 - source][target] = a + b;
        lineData[1 - source][target + span] = a - b;
        GroupMemoryBarrierWithGroupSync();
        source = 1 - source;
    }
    fields[first] = lineData[source][j];
    fields[second] = lineData[source][j + Size / 2];
}

// Les champs rangés dans les textures de la cascade. Ondes numérotées de -N/2 à N/2 - 1 : la FFT les a prises de 0 à
// N - 1, d'où le signe (-1)^(x + z). L'écume naît où la surface se resserre (jacobien du déplacement petit : crête
// qui se replie) et s'efface ensuite lentement.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeAssemble(uint3 id : SV_DispatchThreadID)
{
    const uint x = id.x;
    const uint z = id.y;
    const float sign = ((x + z) & 1) != 0 ? -1.0 : 1.0;
    const float2 a = fields[FieldIndex(0, z, x)] * sign;
    const float2 b = fields[FieldIndex(1, z, x)] * sign;
    const float2 c = fields[FieldIndex(2, z, x)] * sign;
    const float2 d = fields[FieldIndex(3, z, x)] * sign;
    const float lambda = constants.choppiness;
    const float dxDx = lambda * d.x;
    const float dzDz = lambda * d.y;
    const float dxDz = lambda * b.y;
    const float jacobian = (1.0 + dxDx) * (1.0 + dzDz) - dxDz * dxDz;
    const float coverage = saturate((constants.foamStart - jacobian) / (constants.foamStart - constants.foamFull));
    // Le maximum plutôt qu'une somme : la même écume quel que soit le nombre d'images par seconde.
    // foamKeep à 0 : première image, la texture n'a encore rien de lisible.
    const float previous = constants.foamKeep > 0.0 ? displacement[id.xy].w : 0.0;
    const float foam = max(previous * constants.foamKeep, coverage);
    displacement[id.xy] = float4(lambda * a.x, a.y, lambda * b.x, foam);
    slopes[id.xy] = float4(c.x, c.y, dxDx, dzDz);
}
