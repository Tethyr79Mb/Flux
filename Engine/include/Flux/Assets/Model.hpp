#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Math/Vector3.hpp"

#include <array>
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

    // Os qui déforment un sommet d'un modèle animé, envoyés au GPU à côté de ModelVertex (SkinInput dans
    // shaders/Mesh.slang).
    struct ModelSkinVertex
    {
        std::array<std::uint16_t, 4> joints{}; // indices dans ModelSkeleton::joints
        std::array<float, 4> weights{};        // somme 1
    };

    // Nœud de la hiérarchie du fichier, dans sa pose de repos : transformation locale, relative à son parent.
    struct SkeletonNode
    {
        std::string name;
        int parent = -1; // indice dans ModelSkeleton::nodes, toujours inférieur à celui du nœud ; -1 : racine
        Math::Vector3f translation;
        Math::Quaternionf rotation;
        Math::Vector3f scale{1.0f, 1.0f, 1.0f};
    };

    // Os qui déforme des sommets : son nœud, et l'inverse de sa matrice de liaison (du repère du maillage à celui de
    // l'os dans la pose où le maillage a été attaché au squelette).
    struct SkeletonJoint
    {
        std::uint32_t node = 0;
        Math::Matrix4f inverseBind = Math::Matrix4f::Identity();
    };

    enum class AnimationPath : std::uint8_t { Translation, Rotation, Scale };

    enum class AnimationInterpolation : std::uint8_t {
        Step,        // saut d'une clé à la suivante
        Linear,      // rotations en slerp
        CubicSpline, // courbe de Hermite, tangentes données par le fichier
    };

    // Courbe d'une propriété d'un nœud : valeurs aux instants times (secondes, croissants). Une valeur fait 4 floats
    // pour une rotation (x, y, z, w), 3 sinon ; CubicSpline en donne 3 par instant (tangente d'entrée, valeur,
    // tangente de sortie), comme glTF.
    struct AnimationChannel
    {
        std::uint32_t node = 0; // indice dans ModelSkeleton::nodes
        AnimationPath path = AnimationPath::Translation;
        AnimationInterpolation interpolation = AnimationInterpolation::Linear;
        std::vector<float> times;
        std::vector<float> values;
    };

    struct ModelAnimation
    {
        std::string name;
        float duration = 0.0f; // secondes : le dernier instant de ses courbes
        std::vector<AnimationChannel> channels;
    };

    // Hiérarchie, os et animations d'un modèle animé ; vide (aucun nœud) pour un modèle statique.
    struct ModelSkeleton
    {
        std::vector<SkeletonNode> nodes; // parents avant enfants
        std::vector<SkeletonJoint> joints;
        std::vector<ModelAnimation> animations;
    };

    // Modèle 3D chargé en mémoire, prêt pour le GPU, en mètres, y vers le haut.
    // Statique (sans squelette ni animation) : toute la hiérarchie du fichier est aplatie en un seul maillage, les
    // transformations des nœuds déjà appliquées aux sommets. Animé : les sommets restent dans le repère de leur
    // maillage et chacun suit jusqu'à 4 os (skinVertices) ; une pièce rigide suit son nœud comme un os de poids 1.
    struct ModelData
    {
        std::vector<ModelVertex> vertices;
        std::vector<std::uint32_t> indices; // triangles, sens antihoraire vu de l'extérieur
        std::vector<ModelPrimitive> primitives;
        std::vector<ModelMaterial> materials; // au moins un
        std::vector<ImageData> images;
        Math::Box3f bounds; // boîte englobante des sommets (pose de repos pour un modèle animé)
        std::vector<ModelSkinVertex> skinVertices; // un par sommet si le modèle est animé, sinon vide
        ModelSkeleton skeleton;
        // Ce qui a été ignoré sans empêcher le chargement (image illisible, primitive en lignes...).
        std::vector<std::string> warnings;

        bool IsAnimated() const { return !skinVertices.empty(); }
    };

    // glTF 2.0, texte (.gltf avec ses .bin et images à côté) ou binaire (.glb). Lit les maillages en triangles,
    // la couleur de base (facteur et image), l'alpha masqué et les faces doubles, les squelettes et les animations
    // (pas les morph targets). Les extensions de compression (Draco,
    // meshopt, KTX2) ne sont pas gérées : le fichier est refusé.
    FLUX_API std::expected<ModelData, std::string> LoadModelFile(const std::filesystem::path& path);

    // Fichiers dont dépend un modèle (.bin, images), relatifs à son dossier, avec des « / » : ce qu'il faut copier
    // avec un .gltf pour qu'il reste lisible ailleurs. Vide pour un .glb autonome.
    FLUX_API std::expected<std::vector<std::string>, std::string> ListModelFiles(const std::filesystem::path& path);
}
