#include "SceneViewport.hpp"

#include "SceneEditor.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Input.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace FluxEditor {
    using namespace Flux;
    using Math::Vector2f;
    using Math::Vector3d;

    namespace {
        constexpr float ArrowPixels = 100.0f;   // longueur des flèches du gizmo à l'écran
        constexpr float HeadLength = 14.0f;      // pointe de flèche
        constexpr float HeadHalfWidth = 6.0f;
        constexpr float GrabPixels = 7.0f;       // distance de saisie d'une flèche
        constexpr float MinVisiblePixels = 12.0f; // axe pointé vers la caméra : trop court, masqué
        constexpr Vector3d Axes[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

        float Length(Vector2f v) {
            return std::sqrt(v.x * v.x + v.y * v.y);
        }

        float DistanceToSegment(Vector2f p, Vector2f a, Vector2f b) {
            const Vector2f ab = b - a;
            const float lengthSquared = ab.x * ab.x + ab.y * ab.y;
            const float t = lengthSquared > 0.0f
                                ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lengthSquared, 0.0f, 1.0f)
                                : 0.0f;
            return Length(p - (a + ab * t));
        }

        // Flèche d'un axe à l'écran : de l'origine du gizmo à la pointe.
        struct Arrow
        {
            bool visible = false;
            Vector2f start;
            Vector2f end;  // base de la pointe
            Vector2f tip;
            Vector2f normal; // demi-largeur de la pointe
        };

        // Flèches du gizmo placé en origin (relatif à la caméra) ; nullopt s'il est derrière la caméra.
        std::optional<std::array<Arrow, 3>> ProjectGizmo(const ScreenProjection& projection, const Vector3d& origin) {
            const std::optional<Vector2f> start = projection.ToScreen(origin);
            if (!start) {
                return std::nullopt;
            }
            const double length = projection.PixelsToMeters(ArrowPixels, projection.GetDepth(origin));
            std::array<Arrow, 3> arrows;
            for (int axis = 0; axis < 3; ++axis) {
                Arrow& arrow = arrows[axis];
                Vector2f a;
                Vector2f b;
                if (!projection.ProjectSegment(origin, origin + Axes[axis] * length, a, b)) {
                    continue;
                }
                const float pixels = Length(b - *start);
                if (pixels < MinVisiblePixels) {
                    continue;
                }
                const Vector2f direction = (b - *start) * (1.0f / pixels);
                arrow.visible = true;
                arrow.start = *start;
                arrow.end = b;
                arrow.tip = b + direction * HeadLength;
                arrow.normal = Vector2f{-direction.y, direction.x} * HeadHalfWidth;
            }
            return arrows;
        }

        // Les 12 arêtes du cube unité placé par transform.
        void DrawBoxEdges(UI::DrawList& list, const ScreenProjection& projection, const RelativeTransform& transform,
                          Math::LinearColor color) {
            std::array<Vector3d, 8> corners;
            for (int i = 0; i < 8; ++i) {
                const Vector3d local{(i & 1 ? 0.5 : -0.5) * transform.scale.x, (i & 2 ? 0.5 : -0.5) * transform.scale.y,
                                     (i & 4 ? 0.5 : -0.5) * transform.scale.z};
                corners[i] = transform.position + transform.rotation.Rotate(local);
            }
            // Arêtes : coins qui ne diffèrent que d'un bit (un seul axe).
            for (int i = 0; i < 8; ++i) {
                for (const int bit : {1, 2, 4}) {
                    if ((i & bit) == 0) {
                        Vector2f a;
                        Vector2f b;
                        if (projection.ProjectSegment(corners[i], corners[i | bit], a, b)) {
                            list.AddLine(a, b, color, 2.0f);
                        }
                    }
                }
            }
        }
    }

    void SceneViewport::UpdateTextDrag(Engine& engine, Entity selected, const UI::Rect& view) {
        Scene& scene = engine.GetScene();
        ScreenTextComponent* text = selected == NullEntity ? nullptr : scene.TryGet<ScreenTextComponent>(selected);
        if (!text || scene.GetId(selected) != m_textDrag.entity) {
            m_textDrag.active = false; // texte supprimé, composant retiré ou sélection changée ailleurs
            return;
        }
        const Input& input = engine.GetWindow().GetInput();
        float x = m_textDrag.startOffsetX;
        float y = m_textDrag.startOffsetY;
        if (input.WasKeyPressed(Key::Escape) && !engine.GetUi().WantsKeyboard()) {
            m_textDrag.active = false; // annulé : retour au point de départ
        } else {
            // Pixels de la vue ramenés en pixels de référence ; décalage arrondi, comme dans l'inspecteur.
            const float scale = ScreenUi::GetScale(view.min, view.max);
            const Vector2f moved = input.GetMousePosition() - m_textDrag.startMouse;
            x = std::round(x + moved.x / scale);
            y = std::round(y + moved.y / scale);
            if (!input.IsMouseButtonDown(MouseButton::Left)) {
                m_textDrag.active = false;
            }
        }
        if (x != text->offsetX || y != text->offsetY) {
            text->offsetX = x;
            text->offsetY = y;
            m_modified = true;
        }
    }

    void SceneViewport::Update(Engine& engine, SceneEditor& sceneEditor, const UI::Rect& view) {
        UI::Context& ui = engine.GetUi();
        const Input& input = engine.GetWindow().GetInput();
        Scene& scene = engine.GetScene();
        const Camera& camera = engine.GetCamera();
        const Vector2f size = view.GetSize();
        if (size.x < 1.0f || size.y < 1.0f) {
            m_drag.axis = -1;
            m_textDrag.active = false;
            m_textRects.clear();
            return; // vue écrasée par les panneaux
        }

        const ScreenProjection projection(camera, view.min, view.max);
        const Vector2f mouse = input.GetMousePosition();
        const Ray ray = projection.GetRay(mouse);
        scene.ComputeRelativeTransforms(camera.position, m_transforms);
        const auto findTransform = [&](Entity entity) -> RelativeTransform* {
            const auto found = std::ranges::find(m_transforms, entity, &RelativeTransform::entity);
            return found == m_transforms.end() ? nullptr : &*found;
        };
        const Entity selected = sceneEditor.GetSelection() ? scene.FindById(sceneEditor.GetSelection()) : NullEntity;
        RelativeTransform* selectedTransform = selected == NullEntity ? nullptr : findTransform(selected);
        // Un texte à l'écran n'est pas dans le monde : pas de gizmo, il se glisse directement.
        const bool selectedIsText = selected != NullEntity && scene.TryGet<ScreenTextComponent>(selected);

        if (m_textDrag.active) {
            UpdateTextDrag(engine, selected, view);
        }

        // Glissement d'une flèche : le point de l'axe saisi suit le point de l'axe le plus proche de la souris.
        if (m_drag.axis >= 0) {
            if (!selectedTransform || scene.GetId(selected) != m_drag.entity) {
                m_drag.axis = -1; // entité supprimée ou sélection changée ailleurs
            } else {
                auto& position = scene.Get<TransformComponent>(selected).position;
                const Vector3d axis = Axes[m_drag.axis];
                // Origine du gizmo au départ, vue depuis la caméra actuelle.
                const Vector3d startPoint = m_drag.startPoint + Math::Delta(camera.position, m_drag.startCamera);
                Vector3d move{};
                if (input.WasKeyPressed(Key::Escape) && !ui.WantsKeyboard()) {
                    m_drag.axis = -1; // annulé : retour à la position de départ
                } else if (const std::optional<double> t = ClosestPointOnLine(ray, startPoint, axis)) {
                    move = axis * (*t - m_drag.startParameter);
                }
                // Déplacement dans le monde ramené dans l'espace du parent (sa rotation et son échelle).
                Vector3d localMove = move;
                if (const Entity parent = scene.GetParent(selected); parent != NullEntity) {
                    if (const RelativeTransform* parentTransform = findTransform(parent)) {
                        const Vector3d scale(parentTransform->scale);
                        const Vector3d rotated = parentTransform->rotation.Conjugate().Rotate(move);
                        localMove = {scale.x != 0.0 ? rotated.x / scale.x : 0.0, scale.y != 0.0 ? rotated.y / scale.y : 0.0,
                                     scale.z != 0.0 ? rotated.z / scale.z : 0.0};
                    }
                }
                const Vector3d newPosition = m_drag.startLocalPosition + localMove;
                if (newPosition != position) {
                    position = newPosition;
                    m_modified = true;
                }
                selectedTransform->position = startPoint + move; // pour dessiner le gizmo sans décalage d'une frame
                if (!input.IsMouseButtonDown(MouseButton::Left)) {
                    m_drag.axis = -1;
                }
            }
        }

        const std::optional<std::array<Arrow, 3>> arrows = selectedTransform && !selectedIsText
                                                               ? ProjectGizmo(projection, selectedTransform->position)
                                                               : std::nullopt;

        // La souris est à la vue : pas de panneau survolé ni de widget en main, pas de vol libre en cours.
        const bool mouseInView = mouse.x >= view.min.x && mouse.x < view.max.x && mouse.y >= view.min.y
                              && mouse.y < view.max.y && !ui.WantsMouse() && !engine.GetWindow().IsCursorLocked()
                              && !engine.GetCameraController().IsActive();
        if (m_drag.axis < 0 && !m_textDrag.active) {
            m_hoveredAxis = -1;
            if (mouseInView && arrows) {
                float closest = GrabPixels;
                for (int axis = 0; axis < 3; ++axis) {
                    const Arrow& arrow = (*arrows)[axis];
                    if (arrow.visible) {
                        const float distance = DistanceToSegment(mouse, arrow.start, arrow.tip);
                        if (distance < closest) {
                            closest = distance;
                            m_hoveredAxis = axis;
                        }
                    }
                }
            }
            if (mouseInView && input.WasMouseButtonPressed(MouseButton::Left)) {
                std::optional<double> grabbed;
                if (m_hoveredAxis >= 0) {
                    grabbed = ClosestPointOnLine(ray, selectedTransform->position, Axes[m_hoveredAxis]);
                }
                // Texte sous la souris (le dernier dessiné est au-dessus) : il passe avant les objets 3D qu'il recouvre.
                const auto text = std::ranges::find_if(m_textRects.rbegin(), m_textRects.rend(), [&](const auto& rect) {
                    return scene.IsValid(rect.entity) && mouse.x >= rect.min.x && mouse.x < rect.max.x
                        && mouse.y >= rect.min.y && mouse.y < rect.max.y;
                });
                if (grabbed) {
                    m_drag = {m_hoveredAxis, scene.GetId(selected), scene.Get<TransformComponent>(selected).position,
                              selectedTransform->position, camera.position, *grabbed};
                } else if (text != m_textRects.rend() && scene.TryGet<ScreenTextComponent>(text->entity)) {
                    const auto& component = scene.Get<ScreenTextComponent>(text->entity);
                    sceneEditor.Select(scene, text->entity);
                    m_textDrag = {true, scene.GetId(text->entity), component.offsetX, component.offsetY, mouse};
                } else {
                    // Clic dans le vide : désélectionne.
                    sceneEditor.Select(scene, PickEntity(scene, m_transforms, ray));
                }
            }
        }

        // Dessin, sous les panneaux (l'interface est ajoutée à l'overlay après) et découpé à la vue :
        // les textes de la scène comme dans le jeu, puis la sélection par-dessus.
        UI::DrawList& list = engine.GetRenderer().GetOverlay();
        m_textRects.clear();
        ScreenUi::Draw(scene, list, view.min, view.max, &m_textRects);
        if (!selectedTransform) {
            return;
        }
        list.SetClipRect(view.min, view.max);
        if (selectedIsText) {
            const auto rect = std::ranges::find(m_textRects, selected, &ScreenUi::ItemRect::entity);
            if (rect != m_textRects.end()) {
                const Vector2f margin{4.0f, 2.0f};
                list.AddRectOutline(rect->min - margin, rect->max + margin, Math::LinearColor::FromSrgb8(255, 165, 40), 2.0f);
            }
        }
        if (scene.TryGet<MeshRendererComponent>(selected)) {
            DrawBoxEdges(list, projection, *selectedTransform, Math::LinearColor::FromSrgb8(255, 165, 40));
        }
        if (arrows) {
            const UI::Style& style = ui.GetStyle();
            const Math::LinearColor colors[3] = {style.axisX, style.axisY, style.axisZ};
            const Math::LinearColor highlight = Math::LinearColor::FromSrgb8(255, 220, 60);
            const int active = m_drag.axis >= 0 ? m_drag.axis : m_hoveredAxis;
            for (int axis = 0; axis < 3; ++axis) {
                const Arrow& arrow = (*arrows)[axis];
                if (!arrow.visible || (m_drag.axis >= 0 && axis != m_drag.axis)) {
                    continue; // pendant un glissement, seule la flèche saisie reste
                }
                const Math::LinearColor color = axis == active ? highlight : colors[axis];
                list.AddLine(arrow.start, arrow.end, color, 3.0f);
                list.AddTriangle(arrow.tip, arrow.end + arrow.normal, arrow.end - arrow.normal, color);
            }
            if (const std::optional<Vector2f> center = projection.ToScreen(selectedTransform->position)) {
                list.AddRect(*center - Vector2f{3.0f, 3.0f}, *center + Vector2f{3.0f, 3.0f},
                             Math::LinearColor::FromSrgb8(235, 235, 235));
            }
        }
        list.ResetClipRect();
    }
}
