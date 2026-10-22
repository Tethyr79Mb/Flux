// Graphe de rendu (Renderer/RenderGraph) : ordre des passes et des étapes, passes et étapes sautées, heures du GPU
// aux mêmes cases d'une image à l'autre, barrières des textures déclarées, et description refusée quand une passe lit
// ce que rien n'a encore écrit.
#include "Check.hpp"

#include "Renderer/RenderGraph.hpp"

#include <array>
#include <format>
#include <string>
#include <vector>

using namespace Flux;

namespace {
    class NamedTexture final : public RHI::Texture {
        public:
        NamedTexture(const char* textureName, RHI::Format textureFormat) : name(textureName), format(textureFormat) {}
        RHI::Extent GetExtent() const override { return {1, 1}; }
        RHI::Format GetFormat() const override { return format; }
        const char* name;
        RHI::Format format;
    };

    // Liste de commandes qui note seulement ce que le graphe lui demande.
    class RecordingCommands final : public RHI::CommandList {
        public:
        std::vector<std::string> log;

        void UseTextures(std::span<const RHI::TextureUse> uses) override {
            std::string line = "barrières";
            for (const RHI::TextureUse& use : uses) {
                constexpr std::array names{"rien", "couleur", "profondeur", "lue"};
                line += std::format(" {}:{}{}", static_cast<const NamedTexture*>(use.texture)->name,
                                    names[static_cast<std::size_t>(use.access)], use.discard ? " jetée" : "");
            }
            log.push_back(line);
        }
        void BeginRenderPass(const RHI::RenderPassDesc&) override { log.emplace_back("début de passe"); }
        void EndRenderPass() override { log.emplace_back("fin de passe"); }
        void SetViewport(const RHI::Viewport&) override {}
        void SetScissor(const RHI::Rect&) override {}
        void ClearDepth(const RHI::Rect&, float) override {}
        void BindPipeline(const RHI::Pipeline&) override {}
        void SetBindGroup(std::uint32_t, const RHI::BindGroup&) override {}
        void BindVertexBuffers(std::uint32_t, std::span<const RHI::Buffer* const>) override {}
        void BindIndexBuffer(const RHI::Buffer&) override {}
        void PushConstants(const void*, std::uint32_t) override {}
        void Draw(std::uint32_t, std::uint32_t) override {}
        void DrawIndexed(std::uint32_t, std::uint32_t, std::uint32_t) override {}
        void DrawIndexedIndirect(const RHI::Buffer&, std::uint64_t, std::uint32_t) override {}
        void Dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override {}
        void ComputeBarrier() override {}
        void WriteTimestamp(std::uint32_t slot) override { log.push_back("heure " + std::to_string(slot)); }
        void BeginStep(const char* name) override { log.push_back(std::string("étape ") + name); }
    };
}

