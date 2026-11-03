#pragma once

#include <Flux/Platform/LocalHttpServer.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace FluxEditor {
    // Résultat d'un outil : textes et images (format MCP « content »), ou une erreur à montrer à Claude.
    struct McpToolResult
    {
        nlohmann::json content = nlohmann::json::array();
        bool isError = false;

        static McpToolResult Text(std::string text);
        static McpToolResult Error(std::string text);
        McpToolResult& AddText(std::string text);
        McpToolResult& AddPng(std::string_view png); // octets du PNG, encodés ici en base64
    };

    // Un outil qui attend quelques images (capture d'écran, mesures) rend une fonction rappelée au début de chaque
    // image jusqu'à ce qu'elle ait son résultat.
    using McpPoll = std::function<std::optional<McpToolResult>()>;
    using McpOutcome = std::variant<McpToolResult, McpPoll>;

    struct McpTool
    {
        std::string name;        // anglais, snake_case
        std::string description; // pour Claude : quand s'en servir, ce qu'il rend
        nlohmann::json inputSchema = {{"type", "object"}};
        std::function<McpOutcome(const nlohmann::json& arguments)> run;
    };

    // Serveur MCP (Model Context Protocol, transport « Streamable HTTP » sans flux SSE) de l'éditeur : Claude Code,
    // lancé sur le même PC, y appelle des outils (lire les mesures, modifier la scène, capture d'écran...). Le serveur
    // est déclaré dans .mcp.json à la racine du dépôt.
    // Les requêtes arrivent sur des fils réseau (LocalHttpServer) mais les outils tournent dans Update, sur le fil
    // principal, au début de l'image : la scène n'est jamais touchée en parallèle.
    class McpServer {
        public:
        McpServer();
        ~McpServer();

        // Port 0 : pas de serveur. Échec (port pris) : un avertissement dans la console, l'éditeur continue sans.
        void Start(std::uint16_t port);
        bool IsRunning() const { return m_http != nullptr; }
        std::uint16_t GetPort() const;

        void AddTool(McpTool tool);

        // Une fois par image : répond aux requêtes arrivées et rappelle les outils en attente. Sans requête : une
        // lecture atomique.
        void Update();

        private:
        // Réponse JSON-RPC à un message (vide pour une notification, qui n'en attend pas).
        std::optional<nlohmann::json> Handle(std::uint64_t request, const nlohmann::json& message);
        nlohmann::json CallTool(std::uint64_t request, const nlohmann::json& id, const nlohmann::json& params);
        void Reply(std::uint64_t request, const nlohmann::json& response);

        std::unique_ptr<Flux::Platform::LocalHttpServer> m_http;
        std::vector<McpTool> m_tools;
        struct Pending
        {
            std::uint64_t request; // requête HTTP qui attend
            nlohmann::json id;     // de la requête JSON-RPC
            McpPoll poll;
            int frames = 0;
        };
        std::vector<Pending> m_pending;
        std::vector<Pending> m_stillPending; // échangé avec m_pending à chaque image, capacité gardée
        std::vector<Flux::Platform::LocalHttpServer::Request> m_requests; // pris à cette image (réutilisé)
    };
}
