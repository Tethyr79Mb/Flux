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
#include <limits>
#include <numbers>
#include <optional>
#include <string>

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
        constexpr float ScaleBoxHalf = 5.0f;     // carré au bout d'un axe d'échelle
        constexpr float CenterHalf = 7.0f;       // carré du centre : échelle des trois axes
        constexpr float RingPixels = 90.0f;      // rayon des anneaux de rotation à l'écran
        constexpr int RingSegments = 48;
        constexpr double SnapRadians = std::numbers::pi / 12.0; // Ctrl pendant une rotation : pas de 15°
        constexpr float MinScale = 0.001f;       // comme l'inspecteur
        constexpr float MaxScale = 1.0e6f;
        constexpr float UniformScalePixels = 200.0f; // glisser de 200 pixels multiplie l'échelle par e
        constexpr Vector3d Axes[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

        // Boutons de mode, en haut à gauche de la vue.
        struct ToolbarButton
        {
            SceneViewport::GizmoMode mode;
            const char* label;
        };
        constexpr ToolbarButton ToolbarButtons[3] = {{SceneViewport::GizmoMode::Move, "Déplacer (W)"},
                                                     {SceneViewport::GizmoMode::Rotate, "Tourner (E)"},
                                                     {SceneViewport::GizmoMode::Scale, "Échelle (R)"}};
        constexpr float ToolbarTextSize = 14.0f;
        constexpr Vector2f ToolbarPadding{8.0f, 4.0f};
        constexpr Vector2f ToolbarMargin{8.0f, 8.0f};

        float& Component(Math::Vector3f& v, int axis) {
            return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
        }

        // Angle ramené dans [-pi, pi] : le tour de la souris autour du centre passe par ±pi sans sauter.
        float WrapAngle(float angle) {
            constexpr float Pi = std::numbers::pi_v<float>;
            while (angle > Pi) {
                angle -= 2.0f * Pi;
            }
            while (angle < -Pi) {
                angle += 2.0f * Pi;
            }
            return angle;
        }

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

        // Flèches du gizmo placé en origin (relatif à la caméra), le long des axes tournés par rotation ; nullopt s'il
        // est derrière la caméra.
        std::optional<std::array<Arrow, 3>> ProjectGizmo(const ScreenProjection& projection, const Vector3d& origin,
                                                         const Math::Quaternionf& rotation = {}) {
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
                if (!projection.ProjectSegment(origin, origin + rotation.Rotate(Axes[axis]) * length, a, b)) {
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

        // Anneau de rotation d'un axe du monde : cercle dans le plan perpendiculaire, découpé en segments à l'écran.
        struct Ring
        {
            std::array<std::pair<Vector2f, Vector2f>, RingSegments> segments;
            int count = 0;
        };

        std::array<Ring, 3> ProjectRings(const ScreenProjection& projection, const Vector3d& origin) {
            const double radius = projection.PixelsToMeters(RingPixels, projection.GetDepth(origin));
            std::array<Ring, 3> rings;
            for (int axis = 0; axis < 3; ++axis) {
                const Vector3d u = Axes[(axis + 1) % 3] * radius;
                const Vector3d v = Axes[(axis + 2) % 3] * radius;
                Vector3d previous = origin + u;
                for (int i = 1; i <= RingSegments; ++i) {
                    const double angle = 2.0 * std::numbers::pi * i / RingSegments;
                    const Vector3d next = origin + u * std::cos(angle) + v * std::sin(angle);
                    Vector2f a;
                    Vector2f b;
                    if (projection.ProjectSegment(previous, next, a, b)) {
                        rings[axis].segments[rings[axis].count++] = {a, b};
                    }
                    previous = next;
                }
            }
            return rings;
        }

        float DistanceToRing(Vector2f p, const Ring& ring) {
            float closest = std::numeric_limits<float>::max();
            for (int i = 0; i < ring.count; ++i) {
                closest = std::min(closest, DistanceToSegment(p, ring.segments[i].first, ring.segments[i].second));
            }
            return closest;
        }

        bool IsInSquare(Vector2f p, Vector2f center, float half) {
            return std::abs(p.x - center.x) <= half && std::abs(p.y - center.y) <= half;
        }

        // Les 12 arêtes du cube unité placé par transform.
        void DrawBoxEdges(UI::DrawList& list, const ScreenProjection& projection, const RelativeTransform& transform,
                          const Math::Box3f& box, Math::LinearColor color) {
            std::array<Vector3d, 8> corners;
            for (int i = 0; i < 8; ++i) {
                const Vector3d local{(i & 1 ? box.max.x : box.min.x) * transform.scale.x,
                                     (i & 2 ? box.max.y : box.min.y) * transform.scale.y,
                                     (i & 4 ? box.max.z : box.min.z) * transform.scale.z};
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

        // Point du repère de l'entité (échelle comprise) dans l'espace relatif à la caméra.
        Vector3d ToView(const RelativeTransform& transform, const Math::Vector3f& local) {
            const Vector3d scaled{local.x * transform.scale.x, local.y * transform.scale.y, local.z * transform.scale.z};
            return transform.position + transform.rotation.Rotate(scaled);
        }

        void DrawLocalLine(UI::DrawList& list, const ScreenProjection& projection, const RelativeTransform& transform,
                           const Math::Vector3f& a, const Math::Vector3f& b, Math::LinearColor color) {
            Vector2f screenA;
            Vector2f screenB;
            if (projection.ProjectSegment(ToView(transform, a), ToView(transform, b), screenA, screenB)) {
                list.AddLine(screenA, screenB, color, 1.5f);
            }
        }

        // Cercle (ou arc de first à last, en radians) de centre center dans le plan des axes u et v.
        void DrawLocalArc(UI::DrawList& list, const ScreenProjection& projection, const RelativeTransform& transform,
                          const Math::Vector3f& center, const Math::Vector3f& u, const Math::Vector3f& v, float first,
                          float last, Math::LinearColor color) {
            constexpr int Segments = 32;
            Math::Vector3f previous = center + u * std::cos(first) + v * std::sin(first);
            for (int i = 1; i <= Segments; ++i) {
                const float angle = first + (last - first) * static_cast<float>(i) / Segments;
                const Math::Vector3f next = center + u * std::cos(angle) + v * std::sin(angle);
                DrawLocalLine(list, projection, transform, previous, next, color);
                previous = next;
            }
        }

        // Capsule debout (axe y) : cylindre de demi-hauteur halfCylinder, extrémités de rayon radius.
        void DrawCapsule(UI::DrawList& list, const ScreenProjection& projection, const RelativeTransform& transform,
                         const Math::Vector3f& center, float radius, float halfCylinder, Math::LinearColor color) {
            constexpr float Pi = std::numbers::pi_v<float>;
            const Math::Vector3f top = center + Math::Vector3f{0.0f, halfCylinder, 0.0f};
            const Math::Vector3f bottom = center - Math::Vector3f{0.0f, halfCylinder, 0.0f};
            const Math::Vector3f x{radius, 0.0f, 0.0f};
            const Math::Vector3f y{0.0f, radius, 0.0f};
            const Math::Vector3f z{0.0f, 0.0f, radius};
            DrawLocalArc(list, projection, transform, top, x, z, 0.0f, 2.0f * Pi, color);
            DrawLocalArc(list, projection, transform, bottom, x, z, 0.0f, 2.0f * Pi, color);
            for (const Math::Vector3f& side : {x, -x, z, -z}) {
                DrawLocalLine(list, projection, transform, top + side, bottom + side, color);
            }
            DrawLocalArc(list, projection, transform, top, x, y, 0.0f, Pi, color);
            DrawLocalArc(list, projection, transform, top, z, y, 0.0f, Pi, color);
            DrawLocalArc(list, projection, transform, bottom, x, y, Pi, 2.0f * Pi, color);
            DrawLocalArc(list, projection, transform, bottom, z, y, Pi, 2.0f * Pi, color);
        }

        // Forme de collision (et capsule de personnage) de l'entité, en fil de fer.
        void DrawColliderEdges(UI::DrawList& list, const ScreenProjection& projection, const Scene& scene,
                               const RelativeTransform& transform, Math::LinearColor color) {
            if (const auto* character = scene.TryGet<CharacterComponent>(transform.entity)) {
                const float radius = character->radius;
                const float halfCylinder = std::max(character->height * 0.5f - radius, 0.0f);
                DrawCapsule(list, projection, transform, {0.0f, halfCylinder + radius, 0.0f}, radius, halfCylinder, color);
                return;
            }
            const auto* collider = scene.TryGet<ColliderComponent>(transform.entity);
            if (!collider) {
                return;
            }
            switch (collider->shape) {
                case ColliderShape::Box: {
                    const Math::Vector3f half = collider->size * 0.5f;
                    DrawBoxEdges(list, projection, transform, {collider->center - half, collider->center + half}, color);
                    break;
                }
                case ColliderShape::Sphere: {
                    const float r = collider->radius;
                    const Math::Vector3f c = collider->center;
                    constexpr float Turn = 2.0f * std::numbers::pi_v<float>;
                    DrawLocalArc(list, projection, transform, c, {r, 0, 0}, {0, r, 0}, 0.0f, Turn, color);
                    DrawLocalArc(list, projection, transform, c, {r, 0, 0}, {0, 0, r}, 0.0f, Turn, color);
                    DrawLocalArc(list, projection, transform, c, {0, r, 0}, {0, 0, r}, 0.0f, Turn, color);
                    break;
                }
                case ColliderShape::Capsule:
                    DrawCapsule(list, projection, transform, collider->center, collider->radius,
                                std::max(collider->height * 0.5f - collider->radius, 0.0f), color);
                    break;
            }
        }
    }

    void SceneViewport::UpdateScreenDrag(Engine& engine, Entity selected, const UI::Rect& view) {
        Scene& scene = engine.GetScene();
        const std::optional<ScreenUi::Offset> offset = selected == NullEntity ? std::nullopt : ScreenUi::FindOffset(scene, selected);
        if (!offset || scene.GetId(selected) != m_screenDrag.entity) {
            m_screenDrag.active = false; // élément supprimé, composant retiré ou sélection changée ailleurs
            return;
        }
        const Input& input = engine.GetWindow().GetInput();
        float x = m_screenDrag.startOffsetX;
        float y = m_screenDrag.startOffsetY;
        if (input.WasKeyPressed(Key::Escape) && !engine.GetUi().WantsKeyboard()) {
            m_screenDrag.active = false; // annulé : retour au point de départ
        } else {
            // Pixels de la vue ramenés en pixels de référence ; décalage arrondi, comme dans l'inspecteur.
            const float scale = ScreenUi::GetScale(view.min, view.max);
            const Vector2f moved = input.GetMousePosition() - m_screenDrag.startMouse;
            x = std::round(x + moved.x / scale);
            y = std::round(y + moved.y / scale);
            if (!input.IsMouseButtonDown(MouseButton::Left)) {
                m_screenDrag.active = false;
            }
        }
        if (x != *offset->x || y != *offset->y) {
            *offset->x = x;
            *offset->y = y;
            m_modified = true;
        }
    }

    // W, E, R : déplacer, tourner, mettre à l'échelle (par la lettre de la touche, comme Ctrl+Z). Pas pendant une
    // saisie, un raccourci Ctrl ou le vol libre (ZQSD).
    void SceneViewport::HandleShortcuts(Engine& engine) {
        const Input& input = engine.GetWindow().GetInput();
        if (engine.GetUi().WantsKeyboard() || input.IsControlDown() || engine.GetCameraController().IsActive()) {
            return;
        }
        if (input.WasLetterPressed('W')) {
            m_mode = GizmoMode::Move;
        } else if (input.WasLetterPressed('E')) {
            m_mode = GizmoMode::Rotate;
        } else if (input.WasLetterPressed('R')) {
            m_mode = GizmoMode::Scale;
        }
    }

    namespace {
        // Rectangle du bouton index de la barre de modes.
        UI::Rect ToolbarRect(const UI::DrawList& list, const UI::Rect& view, int index) {
            Vector2f min = view.min + ToolbarMargin;
            for (int i = 0;; ++i) {
                const Vector2f size = list.MeasureText(ToolbarButtons[i].label, ToolbarTextSize) + ToolbarPadding * 2.0f;
                if (i == index) {
                    return {min, min + size};
                }
                min.x += size.x + 2.0f;
            }
        }
    }

    bool SceneViewport::UpdateToolbar(Engine& engine, const UI::Rect& view, bool mouseInView) {
        const Input& input = engine.GetWindow().GetInput();
        const Vector2f mouse = input.GetMousePosition();
        const UI::DrawList& list = engine.GetRenderer().GetOverlay();
        m_hoveredButton = -1;
        for (int i = 0; mouseInView && i < 3; ++i) {
            const UI::Rect rect = ToolbarRect(list, view, i);
            if (mouse.x >= rect.min.x && mouse.x < rect.max.x && mouse.y >= rect.min.y && mouse.y < rect.max.y) {
                m_hoveredButton = i;
            }
        }
        if (m_hoveredButton >= 0 && input.WasMouseButtonPressed(MouseButton::Left)) {
            m_mode = ToolbarButtons[m_hoveredButton].mode;
            return true;
        }
        return false;
    }

    void SceneViewport::DrawToolbar(Engine& engine, const UI::Rect& view) {
        UI::DrawList& list = engine.GetRenderer().GetOverlay();
        const UI::Style& style = engine.GetUi().GetStyle();
        list.SetClipRect(view.min, view.max);
        for (int i = 0; i < 3; ++i) {
            const UI::Rect rect = ToolbarRect(list, view, i);
            const bool current = ToolbarButtons[i].mode == m_mode;
            list.AddRect(rect.min, rect.max, current ? style.buttonActive : i == m_hoveredButton ? style.buttonHovered : style.button);
            list.AddText(rect.min + ToolbarPadding, ToolbarButtons[i].label, ToolbarTextSize, style.text);
        }
        list.ResetClipRect();
    }

    void SceneViewport::UpdateDrag(Engine& engine, Entity selected, RelativeTransform& selectedTransform,
                                   const RelativeTransform* parentTransform, const Ray& ray,
                                   const ScreenProjection& projection) {
        Scene& scene = engine.GetScene();
        const Input& input = engine.GetWindow().GetInput();
        TransformComponent& local = scene.Get<TransformComponent>(selected);
        const auto apply = [&](const Vector3d& position, const Math::Quaternionf& rotation, const Math::Vector3f& scale) {
            if (position != local.position || !(rotation == local.rotation) || scale != local.scale) {
                local.position = position;
                local.rotation = rotation;
                local.scale = scale;
                m_modified = true;
            }
        };
        if (input.WasKeyPressed(Key::Escape) && !engine.GetUi().WantsKeyboard()) {
            // Annulé : retour à l'état de départ.
            apply(m_drag.startLocalPosition, m_drag.startLocalRotation, m_drag.startLocalScale);
            m_drag.axis = -1;
            return;
        }
        // Origine du gizmo au départ, vue depuis la caméra actuelle.
        const Vector3d startPoint = m_drag.startPoint + Math::Delta(engine.GetCamera().position, m_drag.startCamera);
        const Vector2f mouse = input.GetMousePosition();
        switch (m_drag.mode) {
            case GizmoMode::Move: {
                // Le point de l'axe saisi suit le point de l'axe le plus proche de la souris.
                const Vector3d axis = Axes[m_drag.axis];
                Vector3d move{};
                if (const std::optional<double> t = ClosestPointOnLine(ray, startPoint, axis)) {
                    move = axis * (*t - m_drag.startParameter);
                }
                // Déplacement dans le monde ramené dans l'espace du parent (sa rotation et son échelle).
                Vector3d localMove = move;
                if (parentTransform) {
                    const Vector3d scale(parentTransform->scale);
                    const Vector3d rotated = parentTransform->rotation.Conjugate().Rotate(move);
                    localMove = {scale.x != 0.0 ? rotated.x / scale.x : 0.0, scale.y != 0.0 ? rotated.y / scale.y : 0.0,
                                 scale.z != 0.0 ? rotated.z / scale.z : 0.0};
                }
                apply(m_drag.startLocalPosition + localMove, local.rotation, local.scale);
                selectedTransform.position = startPoint + move; // pour dessiner le gizmo sans décalage d'une frame
                break;
            }
            case GizmoMode::Rotate: {
                // L'angle de la souris autour du centre à l'écran, cumulé (plusieurs tours possibles).
                if (const std::optional<Vector2f> center = projection.ToScreen(startPoint)) {
                    const float angle = std::atan2(mouse.y - center->y, mouse.x - center->x);
                    m_drag.turned += WrapAngle(angle - m_drag.lastAngle);
                    m_drag.lastAngle = angle;
                }
                // Vu depuis la pointe de l'axe, un angle positif tourne dans le sens inverse des aiguilles d'une
                // montre ; à l'écran (y vers le bas), c'est un angle de souris qui diminue.
                const Vector3d axis = Axes[m_drag.axis];
                const bool facing = Math::Dot(axis, -startPoint) > 0.0;
                double turned = facing ? -m_drag.turned : m_drag.turned;
                if (input.IsControlDown()) {
                    turned = std::round(turned / SnapRadians) * SnapRadians;
                }
                m_drag.applied = static_cast<float>(turned);
                // Rotation autour de l'axe du monde, ramenée dans l'espace du parent : local = P⁻¹ R P L.
                const Math::Quaternionf world = Math::Quaternionf::FromAxisAngle(Math::Vector3f(axis), static_cast<float>(turned));
                const Math::Quaternionf& parent = m_drag.parentRotation;
                apply(local.position, Math::Normalize(parent.Conjugate() * world * parent * m_drag.startLocalRotation),
                      local.scale);
                selectedTransform.rotation = Math::Normalize(world * m_drag.startRotation);
                break;
            }
            case GizmoMode::Scale: {
                Math::Vector3f scale = m_drag.startLocalScale;
                if (m_drag.axis == 3) {
                    // Carré du centre : vers la droite ou le haut, plus grand ; les trois axes ensemble.
                    const float pixels = (mouse.x - m_drag.startMouse.x) - (mouse.y - m_drag.startMouse.y);
                    scale = scale * std::exp(pixels / UniformScalePixels);
                } else {
                    // Axe de l'objet : le point saisi suit la souris le long de l'axe, l'échelle dans la même proportion.
                    const Vector3d axis = m_drag.startRotation.Rotate(Axes[m_drag.axis]);
                    if (const std::optional<double> t = ClosestPointOnLine(ray, startPoint, axis)) {
                        Component(scale, m_drag.axis) *= static_cast<float>(*t / m_drag.startParameter);
                    }
                }
                for (int axis = 0; axis < 3; ++axis) {
                    Component(scale, axis) = std::clamp(Component(scale, axis), MinScale, MaxScale);
                }
                // Échelle dans le monde : celle du parent fois la locale ; mise à jour pour le contour de cette frame.
                const Math::Vector3f parentScale = parentTransform ? parentTransform->scale : Math::Vector3f{1.0f, 1.0f, 1.0f};
                selectedTransform.scale = {parentScale.x * scale.x, parentScale.y * scale.y, parentScale.z * scale.z};
                apply(local.position, local.rotation, scale);
                break;
            }
        }
        if (!input.IsMouseButtonDown(MouseButton::Left)) {
            m_drag.axis = -1;
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
            m_screenDrag.active = false;
            m_screenRects.clear();
            return; // vue écrasée par les panneaux
        }
        if (engine.GetGameState() != GameState::Stopped) {
            // Mode Jouer : la vue montre le jeu tel quel, sans sélection ni gizmo ; le clic appartient au jeu.
            m_drag.axis = -1;
            m_screenDrag.active = false;
            m_hoveredAxis = -1;
            m_screenRects.clear();
            ScreenUi::Draw(scene, engine.GetRenderer().GetOverlay(), view.min, view.max,
                           {.textures = [&engine](std::string_view path) { return engine.LoadAssetTexture(path); }});
            return;
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
        // Un élément à l'écran n'est pas dans le monde : pas de gizmo, il se glisse directement.
        const bool selectedIsScreen = selected != NullEntity && ScreenUi::IsScreenElement(scene, selected);

        if (m_screenDrag.active) {
            UpdateScreenDrag(engine, selected, view);
        }

        const RelativeTransform* parentTransform = nullptr;
        if (selected != NullEntity) {
            if (const Entity parent = scene.GetParent(selected); parent != NullEntity) {
                parentTransform = findTransform(parent);
            }
        }
        if (m_drag.axis >= 0) {
            if (!selectedTransform || scene.GetId(selected) != m_drag.entity) {
                m_drag.axis = -1; // entité supprimée ou sélection changée ailleurs
            } else {
                UpdateDrag(engine, selected, *selectedTransform, parentTransform, ray, projection);
            }
        }

        // La souris est à la vue : pas de panneau survolé ni de widget en main, pas de vol libre en cours.
        const bool mouseInView = mouse.x >= view.min.x && mouse.x < view.max.x && mouse.y >= view.min.y
                              && mouse.y < view.max.y && !ui.WantsMouse() && !engine.GetWindow().IsCursorLocked()
                              && !engine.GetCameraController().IsActive();
        if (m_drag.axis < 0) {
            HandleShortcuts(engine);
        }
        const bool toolbarClicked = m_drag.axis < 0 && !m_screenDrag.active && UpdateToolbar(engine, view, mouseInView);

        // Poignées du gizmo à l'écran, selon le mode : flèches (déplacement, axes du monde), axes de l'objet
        // (échelle) ou anneaux (rotation, axes du monde).
        const bool showGizmo = selectedTransform && !selectedIsScreen;
        const std::optional<std::array<Arrow, 3>> arrows =
            showGizmo && m_mode != GizmoMode::Rotate
                ? ProjectGizmo(projection, selectedTransform->position,
                               m_mode == GizmoMode::Scale ? selectedTransform->rotation : Math::Quaternionf{})
                : std::nullopt;
        const std::optional<Vector2f> center = showGizmo ? projection.ToScreen(selectedTransform->position) : std::nullopt;
        std::array<Ring, 3> rings{};
        if (showGizmo && m_mode == GizmoMode::Rotate && center) {
            rings = ProjectRings(projection, selectedTransform->position);
        }

        if (m_drag.axis < 0 && !m_screenDrag.active && !toolbarClicked) {
            m_hoveredAxis = -1;
            if (mouseInView && center) {
                float closest = GrabPixels;
                if (m_mode == GizmoMode::Rotate) {
                    for (int axis = 0; axis < 3; ++axis) {
                        if (const float distance = DistanceToRing(mouse, rings[axis]); distance < closest) {
                            closest = distance;
                            m_hoveredAxis = axis;
                        }
                    }
                } else if (m_mode == GizmoMode::Scale && IsInSquare(mouse, *center, CenterHalf)) {
                    m_hoveredAxis = 3; // le centre passe avant les axes qui en partent
                } else if (arrows) {
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
            }
            if (mouseInView && input.WasMouseButtonPressed(MouseButton::Left)) {
                bool grabbed = false;
                if (m_hoveredAxis >= 0) {
                    const TransformComponent& local = scene.Get<TransformComponent>(selected);
                    Drag drag;
                    drag.axis = m_hoveredAxis;
                    drag.mode = m_mode;
                    drag.entity = scene.GetId(selected);
                    drag.startLocalPosition = local.position;
                    drag.startLocalRotation = local.rotation;
                    drag.startLocalScale = local.scale;
                    drag.startRotation = selectedTransform->rotation;
                    drag.parentRotation = parentTransform ? parentTransform->rotation : Math::Quaternionf{};
                    drag.startPoint = selectedTransform->position;
                    drag.startCamera = camera.position;
                    drag.startMouse = mouse;
                    grabbed = true;
                    if (m_mode == GizmoMode::Rotate) {
                        drag.lastAngle = std::atan2(mouse.y - center->y, mouse.x - center->x);
                    } else if (m_hoveredAxis < 3) {
                        const Vector3d axis = m_mode == GizmoMode::Scale ? selectedTransform->rotation.Rotate(Axes[m_hoveredAxis])
                                                                         : Axes[m_hoveredAxis];
                        const std::optional<double> t = ClosestPointOnLine(ray, selectedTransform->position, axis);
                        // Échelle : le point saisi sert de référence (facteur = distance actuelle / distance saisie).
                        grabbed = t && (m_mode == GizmoMode::Move || std::abs(*t) > 1.0e-9);
                        drag.startParameter = t.value_or(0.0);
                    }
                    if (grabbed) {
                        m_drag = drag;
                    }
                }
                // Élément à l'écran sous la souris (le dernier dessiné est au-dessus) : il passe avant les objets 3D
                // qu'il recouvre.
                const auto item = std::ranges::find_if(m_screenRects.rbegin(), m_screenRects.rend(), [&](const auto& rect) {
                    return scene.IsValid(rect.entity) && mouse.x >= rect.min.x && mouse.x < rect.max.x
                        && mouse.y >= rect.min.y && mouse.y < rect.max.y;
                });
                if (grabbed) {
                    // La poignée est saisie : ni sélection, ni élément à l'écran.
                } else if (item != m_screenRects.rend() && ScreenUi::IsScreenElement(scene, item->entity)) {
                    sceneEditor.Select(scene, item->entity);
                    // Une image étirée ou qui couvre l'écran se sélectionne mais ne se déplace pas.
                    if (const std::optional<ScreenUi::Offset> offset = ScreenUi::FindOffset(scene, item->entity)) {
                        m_screenDrag = {true, scene.GetId(item->entity), *offset->x, *offset->y, mouse};
                    }
                } else {
                    // Clic dans le vide : désélectionne.
                    sceneEditor.Select(scene, PickEntity(m_transforms, ray, [&engine](Entity entity) {
                        return engine.GetMeshBounds(entity);
                    }));
                }
            }
        }

        // Dessin, sous les panneaux (l'interface est ajoutée à l'overlay après) et découpé à la vue :
        // les textes et images de la scène comme dans le jeu, puis la sélection par-dessus.
        UI::DrawList& list = engine.GetRenderer().GetOverlay();
        m_screenRects.clear();
        ScreenUi::Draw(scene, list, view.min, view.max,
                       {.textures = [&engine](std::string_view path) { return engine.LoadAssetTexture(path); },
                        .rects = &m_screenRects,
                        .placeholders = true});
        DrawToolbar(engine, view);
        if (!selectedTransform) {
            return;
        }
        list.SetClipRect(view.min, view.max);
        if (selectedIsScreen) {
            // Le dernier rectangle de l'entité : son texte s'il y en a un (dessiné après l'image).
            const auto rect = std::ranges::find(m_screenRects.rbegin(), m_screenRects.rend(), selected, &ScreenUi::ItemRect::entity);
            if (rect != m_screenRects.rend()) {
                const Vector2f margin{4.0f, 2.0f};
                list.AddRectOutline(rect->min - margin, rect->max + margin, Math::LinearColor::FromSrgb8(255, 165, 40), 2.0f);
            }
        }
        if (const std::optional<Math::Box3f> box = engine.GetMeshBounds(selected)) {
            DrawBoxEdges(list, projection, *selectedTransform, *box, Math::LinearColor::FromSrgb8(255, 165, 40));
        }
        // La collision en vert, par-dessus : on voit si elle épouse l'objet.
        DrawColliderEdges(list, projection, scene, *selectedTransform, Math::LinearColor::FromSrgb8(80, 220, 120));
        if (center) {
            const UI::Style& style = ui.GetStyle();
            const Math::LinearColor colors[3] = {style.axisX, style.axisY, style.axisZ};
            const Math::LinearColor highlight = Math::LinearColor::FromSrgb8(255, 220, 60);
            const Math::LinearColor white = Math::LinearColor::FromSrgb8(235, 235, 235);
            const int active = m_drag.axis >= 0 ? m_drag.axis : m_hoveredAxis;
            // Pendant un glissement, seule la poignée saisie reste.
            const auto shown = [&](int axis) { return m_drag.axis < 0 || axis == m_drag.axis; };
            if (m_mode == GizmoMode::Rotate) {
                for (int axis = 0; axis < 3; ++axis) {
                    if (shown(axis)) {
                        const Math::LinearColor color = axis == active ? highlight : colors[axis];
                        for (int i = 0; i < rings[axis].count; ++i) {
                            list.AddLine(rings[axis].segments[i].first, rings[axis].segments[i].second, color, 2.5f);
                        }
                    }
                }
                if (m_drag.axis >= 0) {
                    // Angle tourné, près de la souris, et le rayon du centre vers elle.
                    list.AddLine(*center, mouse, highlight, 1.0f);
                    const double degrees = m_drag.applied * 180.0 / std::numbers::pi;
                    list.AddText(mouse + Vector2f{16.0f, 8.0f}, std::to_string(static_cast<int>(std::lround(degrees))) + "°",
                                 ToolbarTextSize, white);
                }
            } else if (arrows) {
                for (int axis = 0; axis < 3; ++axis) {
                    const Arrow& arrow = (*arrows)[axis];
                    if (!arrow.visible || !shown(axis)) {
                        continue;
                    }
                    const Math::LinearColor color = axis == active ? highlight : colors[axis];
                    list.AddLine(arrow.start, arrow.end, color, 3.0f);
                    if (m_mode == GizmoMode::Move) {
                        list.AddTriangle(arrow.tip, arrow.end + arrow.normal, arrow.end - arrow.normal, color);
                    } else {
                        const Vector2f half{ScaleBoxHalf, ScaleBoxHalf};
                        list.AddRect(arrow.end - half, arrow.end + half, color);
                    }
                }
            }
            if (m_mode == GizmoMode::Scale && shown(3)) {
                const Vector2f half{CenterHalf, CenterHalf};
                list.AddRectOutline(*center - half, *center + half, active == 3 ? highlight : white, 2.0f);
            } else {
                list.AddRect(*center - Vector2f{3.0f, 3.0f}, *center + Vector2f{3.0f, 3.0f}, white);
            }
        }
        list.ResetClipRect();
    }
}
