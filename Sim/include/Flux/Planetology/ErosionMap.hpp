#pragma once

#include "Flux/SimExport.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

// Carte d'érosion d'une planète : de vraies vallées, creusées par l'eau qui coule, que le bruit seul ne sait pas
// faire (une vallée dépend de tout l'amont). Calculée une fois depuis la graine, à l'échelle de toute la planète (une
// maille d'environ 1 km), puis gardée en cache ; le relief en formule garde le détail fin par-dessus.
// Le calcul est sur le CPU et déterministe (même carte sur le client et le serveur, quel que soit le nombre de fils) :
// seulement des + - * / et des racines, dans un ordre fixe ; les hauteurs de départ (Terrain::GetHeight, qui passe
// par exp, pow, sin) sont arrondies au 1/16 de mètre pour effacer l'écart d'une bibliothèque mathématique à l'autre.
// Les points de la carte sont les sommets des tuiles d'un niveau (GetLevel) : ceux d'une arête du cube sont partagés
// par ses deux faces, d'où un relief sans marche d'une face à l'autre.
namespace Flux::Planetology {
    class Terrain;

    struct ErosionSettings
    {
        double cellSize = 1000.0;   // m, écart visé entre deux points de la carte
        double strength = 1.0;      // force de l'érosion (vitesse de creusement des cours d'eau)
        double rain = 1.0;          // pluie : multiplie le débit de chaque maille
        double valleyDepth = 0.3;   // creusement le plus profond, en fraction du relief
        double valleySlope = 20.0;  // °, pente des versants des vallées (5 à 50)
        double channelArea = 4.0;   // mailles drainées à partir desquelles l'eau creuse (en dessous : crêtes intactes)
        std::uint32_t iterations = 24;
        // Mer : les points sous seaHeight (m au-dessus du rayon) reçoivent l'eau et ne changent pas.
        bool sea = false;
        double seaHeight = 0.0;

        bool operator==(const ErosionSettings&) const = default;
    };

    class FLUX_SIM_API ErosionMap {
        public:
        // Niveau de tuile le plus fin de la carte : 32 x 64 = 2 048 mailles par côté de face.
        static constexpr std::uint32_t MaxLevel = 6;
        // Pas de voisin (coin du cube, où trois faces se touchent).
        static constexpr std::uint32_t NoNode = 0xFFFFFFFFu;
        // Sens d'écoulement d'un point qui garde son eau (mer, ou point le plus bas d'une planète sans mer).
        static constexpr std::uint8_t Outlet = 0xFF;

        // Niveau de tuile dont les sommets sont les points de la carte (TileQuads.2^niveau mailles par côté de face),
        // celui dont l'écart des sommets est le plus proche de settings.cellSize.
        static std::uint32_t GetLevel(const Terrain& terrain, const ErosionSettings& settings);
        // Clé du cache : change avec le terrain (rayon, relief, graine, forme), les réglages et la méthode.
        static std::uint64_t GetKey(const Terrain& terrain, const ErosionSettings& settings);

        // Calcule la carte : plusieurs secondes sur une grande planète. threads : 0 = un par cœur.
        static ErosionMap Build(const Terrain& terrain, const ErosionSettings& settings, unsigned threads = 0);
        // Relit la carte dans cacheDirectory si elle y est et correspond, sinon la calcule et l'y enregistre. Un
        // fichier illisible ou d'une autre version est recalculé (avertissement au journal), jamais bloquant.
        static ErosionMap LoadOrBuild(const Terrain& terrain, const ErosionSettings& settings,
                                      const std::filesystem::path& cacheDirectory, unsigned threads = 0);
        // Carte partagée par tous ceux qui en ont besoin (affichage, collision, semis), relue ou calculée sur un fil
        // à part, dans bin/Cache : nullptr tant qu'elle n'est pas prête, sauf avec wait (attend la fin). Les quatre
        // dernières demandées restent en mémoire.
        static std::shared_ptr<const ErosionMap> Request(const Terrain& terrain, const ErosionSettings& settings, bool wait);

        bool IsEmpty() const { return m_resolution == 0; }
        std::uint32_t GetLevel() const { return m_level; }
        // Mailles par côté d'une face (TileQuads.2^GetLevel()) ; points par côté : une de plus.
        std::uint32_t GetResolution() const { return m_resolution; }
        std::uint32_t GetSide() const { return m_resolution + 1; }
        std::uint64_t GetKey() const { return m_key; }
        // Creusement le plus profond possible (m, positif).
        double GetDepth() const { return m_depth; }
        std::uint32_t GetNodeCount() const { return static_cast<std::uint32_t>(m_carve.size()); }

