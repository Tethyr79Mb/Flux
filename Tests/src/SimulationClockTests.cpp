// Pas fixes de simulation (SimulationClock) et touches retenues d'un pas à l'autre (Input::Accumulate).
#include "Check.hpp"

#include "Flux/Core/SimulationClock.hpp"
#include "Flux/Platform/Input.hpp"

#include <cmath>
#include <cstdint>

using namespace Flux;

bool RunSimulationClockTests() {
    constexpr double Step = SimulationClock::StepSeconds;

    // Écran à 144 Hz : en moyenne 60 pas par seconde, jamais deux dans une image.
    SimulationClock clock;
    int steps = 0;
    for (int frame = 0; frame < 144; ++frame) {
        const int now = clock.Advance(1.0 / 144.0);
        CHECK(now <= 1);
        steps += now;
        CHECK(clock.GetAlpha() >= 0.0 && clock.GetAlpha() <= 1.0);
    }
    CHECK(steps == 59 || steps == 60);
    CHECK(clock.GetStep() == static_cast<std::uint64_t>(steps));

    // À 30 images/s, deux pas par image ; l'image tombe juste sur un pas.
    clock.Reset();
    CHECK(clock.Advance(2.0 * Step) == 2);
    CHECK(clock.GetAlpha() < 1.0e-9);
    // Une demi-image : rien à simuler, l'image se dessine à mi-chemin.
    CHECK(clock.Advance(0.5 * Step) == 0);
    CHECK(std::abs(clock.GetAlpha() - 0.5) < 1.0e-9);
    // Pause : le temps ne passe pas, l'image reste au même endroit.
    CHECK(clock.Advance(0.0) == 0);
    CHECK(std::abs(clock.GetAlpha() - 0.5) < 1.0e-9);

    // Blocage d'une seconde (point d'arrêt, chargement) : au plus 4 pas, le reste est abandonné.
    clock.Reset();
    CHECK(clock.Advance(1.0) == SimulationClock::MaxStepsPerFrame);
    CHECK(clock.Advance(0.0) == 0);

    // Touches : un appui pendant une image sans pas sert au pas suivant, une seule fois.
    Input frame;
    Input step;
    frame.SetKey(static_cast<std::uint16_t>(Key::Space), true);
    frame.AddMouseDelta(3.0f, 0.0f);
    step.Accumulate(frame); // image 1 : aucun pas
    frame.BeginFrame();     // image 2 : touche toujours enfoncée, plus « appuyée »
    frame.AddMouseDelta(2.0f, 0.0f);
    step.Accumulate(frame);
    CHECK(step.WasKeyPressed(Key::Space));
    CHECK(step.IsKeyDown(Key::Space));
    CHECK(step.GetMouseDelta().x == 5.0f);
    step.ClearEdges(); // le premier pas l'a lue
    CHECK(!step.WasKeyPressed(Key::Space)); // un deuxième pas dans la même image ne la revoit pas
    CHECK(step.IsKeyDown(Key::Space));
    CHECK(step.GetMouseDelta().x == 0.0f);
    frame.BeginFrame();
    frame.SetKey(static_cast<std::uint16_t>(Key::Space), false);
    step.Accumulate(frame);
    CHECK(!step.IsKeyDown(Key::Space));
    return true;
}
