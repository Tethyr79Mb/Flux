#pragma once

#include "Flux/SimExport.hpp"
#include "Flux/Assets/Model.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    // Matériau multicouche (fichier .fluxmat, JSON) : jusqu'à MaxMaterialLayers matières répétées (peinture, métal nu,
    // saleté) posées dans l'ordre sur un matériau du glTF, chacune là où le dit un canal du masque. Le détail vient
    // des couches répétées, le masque (cuit dans Blender : usure des arêtes, saleté des creux) peut rester petit.
    // Chemins relatifs au dossier des assets.
    struct LayeredMaterialLayer
    {
        std::string name;
        // Images de la matière, telles que les donnent les bibliothèques (Poly Haven, ambientCG) ; assemblées en deux
        // au chargement (ModelMaterialLayer). Vide : aucune (couleur blanche, plat, rugosité et métal pleins).
        std::string colorImage;
        std::string normalImage;    // repère tangent, vert vers le haut (OpenGL) sauf normalDirectX
        std::string roughnessImage; // canal rouge
        std::string metallicImage;  // canal rouge
        std::string heightImage;    // canal rouge ; clair : en relief
        bool normalDirectX = false; // vert vers le bas : retourné à l'assemblage
        // Réglages ; images ignorées. metallic vaut 0 par défaut sans image de métal, 1 avec.
        ModelMaterialLayer settings;
    };

    struct LayeredMaterial
    {
        std::string maskImage; // rouge, vert, bleu, alpha : un masque par couche, dans les UV du modèle
        std::vector<LayeredMaterialLayer> layers;
    };

    FLUX_SIM_API std::expected<LayeredMaterial, std::string> ParseLayeredMaterial(std::string_view text);
    FLUX_SIM_API std::string SerializeLayeredMaterial(const LayeredMaterial& material);

    // Matériaux multicouches d'un modèle, dans un fichier voisin (Raie.glb : Raie.materiaux.json) : nom du matériau du
    // glTF, fichier .fluxmat. Le modèle reste tel qu'exporté de Blender ; un même .fluxmat sert à plusieurs modèles.
    FLUX_SIM_API std::filesystem::path GetMaterialListPath(const std::filesystem::path& model);
    FLUX_SIM_API std::expected<std::vector<std::pair<std::string, std::string>>, std::string>
    ParseMaterialList(std::string_view text);

    // Pose sur `data` les matériaux multicouches de sa liste : images lues et assemblées (ajoutées à data.images),
    // couches et masque dans ModelMaterial. Fichier absent ou illisible : message dans la console, matériau laissé
    // tel quel. `dependencies` reçoit les fichiers lus (et la liste, même absente) : le moteur relit le modèle quand
    // l'un d'eux change. Lent (décodage d'images) : sur le fil de lecture des modèles.
    FLUX_SIM_API void ApplyLayeredMaterials(ModelData& data, const std::filesystem::path& modelPath,
                                            const std::filesystem::path& assetsDirectory,
                                            std::vector<std::filesystem::path>& dependencies);
}
