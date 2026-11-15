#include "LocalPlayerCommand.hpp"

#include "Flux/Input/InputActions.hpp"

namespace Flux {
    PlayerCommand SampleLocalCommand(const Input& input, const InputActions& actions, bool keyboardFree, const LocalView& view,
                                     const WalkActions& walk, std::uint64_t step, InjectedCommand* injected) {
        PlayerCommand command;
        command.step = step;
        if (keyboardFree) {
            actions.ReadKeys(input, command);
        }
        const bool injecting = injected && injected->steps > 0;
        if (injecting) {
            command.held |= injected->held;
            if (injected->first) {
                command.pressed |= injected->held | injected->pressed;
                injected->first = false;
            }
            --injected->steps;
        }
        command.move = view.walkForward * command.GetAxis(walk.forward, walk.back)
                     + view.walkRight * command.GetAxis(walk.right, walk.left);
        const bool steered = injecting && injected->steer;
        command.stickYaw = steered ? injected->stickYaw : view.stickYaw;
        command.stickPitch = steered ? injected->stickPitch : view.stickPitch;
        command.viewPosition = view.position;
        command.viewForward = view.forward;
        return command;
    }
}
