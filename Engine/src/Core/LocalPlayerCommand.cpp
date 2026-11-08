#include "LocalPlayerCommand.hpp"

#include "Flux/Input/InputActions.hpp"

namespace Flux {
    PlayerCommand SampleLocalCommand(const Input& input, const InputActions& actions, bool keyboardFree, const LocalView& view,
                                     const WalkActions& walk, std::uint64_t step) {
        PlayerCommand command;
        command.step = step;
        if (keyboardFree) {
            actions.ReadKeys(input, command);
        }
        command.move = view.walkForward * command.GetAxis(walk.forward, walk.back)
                     + view.walkRight * command.GetAxis(walk.right, walk.left);
        command.stickYaw = view.stickYaw;
        command.stickPitch = view.stickPitch;
        command.viewPosition = view.position;
        command.viewForward = view.forward;
        return command;
    }
}