        // Points : index = (face.(n + 1) + j).(n + 1) + i, en s = -1 + 2i/n (de même pour t et j), comme les sommets
        // des tuiles du niveau GetLevel. Un point d'une arête existe sur chaque face qui la porte ; GetCanonical donne
        // celui qui le représente (le plus petit index), le seul à avoir des voisins et un sens d'écoulement.
        std::uint32_t GetNode(std::uint32_t face, std::uint32_t i, std::uint32_t j) const {
            return (face * GetSide() + j) * GetSide() + i;
        }
        std::uint32_t GetCanonical(std::uint32_t node) const;
        Math::Vector3d GetNodeDirection(std::uint32_t node) const;
        // Les voisins d'un point représentant (8 au plus, NoNode pour les places vides), de toutes ses faces.
        void GetNeighbors(std::uint32_t node, std::uint32_t (&neighbors)[8]) const;
        // Creusement (m, de -GetDepth() à 0), débit (mailles drainées, pluie comprise, au moins rain) et voisin où part
        // l'eau (index de GetNeighbors du représentant, ou Outlet) ; les copies d'un point ont les mêmes valeurs.
        float GetCarve(std::uint32_t node) const { return m_carve[node]; }
        float GetFlow(std::uint32_t node) const { return m_flow[node]; }
        std::uint8_t GetFlowDirection(std::uint32_t node) const { return m_direction[node]; }
        std::uint32_t GetReceiver(std::uint32_t node) const;

        // Creusement en un point de la face, u et v de 0 à n (unités de mailles) : Catmull-Rom (lisse, passe par les
        // points), mêlé au bilinéaire sur les deux mailles du bord de la face, où il ne dépend que des points de
        // l'arête (communs aux deux faces). Borné à [-GetDepth(), 0]. Le GPU reçoit ses valeurs aux sommets de chaque
        // tuile (TileGpuInputs::carve).
        double SampleFace(std::uint32_t face, double u, double v) const;
        // SampleFace sur une grille de la face (u[i], v[j]) dans out[j.u.size() + i], plus vite (Catmull-Rom séparé) ;
        // les points hors de la face (u ou v hors de 0 à n) sont laissés tels quels.
        void SampleGrid(std::uint32_t face, std::span<const double> u, std::span<const double> v, float* out) const;
        // Creusement (SampleFace) et débit (bilinéaire) dans une direction (unitaire).
        struct Sample
        {
            double carve = 0.0;
            double flow = 0.0;
        };
        Sample Get(const Math::Vector3d& direction) const;
        double GetCarve(const Math::Vector3d& direction) const;

        bool Save(const std::filesystem::path& file) const;
        // Faux si le fichier manque, est abîmé ou ne porte pas cette clé.
        bool Load(const std::filesystem::path& file, std::uint64_t key);

        private:
        std::uint32_t m_level = 0;
        std::uint32_t m_resolution = 0;
        std::uint64_t m_key = 0;
        double m_depth = 0.0;
        std::vector<double> m_tangents; // tan(s.π/4) des points, i = 0 à n
        std::vector<float> m_carve;
        std::vector<float> m_flow;
        std::vector<std::uint8_t> m_direction;
        // Points du bord des faces (4(n + 1) par face, voir BorderSlot) : leur représentant et ses voisins.
        std::vector<std::uint32_t> m_borderCanonical;
        std::vector<std::uint32_t> m_borderNeighbors;

        void SetLevel(std::uint32_t level);
        // Face et place (u, v de 0 à n) d'une direction.
        void Locate(const Math::Vector3d& direction, std::uint32_t& face, double& u, double& v) const;
        std::uint32_t BorderSlot(std::uint32_t face, std::uint32_t i, std::uint32_t j) const;
        bool IsBorder(std::uint32_t i, std::uint32_t j) const {
            return i == 0 || j == 0 || i == m_resolution || j == m_resolution;
        }
        std::uint32_t Canonical(std::uint32_t face, std::uint32_t i, std::uint32_t j) const {
            return IsBorder(i, j) ? m_borderCanonical[BorderSlot(face, i, j)] : GetNode(face, i, j);
        }
    };
}