bool RunRenderGraphTests() {
    using Lifetime = RenderGraph::Lifetime;
    RenderGraph graph;
    const auto history = graph.AddResource("historique", Lifetime::Previous);
    const auto shadows = graph.AddResource("ombres");
    const auto image = graph.AddResource("image");
    bool scene = true;
    bool grid = false;
    graph.AddPass("ombres").When([&] { return scene; }).Writes(shadows).Step("ombres", [](RHI::CommandList& c) { c.Draw(3, 0); });
    graph.AddPass("scène")
        .When([&] { return scene; })
        .Reads(shadows)
        .Reads(history)
        .Writes(image)
        .Writes(history)
        .Begin([](RHI::CommandList& c) { c.BeginRenderPass({}); })
        .Step("opaques", [](RHI::CommandList& c) { c.Draw(3, 0); })
        .Step("grille", [](RHI::CommandList& c) { c.Draw(3, 0); }, [&] { return grid; });
    graph.AddPass("interface").Reads(image).Step("interface", [](RHI::CommandList& c) { c.Draw(3, 0); });
    CHECK(graph.Compile().has_value());
    CHECK(graph.GetTimestampCount() == 5);

    RecordingCommands commands;
    graph.Execute(commands, false);
    const std::vector<std::string> plain{"étape ombres", "début de passe", "étape opaques", "fin de passe", "étape interface"};
    CHECK(commands.log == plain);

    // Heures : une au début et une après chaque étape, même sautée (ici la grille, puis toute la scène).
    commands.log.clear();
    graph.Execute(commands, true);
    CHECK(commands.log.size() == plain.size() + 5);
    CHECK(commands.log.front() == "heure 0" && commands.log.back() == "heure 4");
    scene = false;
    commands.log.clear();
    graph.Execute(commands, true);
    const std::vector<std::string> sceneless{"heure 0", "heure 1", "heure 2", "heure 3", "étape interface", "heure 4"};
    CHECK(commands.log == sceneless);

    // Temps : la passe de deux étapes, puis chacune ; une heure inconnue rend la ligne inconnue.
    const std::vector<double> stamps{10.0, 11.0, 13.0, 13.5, -1.0};
    const auto timings = graph.ReadTimings(stamps);
    CHECK(timings.size() == 5);
    CHECK(std::string(timings[0].name) == "ombres" && !timings[0].step && timings[0].milliseconds == 1.0);
    CHECK(std::string(timings[1].name) == "scène" && timings[1].milliseconds == 2.5);
    CHECK(timings[2].step && timings[2].milliseconds == 2.0 && timings[3].step && timings[3].milliseconds == 0.5);
    CHECK(std::string(timings[4].name) == "interface" && timings[4].milliseconds < 0.0);

    // Barrières : textures lues puis écrites (cible de couleur, ou de profondeur selon le format) ; contenu jeté
    // seulement pour une texture de l'image ; texture absente à cette image (source nulle) ignorée.
    {
        RenderGraph textures;
        const NamedTexture shadowTexture("ombres", RHI::Format::D32Float);
        const NamedTexture colorTexture("couleur", RHI::Format::RGBA16Float);
        const NamedTexture depthTexture("profondeur", RHI::Format::D32Float);
        const NamedTexture* optional = nullptr;
        const auto shadowMap = textures.AddTexture("ombres", Lifetime::Previous, [&] { return &shadowTexture; });
        const auto color = textures.AddTexture("couleur", Lifetime::Frame, [&] { return &colorTexture; });
        const auto sceneDepth = textures.AddTexture("profondeur", Lifetime::Frame, [&] { return &depthTexture; });
        const auto extra = textures.AddTexture("facultative", Lifetime::Frame, [&] { return optional; });
        textures.AddPass("ombres").Writes(shadowMap).Step("ombres", [](RHI::CommandList&) {});
        textures.AddPass("scène").Reads(shadowMap).Writes(color).Writes(sceneDepth).Writes(extra).Step("scène", [](RHI::CommandList&) {});
        textures.AddPass("fin").Reads(color).Step("fin", [](RHI::CommandList&) {});
        CHECK(textures.Compile().has_value());
        RecordingCommands recorded;
        textures.Execute(recorded, false);
        const std::vector<std::string> expected{
            "barrières ombres:profondeur", "étape ombres",
            "barrières ombres:lue couleur:couleur jetée profondeur:profondeur jetée", "étape scène",
            "barrières couleur:lue", "étape fin"};
        CHECK(recorded.log == expected);

        // Une même texture lue et écrite par une passe : refusé (un seul usage à la fois).
        RenderGraph both;
        const auto feedback = both.AddTexture("historique", Lifetime::Previous, [&] { return &colorTexture; });
        both.AddPass("anticrénelage").Reads(feedback).Writes(feedback).Step("anticrénelage", [](RHI::CommandList&) {});
        CHECK(!both.Compile().has_value());
    }

    // Lire avant toute écriture de l'image : refusé, en nommant la passe et la ressource.
    RenderGraph wrong;
    const auto depth = wrong.AddResource("profondeur");
    wrong.AddPass("pyramide").Reads(depth).Step("pyramide", [](RHI::CommandList&) {});
    wrong.AddPass("scène").Writes(depth).Step("scène", [](RHI::CommandList&) {});
    const auto compiled = wrong.Compile();
    CHECK(!compiled.has_value());
    CHECK(compiled.error().find("pyramide") != std::string::npos && compiled.error().find("profondeur") != std::string::npos);
    // Écrire une ressource remplie hors du graphe : refusé aussi.
    RenderGraph external;
    const auto uploads = external.AddResource("envois", Lifetime::External);
    external.AddPass("calculs").Writes(uploads).Step("calculs", [](RHI::CommandList&) {});
    CHECK(!external.Compile().has_value());
    return true;
}
