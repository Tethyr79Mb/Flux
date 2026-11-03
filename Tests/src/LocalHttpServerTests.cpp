// Serveur HTTP local des outils (serveur MCP de l'éditeur) : requêtes lues par ses fils, réponses données par le fil
// principal, plusieurs requêtes sur une même connexion.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Check.hpp"

#include "Flux/Platform/LocalHttpServer.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Flux;

namespace {
    // Client minimal : une connexion à 127.0.0.1, des requêtes envoyées telles quelles, les réponses lues jusqu'à la
    // fin du corps annoncé.
    class Client {
        public:
        explicit Client(std::uint16_t port) {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
            m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            m_connected = connect(m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        }

        ~Client() {
            closesocket(m_socket);
            WSACleanup();
        }

        bool IsConnected() const { return m_connected; }

        void Send(const std::string& request) { send(m_socket, request.data(), static_cast<int>(request.size()), 0); }

        // Réponse complète (en-têtes et corps), vide si la connexion se ferme avant.
        std::string Receive() {
            std::string response;
            char chunk[4096];
            while (true) {
                const std::size_t headerEnd = response.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    const std::size_t lengthAt = response.find("Content-Length: ");
                    const std::size_t length = lengthAt < headerEnd ? std::stoul(response.substr(lengthAt + 16)) : 0;
                    if (response.size() >= headerEnd + 4 + length) {
                        return response;
                    }
                }
                const int received = recv(m_socket, chunk, sizeof(chunk), 0);
                if (received <= 0) {
                    return {};
                }
                response.append(chunk, static_cast<std::size_t>(received));
            }
        }

        private:
        SOCKET m_socket = INVALID_SOCKET;
        bool m_connected = false;
    };

    // Comme le fil principal de l'éditeur : prend les requêtes à chaque « image » jusqu'à en avoir une.
    std::vector<Platform::LocalHttpServer::Request> WaitRequests(Platform::LocalHttpServer& server) {
        std::vector<Platform::LocalHttpServer::Request> requests;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (requests.empty() && std::chrono::steady_clock::now() < deadline) {
            server.TakeRequests(requests);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return requests;
    }
}

bool RunLocalHttpServerTests() {
    auto server = Platform::LocalHttpServer::Start(0);
    CHECK(server.has_value());
    const std::uint16_t port = (*server)->GetPort();
    CHECK(port != 0);
    // Un second serveur sur le même port est refusé (un deuxième éditeur ouvert).
    CHECK(!Platform::LocalHttpServer::Start(port).has_value());

    Client client(port);
    CHECK(client.IsConnected());
    std::vector<Platform::LocalHttpServer::Request> none;
    (*server)->TakeRequests(none);
    CHECK(none.empty());

    // Deux requêtes à la suite sur la même connexion, la réponse donnée plus tard par le fil principal.
    for (int i = 0; i < 2; ++i) {
        const std::string body = i == 0 ? R"({"jsonrpc":"2.0","id":1,"method":"ping"})" : "été";
        client.Send("POST /mcp?x=1 HTTP/1.1\r\nHost: 127.0.0.1\r\ncontent-length: " + std::to_string(body.size())
                    + "\r\nOrigin: http://localhost:3000\r\n\r\n" + body);
        std::vector<Platform::LocalHttpServer::Request> requests = WaitRequests(**server);
        CHECK(requests.size() == 1);
        CHECK(requests[0].http.method == "POST");
        CHECK(requests[0].http.path == "/mcp");
        CHECK(requests[0].http.origin == "http://localhost:3000");
        CHECK(requests[0].http.body == body);
        (*server)->Respond(requests[0].id, {.status = 200, .contentType = "application/json", .body = "réponse " + std::to_string(i)});
        const std::string response = client.Receive();
        CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(response.ends_with("\r\n\r\nréponse " + std::to_string(i)));
        CHECK(response.find("Content-Type: application/json") != std::string::npos);
    }

    // Requête sans corps, réponse sans corps.
    client.Send("GET /autre HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    std::vector<Platform::LocalHttpServer::Request> requests = WaitRequests(**server);
    CHECK(requests.size() == 1);
    CHECK(requests[0].http.method == "GET" && requests[0].http.path == "/autre" && requests[0].http.body.empty());
    (*server)->Respond(requests[0].id, {.status = 405, .contentType = "text/plain", .body = {}});
    CHECK(client.Receive().starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    (*server)->Respond(requests[0].id, {}); // déjà répondu : sans effet

    // Le serveur se ferme même avec une connexion ouverte et une requête sans réponse (fils débloqués).
    client.Send("POST /mcp HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}");
    CHECK(WaitRequests(**server).size() == 1);
    server->reset();
    return true;
}
