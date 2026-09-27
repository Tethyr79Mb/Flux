#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Renderer/Image.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace Flux {
    // Sommet d'un modèle, tel qu'envoyé au GPU (doit correspondre à VertexInput dans shaders/Mesh.slang).
    struct ModelVertex
    {
        Math::Vector3f position;
        Math::Vector3f normal; // unitaire
        Math::Vector2f uv;     // (0, 0) en haut à gauche de l'image, comme en glTF
    };

    struct ModelMaterial
    {
        std::string name;
        Math::LinearColor baseColor{1.0f, 1.0f, 1.0f, 1.0f}; // multiplie l'image
        int baseColorImage = -1; // indice dans ModelData::images ; -1 : pas d'image
        // Seuil d'alpha sous lequel le pixel est jeté (feuillages, cheveux) ; 0 : opaque.
        float alphaCutoff = 0.0f;
        bool doubleSided = false; // faces arrière dessinées aussi
    };

    // Suite de triangles d'un même matériau : indices [firstIndex, firstIndex + indexCount) de ModelData::indices.
    struct ModelPrimitive
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t material = 0;
    };

    // Modèle 3D chargé en mémoire, prêt pour le GPU : toute la hiérarchie du fichier est aplatie en un seul
    // maillage (les transformations des nœuds sont appliquées aux sommets), en mètres, y vers le haut.
    struct ModelData
    {
        std::vector<ModelVertex> vertices;
        std::vector<std::uint32_t> indices; // triangles, sens antihoraire vu de l'extérieur
        std::vector<ModelPrimitive> primitives;
        std::vector<ModelMaterial> materials; // au moins un
        std::vector<ImageData> images;
        Math::Box3f bounds; // boîte englobante des sommets
        // Ce qui a été ignoré sans empêcher le chargement (image illisible, primitive en lignes...).
        std::vector<std::string> warnings;
    };

    // glTF 2.0, texte (.gltf avec ses .bin et images à côté) ou binaire (.glb). Lit les maillages en triangles,
    // la couleur de base (facteur et image), l'alpha masqué et les faces doubles ; un personnage avec squelette
    // est figé dans sa pose de repos (ni animation, ni morph targets). Les extensions de compression (Draco,
    // meshopt, KTX2) ne sont pas gérées : le fichier est refusé.
    FLUX_API std::expected<ModelData, std::string> LoadModelFile(const std::filesystem::path& path);

    // Fichiers dont dépend un modèle (.bin, images), relatifs à son dossier, avec des « / » : ce qu'il faut copier
    // avec un .gltf pour qu'il reste lisible ailleurs. Vide pour un .glb autonome.
    FLUX_API std::expected<std::vector<std::string>, std::string> ListModelFiles(const std::filesystem::path& path);
}
