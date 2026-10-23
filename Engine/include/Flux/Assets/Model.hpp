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
        // Lumière propre (écrans, voyants), ajoutée à l'éclairage : facteur x image (KHR_materials_emissive_strength
        // compris). Noir : aucune.
        Math::LinearColor emissive{0.0f, 0.0f, 0.0f, 1.0f};
        int emissiveImage = -1;
        // Métal et rugosité (0 à 1), multipliés par l'image (canal bleu : métal, vert : rugosité) si elle existe.
        // Valeurs par défaut de glTF : un matériau sans ces facteurs est un métal rugueux.
        float metallic = 1.0f;
        float roughness = 1.0f;
        int metallicRoughnessImage = -1;
        // Relief : carte de normales (repère tangent, comme glTF), creusée ou accentuée par normalScale.
        int normalImage = -1;
        float normalScale = 1.0f;
        // Lumière ambiante bloquée par les creux (canal rouge de l'image) ; 0 : aucun effet, 1 : plein effet.
        int occlusionImage = -1;
        float occlusionStrength = 1.0f;
        // Seuil d'alpha sous lequel le pixel est jeté (feuillages, cheveux) ; 0 : opaque.
        float alphaCutoff = 0.0f;
        bool doubleSided = false; // faces arrière dessinées aussi
        // Transparent (alphaMode BLEND) : mélangé à ce qui est derrière selon l'alpha, dessiné après les opaques.
        bool blend = false;
    };

    // Suite de triangles d'un même matériau : indices [firstIndex, firstIndex + indexCount) de ModelData::indices.
    struct ModelPrimitive
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t material = 0;
        std::uint32_t cluster = 0; // bloc d'un grand modèle (ModelData::clusters) ; 0 sinon
        // Ses paquets de triangles (ModelData::meshlets), qui couvrent exactement ses indices ; aucun : pas découpée.
        std::uint32_t firstMeshlet = 0;
        std::uint32_t meshletCount = 0;
    };

    // Paquet de triangles voisins d'une primitive (meshlet), l'unité que le GPU écarte seule (hors du champ, caché,
    // tourné vers l'arrière) et que dessine un groupe de fils du mesh shader. Ses triangles sont décrits deux fois :
    // indices globaux (chemin par les sommets) et indices locaux sur 8 bits vers ses sommets (mesh shaders).
    inline constexpr std::uint32_t ModelMeshletMaxVertices = 64;
    inline constexpr std::uint32_t ModelMeshletMaxTriangles = 124;
    struct ModelMeshlet
    {
        // Sphère qui contient ses triangles.
        Math::Vector3f center;
        float radius = 0.0f;
        // Cône des normales : tout le paquet tourne le dos à une caméra en c si
        // dot(center - c, coneAxis) >= coneCutoff * |center - c| + radius. coneCutoff > 1 : jamais (faces doubles).
        Math::Vector3f coneAxis;
        float coneCutoff = 2.0f;
        std::uint32_t firstIndex = 0;    // ses triangles dans ModelData::indices, à la suite
        std::uint32_t firstVertex = 0;   // ses sommets dans ModelData::meshletVertices
        std::uint32_t firstTriangle = 0; // ses triangles dans ModelData::meshletTriangles (octets, multiple de 4)
        std::uint16_t vertexCount = 0;
        std::uint16_t triangleCount = 0;
        std::uint32_t primitive = 0; // dans ModelData::primitives (son matériau, son bloc)
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
        // Préparés pour le rendu (PrepareModel). Ombres : les primitives opaques qui se suivent et n'ont pas besoin de
        // leur matériau (sans seuil d'alpha, même sorte de faces, même bloc) réunies en une ; material : la première.
        std::vector<ModelPrimitive> shadowPrimitives;
        // Blocs d'un grand modèle (station), chacun avec la boîte de ses triangles : écartés un par un hors du champ
        // et des ombres. Vide : un seul bloc, la boîte du modèle.
        std::vector<Math::Box3f> clusters;
        // Paquets de triangles d'un modèle fixe (vides pour un modèle animé : ses sommets bougent, les sphères et cônes
        // ne tiendraient pas), primitive par primitive. meshletVertices : indices dans vertices ; meshletTriangles :
        // trois octets par triangle vers les sommets du paquet, chaque paquet complété à un multiple de 4 octets.
        std::vector<ModelMeshlet> meshlets;
        std::vector<std::uint32_t> meshletVertices;
        std::vector<std::uint8_t> meshletTriangles;
        // Ce qui a été ignoré sans empêcher le chargement (image illisible, primitive en lignes...).
        std::vector<std::string> warnings;

        bool IsAnimated() const { return !skinVertices.empty(); }
    };

    // glTF 2.0, texte (.gltf avec ses .bin et images à côté) ou binaire (.glb). Lit les maillages en triangles,
    // la couleur de base (facteur et image), métal et rugosité, les cartes de normales et d'occlusion, l'émissif
    // (facteur, image, intensité), l'alpha masqué et les faces doubles, les squelettes et les animations (pas les
    // morph targets). Les extensions de compression (Draco,
    // meshopt, KTX2) ne sont pas gérées : le fichier est refusé. images = false : les images ne sont pas décodées
    // (ModelData::images vide, indices d'image à -1) ni les paquets construits, pour qui n'a besoin que des triangles
    // (collisions).
    FLUX_API std::expected<ModelData, std::string> LoadModelFile(const std::filesystem::path& path, bool images = true);

    // Rangement pour le rendu, appelé par LoadModelFile : le modèle se dessine en peu d'appels quelle que soit la façon
    // dont il a été modélisé. Les matériaux identiques (tout sauf le nom) n'en font plus qu'un, les triangles d'un même
    // matériau ne forment plus qu'une primitive, et shadowPrimitives est rempli. Un modèle fixe dont un côté dépasse
    // 2 x ModelClusterSize est en plus découpé en blocs de ModelClusterSize (selon le centre de chaque triangle). Enfin
    // chaque primitive d'un modèle fixe est découpée en paquets (meshlets), ses indices rangés paquet par paquet, sauf
    // si meshlets = false.
    inline constexpr float ModelClusterSize = 16.0f;
    FLUX_API void PrepareModel(ModelData& model, bool meshlets = true);

    // Fichiers dont dépend un modèle (.bin, images), relatifs à son dossier, avec des « / » : ce qu'il faut copier
    // avec un .gltf pour qu'il reste lisible ailleurs. Vide pour un .glb autonome.
    FLUX_API std::expected<std::vector<std::string>, std::string> ListModelFiles(const std::filesystem::path& path);
}
