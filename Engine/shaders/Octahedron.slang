// Normales rangées en deux nombres (G-buffer) : écrites par Mesh.slang, lues par Lighting.slang et
// AmbientOcclusion.slang.

// Direction unitaire en deux nombres de 0 à 1 (octaèdre déplié, Cigolle et al. 2014) : 16 bits chacun gardent
// l'angle à mieux qu'un centième de degré.
float2 EncodeOctahedron(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 side = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    const float2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * side;
    return e * 0.5 + 0.5;
}

float3 DecodeOctahedron(float2 encoded)
{
    const float2 e = encoded * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float fold = saturate(-n.z);
    n.x += n.x >= 0.0 ? -fold : fold;
    n.y += n.y >= 0.0 ? -fold : fold;
    return normalize(n);
}
