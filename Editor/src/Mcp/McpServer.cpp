#include "Mcp/McpServer.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Core/Version.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <utility>

namespace FluxEditor {
    using Json = nlohmann::json;

    namespace {
        // Versions du protocole comprises, la plus récente d'abord : celle du client si on la connaît, sinon la nôtre.
        constexpr std::array<std::string_view, 3> ProtocolVersions{"2025-06-18", "2025-03-26", "2024-11-05"};

        // Ce que Claude lit en se branchant : les règles du jeu, valables pour tous les outils.
        constexpr std::string_view Instructions =
            "Éditeur FluxEditor (moteur FluxEngine, jeu Ars Solaris) ouvert sur le PC de Bruno. Les objets sont désignés "
            "par leur identifiant (Uuid) ou leur nom exact. Chaque modification passe par l'historique de l'éditeur "
            "(Ctrl+Z l'annule, l'outil undo aussi) ; rien n'est enregistré sur le disque : Bruno enregistre lui-même "
            "(Ctrl+S). En mode Jouer, la scène est une copie jetée à l'arrêt : les outils qui modifient refusent. "
            "Valeurs au format des fichiers de scène : positions en mètres (relatives au parent et au secteur de 1e9 m), "
            "rotations en quaternion [x, y, z, w] (ou {\"degrees\": [x, y, z]} à l'écriture, comme l'inspecteur), "
            "couleurs linéaires.";

        // Base64 standard (RFC 4648), pour les images.
        std::string Base64(std::string_view bytes) {
            constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((bytes.size() + 2) / 3 * 4);
            std::size_t i = 0;
            for (; i + 2 < bytes.size(); i += 3) {
                const std::uint32_t n = static_cast<std::uint8_t>(bytes[i]) << 16 | static_cast<std::uint8_t>(bytes[i + 1]) << 8
                                      | static_cast<std::uint8_t>(bytes[i + 2]);
                out += Alphabet[n >> 18 & 63];
                out += Alphabet[n >> 12 & 63];
                out += Alphabet[n >> 6 & 63];
                out += Alphabet[n & 63];
            }
            if (i < bytes.size()) {
                const bool two = i + 1 < bytes.size();
                const std::uint32_t n = static_cast<std::uint8_t>(bytes[i]) << 16 | (two ? static_cast<std::uint8_t>(bytes[i + 1]) << 8 : 0);
                out += Alphabet[n >> 18 & 63];
                out += Alphabet[n >> 12 & 63];
                out += two ? Alphabet[n >> 6 & 63] : '=';
                out += '=';
            }
            return out;
        }

        Json RpcResult(const Json& id, Json result) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
        }

