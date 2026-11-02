// Directions des textures cube et des panoramas, partagées par le fond du ciel (Sky.slang) et l'environnement
// (Environment.slang).

// Point (u, v de -1 à 1, au centre des texels) d'une face de texture cube -> direction (l'ordre et l'orientation
// des faces de Vulkan, comme CubeFaceDirection dans Sky.cpp).
float3 CubeFaceDirection(uint face, float u, float v)
{
    float3 d;
    switch (face) {
        case 0: d = float3(1.0, -v, -u); break;
        case 1: d = float3(-1.0, -v, u); break;
        case 2: d = float3(u, 1.0, v); break;
        case 3: d = float3(u, -1.0, -v); break;
        case 4: d = float3(u, -v, 1.0); break;
        default: d = float3(-u, -v, -1.0); break;
    }
    return normalize(d);
}

// Point du panorama vu dans `direction` (comme PanoramaUv dans Sky.cpp).
float2 PanoramaUv(float3 direction)
{
    const float Pi = 3.14159265;
    return float2(atan2(direction.x, -direction.z) / (2.0 * Pi) + 0.5, acos(clamp(direction.y, -1.0, 1.0)) / Pi);
}
