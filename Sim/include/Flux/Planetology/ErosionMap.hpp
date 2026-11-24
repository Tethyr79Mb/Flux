#pragma once

#include "Flux/SimExport.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

// Carte d'érosion d'une planète : de vraies vallées, creusées par l'eau qui coule, que le bruit seul ne sait pas
// faire (une vallée dépend de tout l'amont). Calculée une fois depuis la graine, à l'échelle de toute la planète (une
// maille d'environ 1 km), puis gardée en cache ; le relief en formule garde le détail fin par-dessus.
// Le calcul est sur le CPU et déterministe (même carte sur le client et le serveur, quel que soit le nombre de fils) :
// seulement des + - * / et des racines, dans un ordre fixe ; les hauteurs de départ (Terrain::GetHeight, qui passe
// par exp, pow, sin) sont arrondies au 1/16 de mètre pour effacer l'écart d'une bibliothèque mathématique à l'autre.
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
        // Mer : les mailles sous seaHeight (m au-dessus du rayon) reçoivent l'eau et ne changent pas.
        bool sea = false;
        double seaHeight = 0.0;

        bool operator==(const ErosionSettings&) const = default;
    };

    class FLUX_SIM_API ErosionMap {
        public:
        // Points par côté d'une face, au plus.
        static constexpr std::uint32_t MaxResolution = 2048;
        // Pas de voisin (coin du cube, où trois faces se touchent).
        static constexpr std::uint32_t NoNode = 0xFFFFFFFFu;
        // Sens d'écoulement d'une maille qui garde son eau (mer, ou point le plus bas d'une planète sans mer).
        static constexpr std::uint8_t Outlet = 0xFF;

        // Points par côté d'une face pour ce terrain et ces réglages (16 à MaxResolution).
        static std::uint32_t GetResolution(const Terrain& terrain, const ErosionSettings& settings);
        // Clé du cache : change avec le terrain (rayon, relief, graine, forme), les réglages et la méthode.
        static std::uint64_t GetKey(const Terrain& terrain, const ErosionSettings& settings);

        // Calcule la carte : plusieurs secondes sur une grande planète. threads : 0 = un par cœur.
        static ErosionMap Build(const Terrain& terrain, const ErosionSettings& settings, unsigned threads = 0);
        // Relit la carte dans cacheDirectory si elle y est et correspond, sinon la calcule et l'y enregistre. Un
        // fichier illisible ou d'une autre version est recalculé (avertissement au journal), jamais bloquant.
        static ErosionMap LoadOrBuild(const Terrain& terrain, const ErosionSettings& settings,
                                      const std::filesystem::path& cacheDirectory, unsigned threads = 0);

        bool IsEmpty() const { return m_resolution == 0; }
        std::uint32_t GetResolution() const { return m_resolution; }
        std::uint64_t GetKey() const { return m_key; }
        std::uint32_t GetNodeCount() const { return static_cast<std::uint32_t>(m_carve.size()); }

        // Mailles : index = (face.n + j).n + i, centre en s = -1 + (2i + 1)/n (de même pour t et j).
        Math::Vector3d GetNodeDirection(std::uint32_t node) const;
        // Les 8 voisins (NoNode au coin du cube), dans l'ordre (-1,-1) (0,-1) (1,-1) (-1,0) (1,0) (-1,1) (0,1) (1,1).
        void GetNeighbors(std::uint32_t node, std::uint32_t (&neighbors)[8]) const;
        // Creusement (m, de -valleyDepth.relief à 0), débit (mailles drainées, pluie comprise, au moins rain) et
        // voisin où part l'eau (index de GetNeighbors, ou Outlet).
        float GetCarve(std::uint32_t node) const { return m_carve[node]; }
        float GetFlow(std::uint32_t node) const { return m_flow[node]; }
        std::uint8_t GetFlowDirection(std::uint32_t node) const { return m_direction[node]; }
        std::uint32_t GetReceiver(std::uint32_t node) const;

        // Valeurs interpolées entre les mailles (bilinéaire, coutures du cube comprises).
        struct Sample
        {
            double carve = 0.0;
            double flow = 0.0;
        };
        Sample Get(const Math::Vector3d& direction) const;

        bool Save(const std::filesystem::path& file) const;
        // Faux si le fichier manque, est abîmé ou ne porte pas cette clé.
        bool Load(const std::filesystem::path& file, std::uint64_t key);

        private:
        std::uint32_t m_resolution = 0;
        std::uint64_t m_key = 0;
        std::vector<double> m_tangents; // tan(s.π/4) des centres, de i = -1 à n (n + 2 valeurs)
        std::vector<float> m_carve;
        std::vector<float> m_flow;
        std::vector<std::uint8_t> m_direction;
        // Voisins des mailles du bord des faces, calculés une fois (4n par face, voir BorderSlot).
        std::vector<std::uint32_t> m_borderNeighbors;

        void SetResolution(std::uint32_t resolution);
        std::uint32_t ResolveNode(std::uint32_t face, std::int64_t i, std::int64_t j) const;
        std::uint32_t BorderSlot(std::uint32_t face, std::uint32_t i, std::uint32_t j) const;
    };
}