        Json RpcError(const Json& id, int code, std::string message) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(message)}}}};
        }

        Json ToJson(const McpToolResult& result) {
            return {{"content", result.content}, {"isError", result.isError}};
        }

        // Une page web ouverte dans le navigateur pourrait appeler 127.0.0.1 : elle envoie toujours son origine.
        // Claude Code n'en envoie pas ; une page servie en local reste acceptée.
        bool IsLocalOrigin(std::string_view origin) {
            return origin.empty() || origin.starts_with("http://localhost") || origin.starts_with("http://127.0.0.1");
        }

        // Les outils attendent au plus une trentaine de secondes ; au-delà, quelque chose est bloqué.
        constexpr int MaxPendingFrames = 60 * 90;
    }

    McpToolResult McpToolResult::Text(std::string text) {
        McpToolResult result;
        result.AddText(std::move(text));
        return result;
    }

    McpToolResult McpToolResult::Error(std::string text) {
        McpToolResult result = Text(std::move(text));
        result.isError = true;
        return result;
    }

    McpToolResult& McpToolResult::AddText(std::string text) {
        content.push_back({{"type", "text"}, {"text", std::move(text)}});
        return *this;
    }

    McpToolResult& McpToolResult::AddPng(std::string_view png) {
        content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", Base64(png)}});
        return *this;
    }

    McpServer::McpServer() = default;
    McpServer::~McpServer() = default;

    void McpServer::Start(std::uint16_t port) {
        if (port == 0) {
            return;
        }
        auto server = Flux::Platform::LocalHttpServer::Start(port);
        if (!server) {
            Flux::Log::Warning("serveur MCP (Claude) non démarré : {}", server.error());
            return;
        }
        m_http = std::move(*server);
        Flux::Log::Info("serveur MCP (Claude) : http://127.0.0.1:{}/mcp", port);
    }

    std::uint16_t McpServer::GetPort() const {
        return m_http ? m_http->GetPort() : 0;
    }

    void McpServer::AddTool(McpTool tool) {
        m_tools.push_back(std::move(tool));
    }

    void McpServer::Update() {
        if (!m_http) {
            return;
        }
        // Outils en attente d'abord : ceux qui arrivent maintenant ont au moins une image devant eux.
        if (!m_pending.empty()) {
            m_stillPending.clear();
            for (Pending& pending : m_pending) {
                std::optional<McpToolResult> result;
                try {
                    result = pending.poll();
                } catch (const std::exception& error) {
                    result = McpToolResult::Error(error.what());
                }
                if (!result && ++pending.frames > MaxPendingFrames) {
                    result = McpToolResult::Error("délai dépassé");
                }
                if (result) {
                    Reply(pending.request, RpcResult(pending.id, ToJson(*result)));
                } else {
                    m_stillPending.push_back(std::move(pending));
                }
            }
            std::swap(m_pending, m_stillPending);
        }

        m_requests.clear();
        m_http->TakeRequests(m_requests);
        for (Flux::Platform::LocalHttpServer::Request& request : m_requests) {
            const Flux::Platform::HttpRequest& http = request.http;
            if (!IsLocalOrigin(http.origin)) {
                m_http->Respond(request.id, {.status = 403, .contentType = "text/plain", .body = "origine refusée"});
                continue;
            }
            if (http.path != "/mcp") {
                m_http->Respond(request.id, {.status = 404, .contentType = "text/plain", .body = "le serveur MCP est sur /mcp"});
                continue;
            }
            // Pas de flux SSE ouvert par le serveur (GET) ni de session à fermer (DELETE) : permis par le protocole.
            if (http.method != "POST") {
                m_http->Respond(request.id, {.status = 405, .contentType = "text/plain", .body = {}});
                continue;
            }
            Json message;
            try {
                message = Json::parse(http.body);
            } catch (const std::exception& error) {
                Reply(request.id, RpcError(nullptr, -32700, std::string("JSON illisible : ") + error.what()));
                continue;
            }
            std::optional<Json> response;
            try {
                response = Handle(request.id, message);
            } catch (const std::exception& error) {
                response = RpcError(message.is_object() ? message.value("id", Json()) : Json(), -32603, error.what());
            }
            if (response && response->is_null()) {
                continue; // outil en attente : il répondra plus tard
            }
            if (response) {
                Reply(request.id, *response);
            } else {
                m_http->Respond(request.id, {.status = 202, .contentType = "text/plain", .body = {}});
            }
        }
    }

    std::optional<Json> McpServer::Handle(std::uint64_t request, const Json& message) {
        if (!message.is_object() || !message.contains("method")) {
            // Réponse du client (aucune demande de notre part) ou lot (retiré du protocole) : rien à faire.
            return message.is_object() && message.contains("id") && !message.contains("method")
                       ? std::nullopt
                       : std::optional<Json>(RpcError(nullptr, -32600, "requête JSON-RPC attendue (un objet)"));
        }
        const std::string method = message.at("method").get<std::string>();
        if (!message.contains("id")) {
            return std::nullopt; // notification (notifications/initialized...)
        }
        const Json& id = message.at("id");
        const Json params = message.value("params", Json::object());
        if (method == "initialize") {
            const std::string asked = params.value("protocolVersion", std::string{});
            const bool known = std::ranges::find(ProtocolVersions, asked) != ProtocolVersions.end();
            return RpcResult(id, {{"protocolVersion", known ? asked : std::string(ProtocolVersions[0])},
                                  {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                  {"serverInfo", {{"name", "flux-editor"}, {"version", std::string(Flux::GetVersion())}}},
                                  {"instructions", Instructions}});
        }
        if (method == "ping") {
            return RpcResult(id, Json::object());
        }
        if (method == "tools/list") {
            Json tools = Json::array();
            for (const McpTool& tool : m_tools) {
                tools.push_back({{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.inputSchema}});
            }
            return RpcResult(id, {{"tools", std::move(tools)}});
        }
        if (method == "tools/call") {
            return CallTool(request, id, params);
        }
        return RpcError(id, -32601, std::format("méthode inconnue : {}", method));
    }

    Json McpServer::CallTool(std::uint64_t request, const Json& id, const Json& params) {
        const std::string name = params.value("name", std::string{});
        const auto tool = std::ranges::find(m_tools, name, &McpTool::name);
        if (tool == m_tools.end()) {
            return RpcError(id, -32602, std::format("outil inconnu : {}", name));
        }
        const Json arguments = params.value("arguments", Json::object());
        McpOutcome outcome;
        try {
            outcome = tool->run(arguments.is_object() ? arguments : Json::object());
        } catch (const std::exception& error) {
            // Argument du mauvais type (nlohmann::json::type_error...) : Claude corrige et recommence.
            return RpcResult(id, ToJson(McpToolResult::Error(std::format("{} : {}", name, error.what()))));
        }
        if (McpPoll* poll = std::get_if<McpPoll>(&outcome)) {
            m_pending.push_back({request, id, std::move(*poll)});
            return nullptr; // réponse plus tard (Update)
        }
        return RpcResult(id, ToJson(std::get<McpToolResult>(outcome)));
    }

    void McpServer::Reply(std::uint64_t request, const Json& response) {
        // Texte non UTF-8 (nom de fichier mal décodé...) : remplacé plutôt qu'une exception.
        m_http->Respond(request, {.status = 200, .contentType = "application/json",
                                  .body = response.dump(-1, ' ', false, Json::error_handler_t::replace)});
    }
}
