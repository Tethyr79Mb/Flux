// Pictogrammes de l'éditeur : rectangles, triangles et traits seulement, dans un carré unité mis à l'échelle.
#include "Flux/UI/Icons.hpp"

#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::UI {
    using Math::LinearColor;
    using Math::Vector2f;

    namespace {
        // Coordonnées dans le carré unité (0,0 en haut à gauche) vers les pixels.
        struct Pen
        {
            DrawList& list;
            Vector2f min;
            float size;

            Vector2f At(float x, float y) const { return {min.x + x * size, min.y + y * size}; }
            void Rect(float x0, float y0, float x1, float y1, LinearColor color) const { list.AddRect(At(x0, y0), At(x1, y1), color); }
            void Triangle(Vector2f a, Vector2f b, Vector2f c, LinearColor color) const {
                list.AddTriangle(At(a.x, a.y), At(b.x, b.y), At(c.x, c.y), color);
            }
            void Quad(Vector2f a, Vector2f b, Vector2f c, Vector2f d, LinearColor color) const {
                Triangle(a, b, c, color);
                Triangle(a, c, d, color);
            }
            void Line(float x0, float y0, float x1, float y1, LinearColor color, float width) const {
                list.AddLine(At(x0, y0), At(x1, y1), color, std::max(1.0f, width * size));
            }
            void Disc(float x, float y, float radius, LinearColor color) const {
                constexpr int Segments = 12;
                for (int i = 0; i < Segments; ++i) {
                    const float a0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / Segments;
                    const float a1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i + 1) / Segments;
                    Triangle({x, y}, {x + radius * std::cos(a0), y + radius * std::sin(a0)},
                             {x + radius * std::cos(a1), y + radius * std::sin(a1)}, color);
                }
            }
        };

        LinearColor Shade(LinearColor color, float factor) {
            return {color.r * factor, color.g * factor, color.b * factor, color.a};
        }

        // Cube isométrique : dessus clair, gauche moyenne, droite sombre.
        void DrawCube(const Pen& pen, LinearColor color) {
            const Vector2f top{0.5f, 0.08f}, left{0.1f, 0.3f}, right{0.9f, 0.3f}, center{0.5f, 0.52f};
            const Vector2f bottom{0.5f, 0.94f}, bottomLeft{0.1f, 0.72f}, bottomRight{0.9f, 0.72f};
            pen.Quad(top, right, center, left, Shade(color, 1.15f));
            pen.Quad(left, center, bottom, bottomLeft, color);
            pen.Quad(center, right, bottomRight, bottom, Shade(color, 0.7f));
        }
    }

    void DrawIcon(DrawList& list, Icon icon, Vector2f min, float size) {
        const Pen pen{list, min, size};
        switch (icon) {
            case Icon::None:
                break;
            case Icon::Folder: {
                const LinearColor back = LinearColor::FromSrgb8(196, 150, 58);
                pen.Rect(0.0f, 0.12f, 0.42f, 0.28f, back);
                pen.Triangle({0.42f, 0.12f}, {0.42f, 0.28f}, {0.53f, 0.28f}, back);
                pen.Rect(0.0f, 0.22f, 1.0f, 0.9f, back);
                pen.Rect(0.0f, 0.36f, 1.0f, 0.9f, LinearColor::FromSrgb8(232, 186, 82));
                break;
            }
            case Icon::File: {
                const LinearColor paper = LinearColor::FromSrgb8(214, 218, 226);
                pen.Rect(0.12f, 0.0f, 0.62f, 0.26f, paper);
                pen.Rect(0.12f, 0.26f, 0.88f, 1.0f, paper);
                pen.Triangle({0.62f, 0.0f}, {0.62f, 0.26f}, {0.88f, 0.26f}, Shade(paper, 0.65f));
                break;
            }
            case Icon::Scene: {
                // Clap de cinéma : planche rayée inclinée au-dessus, corps sombre.
                const LinearColor body = LinearColor::FromSrgb8(58, 64, 78);
                const LinearColor light = LinearColor::FromSrgb8(226, 230, 238);
                pen.Rect(0.06f, 0.4f, 0.94f, 0.92f, body);
                pen.Rect(0.06f, 0.4f, 0.94f, 0.52f, light);
                for (int i = 0; i < 4; ++i) {
                    const float x = 0.1f + 0.22f * static_cast<float>(i);
                    pen.Quad({x, 0.4f}, {x + 0.1f, 0.4f}, {x + 0.16f, 0.52f}, {x + 0.06f, 0.52f}, body);
                }
                pen.Quad({0.04f, 0.26f}, {0.9f, 0.06f}, {0.93f, 0.18f}, {0.07f, 0.38f}, light);
                for (int i = 0; i < 4; ++i) {
                    const float t = 0.08f + 0.22f * static_cast<float>(i);
                    const Vector2f a{0.04f + 0.86f * t, 0.26f - 0.2f * t};
                    const Vector2f b{0.04f + 0.86f * (t + 0.1f), 0.26f - 0.2f * (t + 0.1f)};
                    pen.Quad(a, b, {b.x + 0.03f, b.y + 0.12f}, {a.x + 0.03f, a.y + 0.12f}, body);
                }
                break;
            }
            case Icon::Cube:
                DrawCube(pen, LinearColor::FromSrgb8(120, 150, 196));
                break;
            case Icon::Model:
                DrawCube(pen, LinearColor::FromSrgb8(222, 138, 64));
                break;
            case Icon::Character: {
                const LinearColor color = LinearColor::FromSrgb8(110, 196, 120);
                pen.Disc(0.5f, 0.24f, 0.18f, color);
                pen.Quad({0.24f, 0.48f}, {0.76f, 0.48f}, {0.86f, 0.96f}, {0.14f, 0.96f}, color);
                break;
            }
            case Icon::Ship: {
                // Vue de dessus : nez en haut, ailes en flèche, échancrure à l'arrière.
                const LinearColor color = LinearColor::FromSrgb8(96, 192, 214);
                pen.Triangle({0.5f, 0.04f}, {0.96f, 0.86f}, {0.5f, 0.68f}, color);
                pen.Triangle({0.5f, 0.04f}, {0.5f, 0.68f}, {0.04f, 0.86f}, Shade(color, 0.8f));
                pen.Triangle({0.5f, 0.68f}, {0.66f, 0.96f}, {0.34f, 0.96f}, Shade(color, 0.6f));
                break;
            }
            case Icon::Collider: {
                const LinearColor color = LinearColor::FromSrgb8(170, 214, 90);
                const float w = 0.07f;
                pen.Line(0.1f, 0.3f, 0.5f, 0.1f, color, w);
                pen.Line(0.5f, 0.1f, 0.9f, 0.3f, color, w);
                pen.Line(0.9f, 0.3f, 0.5f, 0.5f, color, w);
                pen.Line(0.5f, 0.5f, 0.1f, 0.3f, color, w);
                pen.Line(0.1f, 0.3f, 0.1f, 0.72f, color, w);
                pen.Line(0.9f, 0.3f, 0.9f, 0.72f, color, w);
                pen.Line(0.5f, 0.5f, 0.5f, 0.92f, color, w);
                pen.Line(0.1f, 0.72f, 0.5f, 0.92f, color, w);
                pen.Line(0.5f, 0.92f, 0.9f, 0.72f, color, w);
                break;
            }
            case Icon::Seat: {
                const LinearColor color = LinearColor::FromSrgb8(176, 128, 96);
                pen.Rect(0.18f, 0.06f, 0.36f, 0.62f, color);                  // dossier
                pen.Rect(0.18f, 0.5f, 0.84f, 0.64f, Shade(color, 1.15f));    // assise
                pen.Rect(0.22f, 0.64f, 0.32f, 0.96f, Shade(color, 0.7f));    // pieds
                pen.Rect(0.7f, 0.64f, 0.8f, 0.96f, Shade(color, 0.7f));
                break;
            }
            case Icon::Text: {
                const LinearColor color = LinearColor::FromSrgb8(230, 230, 236);
                pen.Rect(0.12f, 0.1f, 0.88f, 0.26f, color);
                pen.Rect(0.42f, 0.1f, 0.58f, 0.92f, color);
                break;
            }
            case Icon::Image: {
                pen.Rect(0.04f, 0.14f, 0.96f, 0.86f, LinearColor::FromSrgb8(214, 218, 226));
                pen.Rect(0.1f, 0.2f, 0.9f, 0.8f, LinearColor::FromSrgb8(80, 130, 196));
                pen.Disc(0.7f, 0.36f, 0.08f, LinearColor::FromSrgb8(240, 210, 90));
                pen.Triangle({0.1f, 0.8f}, {0.38f, 0.42f}, {0.62f, 0.8f}, LinearColor::FromSrgb8(70, 150, 90));
                pen.Triangle({0.42f, 0.8f}, {0.64f, 0.54f}, {0.9f, 0.8f}, LinearColor::FromSrgb8(56, 120, 72));
                break;
            }
            case Icon::SceneChange: {
                const LinearColor color = LinearColor::FromSrgb8(176, 120, 220);
                pen.Rect(0.08f, 0.38f, 0.56f, 0.62f, color);
                pen.Triangle({0.52f, 0.14f}, {0.94f, 0.5f}, {0.52f, 0.86f}, color);
                break;
            }
            case Icon::Light: {
                const LinearColor glass = LinearColor::FromSrgb8(250, 214, 90);
                pen.Disc(0.5f, 0.4f, 0.32f, glass);
                pen.Rect(0.36f, 0.56f, 0.64f, 0.74f, glass);                           // col de l'ampoule
                pen.Rect(0.34f, 0.74f, 0.66f, 0.84f, LinearColor::FromSrgb8(170, 172, 180)); // culot
                pen.Rect(0.4f, 0.84f, 0.6f, 0.94f, LinearColor::FromSrgb8(120, 122, 130));
                pen.Disc(0.4f, 0.3f, 0.08f, LinearColor::FromSrgb8(255, 248, 210));     // reflet
                break;
            }
            case Icon::Sound: {
                const LinearColor body = LinearColor::FromSrgb8(120, 200, 230);
                pen.Rect(0.1f, 0.38f, 0.3f, 0.62f, body);                         // aimant
                pen.Quad({0.3f, 0.38f}, {0.56f, 0.14f}, {0.56f, 0.86f}, {0.3f, 0.62f}, body); // pavillon
                const LinearColor wave = LinearColor::FromSrgb8(200, 236, 250);
                pen.Line(0.68f, 0.36f, 0.74f, 0.5f, wave, 0.07f);
                pen.Line(0.74f, 0.5f, 0.68f, 0.64f, wave, 0.07f);
                pen.Line(0.8f, 0.22f, 0.9f, 0.5f, wave, 0.07f);
                pen.Line(0.9f, 0.5f, 0.8f, 0.78f, wave, 0.07f);
                break;
            }
            case Icon::Sky: {
                const LinearColor night = LinearColor::FromSrgb8(40, 52, 110);
                pen.Disc(0.5f, 0.5f, 0.44f, night);
                pen.Disc(0.4f, 0.42f, 0.2f, LinearColor::FromSrgb8(240, 230, 190)); // lune
                pen.Disc(0.48f, 0.36f, 0.18f, night);                               // son croissant
                const LinearColor star = LinearColor::FromSrgb8(255, 255, 240);
                pen.Disc(0.7f, 0.32f, 0.05f, star);
                pen.Disc(0.64f, 0.66f, 0.04f, star);
                pen.Disc(0.36f, 0.74f, 0.035f, star);
                break;
            }
            case Icon::Component: {
                const LinearColor color = LinearColor::FromSrgb8(200, 200, 120);
                pen.Quad({0.5f, 0.06f}, {0.94f, 0.5f}, {0.5f, 0.94f}, {0.06f, 0.5f}, color);
                pen.Quad({0.5f, 0.32f}, {0.68f, 0.5f}, {0.5f, 0.68f}, {0.32f, 0.5f}, Shade(color, 0.5f));
                break;
            }
            case Icon::Empty: {
                const LinearColor color = LinearColor::FromSrgb8(140, 144, 156);
                const float w = 0.08f;
                pen.Line(0.2f, 0.2f, 0.8f, 0.2f, color, w);
                pen.Line(0.8f, 0.2f, 0.8f, 0.8f, color, w);
                pen.Line(0.8f, 0.8f, 0.2f, 0.8f, color, w);
                pen.Line(0.2f, 0.8f, 0.2f, 0.2f, color, w);
                break;
            }
            case Icon::Window: {
                pen.Rect(0.04f, 0.12f, 0.96f, 0.9f, LinearColor::FromSrgb8(60, 190, 220));
                pen.Rect(0.1f, 0.32f, 0.9f, 0.84f, LinearColor::FromSrgb8(28, 38, 54));
                pen.Rect(0.76f, 0.17f, 0.88f, 0.27f, LinearColor::FromSrgb8(230, 240, 246)); // croix de fermeture
                break;
            }
            case Icon::Frame: {
                const LinearColor color = LinearColor::FromSrgb8(120, 170, 200);
                pen.Rect(0.08f, 0.16f, 0.92f, 0.84f, color);
                pen.Rect(0.16f, 0.24f, 0.84f, 0.76f, Shade(color, 0.45f));
                break;
            }
            case Icon::Button: {
                const LinearColor color = LinearColor::FromSrgb8(70, 130, 190);
                pen.Rect(0.04f, 0.28f, 0.96f, 0.72f, color);
                pen.Rect(0.04f, 0.64f, 0.96f, 0.72f, Shade(color, 0.6f));
                pen.Rect(0.3f, 0.45f, 0.7f, 0.53f, LinearColor::FromSrgb8(235, 240, 246));
                break;
            }
            case Icon::Checkbox: {
                pen.Rect(0.12f, 0.12f, 0.88f, 0.88f, LinearColor::FromSrgb8(214, 218, 226));
                pen.Rect(0.2f, 0.2f, 0.8f, 0.8f, LinearColor::FromSrgb8(40, 52, 70));
                const LinearColor check = LinearColor::FromSrgb8(60, 200, 230);
                pen.Line(0.28f, 0.5f, 0.45f, 0.68f, check, 0.12f);
                pen.Line(0.45f, 0.68f, 0.74f, 0.3f, check, 0.12f);
                break;
            }
            case Icon::Slider: {
                pen.Rect(0.06f, 0.44f, 0.94f, 0.56f, LinearColor::FromSrgb8(110, 116, 130));
                pen.Rect(0.06f, 0.44f, 0.58f, 0.56f, LinearColor::FromSrgb8(60, 200, 230));
                pen.Disc(0.58f, 0.5f, 0.16f, LinearColor::FromSrgb8(232, 238, 244));
                break;
            }
            case Icon::ProgressBar:
                pen.Rect(0.04f, 0.34f, 0.96f, 0.66f, LinearColor::FromSrgb8(60, 74, 92));
                pen.Rect(0.04f, 0.34f, 0.66f, 0.66f, LinearColor::FromSrgb8(60, 200, 230));
                break;
            case Icon::Tabs: {
                const LinearColor page = LinearColor::FromSrgb8(40, 70, 100);
                pen.Rect(0.06f, 0.4f, 0.94f, 0.9f, page);
                pen.Rect(0.06f, 0.14f, 0.34f, 0.4f, page); // onglet choisi, du même bleu que la page
                pen.Rect(0.06f, 0.34f, 0.34f, 0.4f, LinearColor::FromSrgb8(60, 200, 230));
                pen.Rect(0.38f, 0.18f, 0.64f, 0.36f, LinearColor::FromSrgb8(110, 124, 142));
                pen.Rect(0.68f, 0.18f, 0.94f, 0.36f, LinearColor::FromSrgb8(110, 124, 142));
                break;
            }
            case Icon::ScrollArea: {
                const LinearColor line = LinearColor::FromSrgb8(150, 170, 190);
                pen.Rect(0.06f, 0.08f, 0.94f, 0.92f, LinearColor::FromSrgb8(40, 52, 70));
                pen.Rect(0.14f, 0.2f, 0.66f, 0.3f, line);
                pen.Rect(0.14f, 0.45f, 0.66f, 0.55f, line);
                pen.Rect(0.14f, 0.7f, 0.66f, 0.8f, line);
                pen.Rect(0.76f, 0.14f, 0.88f, 0.86f, LinearColor::FromSrgb8(70, 84, 104));
                pen.Rect(0.76f, 0.22f, 0.88f, 0.5f, LinearColor::FromSrgb8(60, 200, 230)); // poignée
                break;
            }
            case Icon::Play:
                pen.Triangle({0.22f, 0.1f}, {0.9f, 0.5f}, {0.22f, 0.9f}, LinearColor::FromSrgb8(88, 200, 110));
                break;
            case Icon::Pause: {
                const LinearColor color = LinearColor::FromSrgb8(150, 158, 172); // visible sur fond clair et sombre
                pen.Rect(0.2f, 0.14f, 0.4f, 0.86f, color);
                pen.Rect(0.6f, 0.14f, 0.8f, 0.86f, color);
                break;
            }
            case Icon::Stop:
                pen.Rect(0.18f, 0.18f, 0.82f, 0.82f, LinearColor::FromSrgb8(226, 84, 76));
                break;
            case Icon::Plus: {
                const LinearColor color = LinearColor::FromSrgb8(88, 200, 110);
                pen.Rect(0.42f, 0.12f, 0.58f, 0.88f, color);
                pen.Rect(0.12f, 0.42f, 0.88f, 0.58f, color);
                break;
            }
            case Icon::Camera: {
                const LinearColor body = LinearColor::FromSrgb8(150, 160, 178);
                pen.Rect(0.06f, 0.3f, 0.66f, 0.78f, body);
                pen.Quad({0.66f, 0.46f}, {0.94f, 0.3f}, {0.94f, 0.78f}, {0.66f, 0.62f}, Shade(body, 0.8f));
                pen.Disc(0.22f, 0.2f, 0.12f, Shade(body, 0.9f));
                pen.Disc(0.48f, 0.2f, 0.12f, Shade(body, 0.9f));
                break;
            }
            case Icon::Inspector: {
                const LinearColor line = LinearColor::FromSrgb8(150, 160, 178);
                const LinearColor knob = LinearColor::FromSrgb8(88, 156, 255);
                for (int i = 0; i < 3; ++i) {
                    const float y = 0.2f + 0.3f * static_cast<float>(i);
                    pen.Rect(0.08f, y - 0.04f, 0.92f, y + 0.04f, line);
                    const float x = i == 1 ? 0.66f : i == 0 ? 0.32f : 0.48f;
                    pen.Rect(x - 0.08f, y - 0.11f, x + 0.08f, y + 0.11f, knob);
                }
                break;
            }
            case Icon::Place: {
                DrawCube(Pen{list, min + Vector2f{0.0f, size * 0.12f}, size * 0.72f}, LinearColor::FromSrgb8(90, 140, 220));
                const LinearColor plus = LinearColor::FromSrgb8(88, 200, 110);
                pen.Rect(0.72f, 0.04f, 0.84f, 0.44f, plus);
                pen.Rect(0.58f, 0.18f, 0.98f, 0.30f, plus);
                break;
            }
            case Icon::Content: {
                const LinearColor color = LinearColor::FromSrgb8(232, 182, 72);
                pen.Rect(0.06f, 0.16f, 0.42f, 0.3f, Shade(color, 0.85f));
                pen.Rect(0.06f, 0.26f, 0.9f, 0.84f, Shade(color, 0.7f));
                pen.Quad({0.18f, 0.42f}, {0.98f, 0.42f}, {0.88f, 0.84f}, {0.06f, 0.84f}, color);
                break;
            }
            case Icon::Search: {
                const LinearColor color = LinearColor::FromSrgb8(150, 158, 172);
                list.AddCircleOutline(pen.At(0.42f, 0.42f), 0.28f * size, color, std::max(1.5f, 0.11f * size));
                pen.Line(0.62f, 0.62f, 0.9f, 0.9f, color, 0.14f);
                break;
            }
            case Icon::Console: {
                const LinearColor frame = LinearColor::FromSrgb8(60, 66, 80);
                const LinearColor prompt = LinearColor::FromSrgb8(110, 210, 130);
                list.AddRoundedRect(pen.At(0.04f, 0.12f), pen.At(0.96f, 0.88f), frame, 0.1f * size);
                pen.Line(0.2f, 0.34f, 0.38f, 0.5f, prompt, 0.09f);
                pen.Line(0.38f, 0.5f, 0.2f, 0.66f, prompt, 0.09f);
                pen.Rect(0.46f, 0.62f, 0.76f, 0.7f, prompt);
                break;
            }
            case Icon::Info: {
                list.AddCircle(pen.At(0.5f, 0.5f), 0.44f * size, LinearColor::FromSrgb8(70, 140, 230));
                const LinearColor white = LinearColor::FromSrgb8(240, 244, 250);
                pen.Rect(0.44f, 0.42f, 0.56f, 0.76f, white);
                pen.Rect(0.44f, 0.22f, 0.56f, 0.33f, white);
                break;
            }
            case Icon::Warning: {
                pen.Triangle({0.5f, 0.06f}, {0.96f, 0.9f}, {0.04f, 0.9f}, LinearColor::FromSrgb8(240, 186, 50));
                const LinearColor dark = LinearColor::FromSrgb8(40, 32, 10);
                pen.Rect(0.44f, 0.34f, 0.56f, 0.64f, dark);
                pen.Rect(0.44f, 0.71f, 0.56f, 0.81f, dark);
                break;
            }
            case Icon::Error: {
                list.AddCircle(pen.At(0.5f, 0.5f), 0.44f * size, LinearColor::FromSrgb8(222, 70, 64));
                const LinearColor white = LinearColor::FromSrgb8(250, 240, 240);
                pen.Line(0.32f, 0.32f, 0.68f, 0.68f, white, 0.12f);
                pen.Line(0.68f, 0.32f, 0.32f, 0.68f, white, 0.12f);
                break;
            }
        }
    }
}
