#include "Flux/Animation/Animator.hpp"

#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <utility>

namespace Flux::Animator {
    void Update(Scene& scene, double deltaSeconds) {
        for (const auto [entity, animator] : scene.GetRegistry().view<AnimatorComponent>().each()) {
            // Animation voulue, et sa vitesse de lecture.
            const std::string* wanted = &animator.animation;
            double rate = animator.speed;
            bool once = false;
            const auto* character = scene.TryGet<CharacterComponent>(entity);
            if (character && character->seat != NullEntity) {
                // Sur un siège : s'asseoir et se lever une fois, assis en boucle ; à défaut, le repos.
                const std::string& seated = animator.seatedAnimation.empty() ? animator.idleAnimation : animator.seatedAnimation;
                if (character->seatState == SeatState::SittingDown && !animator.sitDownAnimation.empty()) {
                    wanted = &animator.sitDownAnimation;
                    once = true;
                } else if (character->seatState == SeatState::StandingUp && !animator.standUpAnimation.empty()) {
                    wanted = &animator.standUpAnimation;
                    once = true;
                } else if (!seated.empty()) {
                    wanted = &seated;
                } else {
                    rate = 0.0;
                }
            } else if (character) {
                const float speed = character->groundSpeed;
                const float runThreshold = (character->walkSpeed + character->runSpeed) * 0.5f;
                if (!animator.airAnimation.empty() && character->airTime > AirDelay) {
                    wanted = &animator.airAnimation;
                } else if (speed < IdleSpeed) {
                    if (!animator.idleAnimation.empty()) {
                        wanted = &animator.idleAnimation;
                    } else {
                        rate = 0.0; // pas d'animation de repos : la marche se fige
                    }
                } else if (!animator.runAnimation.empty() && speed > runThreshold && character->runSpeed > 0.0f) {
                    wanted = &animator.runAnimation;
                    rate *= speed / character->runSpeed;
                } else if (character->walkSpeed > 0.0f) {
                    rate *= speed / character->walkSpeed;
                }
            }

            if (animator.playing != *wanted || animator.playingOnce != once) {
                if (animator.playing.empty() && animator.previous.empty() && animator.time == 0.0) {
                    animator.fade = 1.0f; // tout début du jeu : rien à fondre
                } else {
                    animator.previous = std::move(animator.playing);
                    animator.previousTime = animator.time;
                    animator.previousOnce = animator.playingOnce;
                    animator.previousRate = animator.rate;
                    animator.fade = 0.0f;
                }
                animator.playing = *wanted;
                animator.playingOnce = once;
                animator.time = 0.0;
            }
            animator.rate = rate;
            animator.time += deltaSeconds * rate;
            if (animator.fade < 1.0f) {
                animator.previousTime += deltaSeconds * animator.previousRate;
                animator.fade = std::min(1.0f, animator.fade + static_cast<float>(deltaSeconds) / FadeSeconds);
            }
        }
    }
}
