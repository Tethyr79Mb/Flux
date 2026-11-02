// Tests du ciel : directions des faces de la texture cube (convention de Vulkan), panorama, réglages du ciel généré.
#include "Check.hpp"

#include "Flux/Graphics/Sky.hpp"
#include "Flux/Scene/Components.hpp"

#include <cmath>

using namespace Flux;

namespace {
    bool Near(float a, float b, float epsilon = 1.0e-4f) {
        return std::abs(a - b) <= epsilon;
    }

    // Face et coordonnées (s, t de 0 à 1) que le GPU lit pour une direction : le tableau de la spécification Vulkan
    // (« Cube Map Face Selection »).
    void VulkanFace(const Math::Vector3f& d, int& face, float& s, float& t) {
        const float ax = std::abs(d.x);
        const float ay = std::abs(d.y);
        const float az = std::abs(d.z);
        float sc = 0.0f;
        float tc = 0.0f;
        float ma = 0.0f;
        if (ax >= ay && ax >= az) {
            face = d.x > 0.0f ? 0 : 1;
            sc = d.x > 0.0f ? -d.z : d.z;
            tc = -d.y;
            ma = ax;
        } else if (ay >= az) {
            face = d.y > 0.0f ? 2 : 3;
            sc = d.x;
            tc = d.y > 0.0f ? d.z : -d.z;
            ma = ay;
        } else {
            face = d.z > 0.0f ? 4 : 5;
            sc = d.z > 0.0f ? d.x : -d.x;
            tc = -d.y;
            ma = az;
        }
        s = 0.5f * (sc / ma + 1.0f);
        t = 0.5f * (tc / ma + 1.0f);
    }

    bool CubeFacesMatchVulkan() {
        for (int face = 0; face < 6; ++face) {
            for (const float u : {-0.8f, -0.1f, 0.3f, 0.9f}) {
                for (const float v : {-0.7f, 0.2f, 0.6f}) {
                    const Math::Vector3f d = CubeFaceDirection(face, u, v);
                    CHECK(Near(Math::Length(d), 1.0f));
                    int read = -1;
                    float s = 0.0f;
                    float t = 0.0f;
                    VulkanFace(d, read, s, t);
                    CHECK(read == face);
                    CHECK(Near(s, 0.5f * (u + 1.0f)));
                    CHECK(Near(t, 0.5f * (v + 1.0f)));
                }
            }
        }
        return true;
    }

    bool PanoramaMapping() {
        float u = 0.0f;
        float v = 0.0f;
        PanoramaUv({0.0f, 0.0f, -1.0f}, u, v); // devant (caméra par défaut) : le centre de l'image
        CHECK(Near(u, 0.5f) && Near(v, 0.5f));
        PanoramaUv({0.0f, 1.0f, 0.0f}, u, v); // en haut : la première ligne
        CHECK(Near(v, 0.0f));
        PanoramaUv({1.0f, 0.0f, 0.0f}, u, v); // à droite : un quart de tour plus loin
        CHECK(Near(u, 0.75f));
        return true;
    }

    bool GeneratedSky() {
        SkyComponent sky;
        const SkyParameters parameters = MakeSkyParameters(sky);
        CHECK(Near(Math::Length(parameters.galaxyAxis), 1.0f));
        CHECK(Near(Math::Length(parameters.galaxyCenter), 1.0f));
        CHECK(Near(Math::Dot(parameters.galaxyAxis, parameters.galaxyCenter), 0.0f, 1.0e-3f));
        // Même graine : même ciel ; autre graine : autre ciel.
        CHECK(parameters == MakeSkyParameters(sky));
        SkyComponent other = sky;
        other.seed = 2;
        CHECK(parameters != MakeSkyParameters(other));
        return true;
    }

    bool SunDefault() {
        // Les valeurs par défaut du composant redonnent le soleil d'avant le ciel.
        const SkyComponent sky;
        const Math::Vector3f sun = SunDirection(sky.sunElevation, sky.sunAzimuth);
        const Math::Vector3f before = Math::Normalize(Math::Vector3f{0.36f, 0.89f, 0.27f});
        CHECK(Math::Dot(sun, before) > 0.9999f);
        return true;
    }
}

bool RunSkyTests() {
    return CubeFacesMatchVulkan() && PanoramaMapping() && GeneratedSky() && SunDefault();
}
