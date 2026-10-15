#pragma once

#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>

namespace Flux {
    // Ce que l'anticrénelage temporel (PostRenderer::ResolveTemporal) demande aux passes de la scène pour une image.
    struct TemporalView
    {
        // Décalage de la projection en clip space (2 / taille de la vue fois une fraction de pixel) : chaque image voit
        // la scène un peu ailleurs dans ses pixels, et leur moyenne lisse les bords. Nul sans anticrénelage.
        Math::Vector2f jitter;
        // Vue et projection de l'image précédente, sans décalage, relatives à sa caméra.
        Math::Matrix4f previousViewProjection;
        // Position de la caméra moins celle de l'image précédente : un point relatif à la caméra plus ce vecteur donne
        // le même point relatif à l'ancienne caméra.
        Math::Vector3f cameraDelta;
        // Numéro de l'image pour les bruits qui doivent changer d'une image à l'autre (nuages) ; 0 sans anticrénelage
        // (un bruit fixe ne scintille pas).
        std::uint32_t noiseFrame = 0;
        // false : rien à reprendre de l'image précédente (premier affichage, saut de la caméra).
        bool history = false;
    };

    // `viewProjection` décalé de `jitter` : x et y du clip space reçoivent jitter x w, soit jitter après la division.
    inline Math::Matrix4f Jittered(Math::Matrix4f viewProjection, Math::Vector2f jitter) {
        for (auto& column : viewProjection.columns) {
            column[0] += jitter.x * column[3];
            column[1] += jitter.y * column[3];
        }
        return viewProjection;
    }
}
