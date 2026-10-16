// Vector icons in the spirit of SF Symbols, drawn on a 24-unit grid.
// No icon font is needed, so they look the same on Windows 10, 11 and Wine.
#include <algorithm>
#include <cmath>

#include "gfx.h"

namespace {
constexpr float PI = 3.14159265f;

struct Pen {
    Canvas& c;
    float cx, cy, k;   // k = pixels per grid unit
    Color col;
    float sw;
    float X(float x) const { return cx + (x - 12) * k; }
    float Y(float y) const { return cy + (y - 12) * k; }
    void line(float x1, float y1, float x2, float y2) const { c.Line(X(x1), Y(y1), X(x2), Y(y2), col, sw); }
    void circle(float x, float y, float r) const { c.StrokeCircle(X(x), Y(y), r * k, col, sw); }
    void dot(float x, float y, float r) const { c.FillCircle(X(x), Y(y), r * k, col); }
    void arc(float x, float y, float r, float a0, float sweep) const { c.Arc(X(x), Y(y), r * k, a0, sweep, col, sw); }
    void rrect(float x, float y, float w, float h, float r) const {
        c.StrokeRound(D2D1::RectF(X(x), Y(y), X(x + w), Y(y + h)), r * k, col, sw);
    }
    void frrect(float x, float y, float w, float h, float r) const {
        c.FillRound(D2D1::RectF(X(x), Y(y), X(x + w), Y(y + h)), r * k, col);
    }
    void poly(std::initializer_list<D2D1_POINT_2F> pts, bool closed, bool fill) const {
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* s = nullptr;
        if (FAILED(g_gfx.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return; }
        bool first = true;
        for (auto p : pts) {
            D2D1_POINT_2F q = D2D1::Point2F(X(p.x), Y(p.y));
            if (first) { s->BeginFigure(q, fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW); first = false; }
            else s->AddLine(q);
        }
        s->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        s->Close();
        if (fill) c.FillGeometry(g, col);
        else c.StrokeGeometry(g, col, sw);
        s->Release();
        g->Release();
    }
};

D2D1_POINT_2F P(float x, float y) { return D2D1::Point2F(x, y); }
}  // namespace

void DrawIcon(Canvas& c, const std::wstring& n, float cx, float cy, float size, Color col, float stroke) {
    Pen p{c, cx, cy, size / 24.f, col, stroke * size / 24.f * 1.15f};
    if (n == L"wifi") {
        p.dot(12, 18.5f, 1.6f);
        p.arc(12, 19, 5.5f, -42, 84);
        p.arc(12, 19, 10, -44, 88);
        p.arc(12, 19, 14.5f, -45, 90);
    } else if (n == L"bluetooth") {
        p.poly({P(7, 7.5f), P(16.5f, 16), P(12, 20.5f), P(12, 3.5f), P(16.5f, 8), P(7, 16.5f)}, false, false);
    } else if (n == L"speaker" || n == L"speaker-off") {
        p.poly({P(3.5f, 9.5f), P(7, 9.5f), P(11.5f, 5), P(11.5f, 19), P(7, 14.5f), P(3.5f, 14.5f)}, true, true);
        if (n == L"speaker") {
            p.arc(12, 12, 4, 40, 100);
            p.arc(12, 12, 7.5f, 38, 104);
        } else {
            p.line(15, 9, 21, 15);
            p.line(21, 9, 15, 15);
        }
    } else if (n == L"mic" || n == L"mic-off") {
        p.rrect(9, 3, 6, 11, 3);
        p.arc(12, 11, 6, 90, 180);
        p.line(12, 17, 12, 21);
        if (n == L"mic-off") p.line(4.5f, 4, 19.5f, 20);
    } else if (n == L"moon") {
        // crescent = outer circle minus an offset inner circle, built from two arcs
        float ax = 11.5f, ay = 12.5f, ar = 8.5f, bx = 16.5f, by = 8.f, br = 7.f;
        float dx = bx - ax, dy = by - ay, d = std::sqrt(dx * dx + dy * dy);
        float a = (ar * ar - br * br + d * d) / (2 * d), h = std::sqrt(std::max(0.f, ar * ar - a * a));
        float mx = ax + a * dx / d, my = ay + a * dy / d;
        D2D1_POINT_2F i1 = D2D1::Point2F(p.X(mx + h * dy / d), p.Y(my - h * dx / d));
        D2D1_POINT_2F i2 = D2D1::Point2F(p.X(mx - h * dy / d), p.Y(my + h * dx / d));
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(g_gfx.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
            sk->BeginFigure(i1, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddArc(D2D1::ArcSegment(i2, D2D1::SizeF(ar * p.k, ar * p.k), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                        D2D1_ARC_SIZE_LARGE));
            sk->AddArc(D2D1::ArcSegment(i1, D2D1::SizeF(br * p.k, br * p.k), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                        D2D1_ARC_SIZE_SMALL));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();
            c.FillGeometry(g, col);
        }
        SafeRelease(sk);
        SafeRelease(g);
    } else if (n == L"contrast") {
        p.circle(12, 12, 8.5f);
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g_gfx.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            s->BeginFigure(D2D1::Point2F(p.X(12), p.Y(3.5f)), D2D1_FIGURE_BEGIN_FILLED);
            s->AddArc(D2D1::ArcSegment(D2D1::Point2F(p.X(12), p.Y(20.5f)), D2D1::SizeF(8.5f * p.k, 8.5f * p.k), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            c.FillGeometry(g, col);
        }
        SafeRelease(s); SafeRelease(g);
    } else if (n == L"sun") {
        p.circle(12, 12, 4);
        for (int i = 0; i < 8; i++) {
            float a = i * PI / 4;
            p.line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 9.5f * std::cos(a), 12 + 9.5f * std::sin(a));
        }
    } else if (n == L"cast") {
        p.poly({P(3, 9), P(3, 5.5f), P(21, 5.5f), P(21, 18.5f), P(14, 18.5f)}, false, false);
        p.arc(3, 19, 4, 0, 90);
        p.arc(3, 19, 8, 0, 90);
        p.dot(3.5f, 18.5f, 1.4f);
    } else if (n == L"screen") {
        p.rrect(3, 4.5f, 18, 12, 2);
        p.line(9, 20, 15, 20);
        p.line(12, 16.5f, 12, 20);
    } else if (n == L"crop") {
        p.poly({P(7, 3), P(7, 17), P(21, 17)}, false, false);
        p.poly({P(3, 7), P(17, 7), P(17, 21)}, false, false);
    } else if (n == L"dropper") {
        p.poly({P(14.5f, 6.5f), P(5, 16), P(4.5f, 19.5f), P(8, 19), P(17.5f, 9.5f)}, false, false);
        p.line(13, 5, 19, 11);
        p.poly({P(15.5f, 7.5f), P(18, 5), P(19.5f, 5), P(19.5f, 6.5f), P(16.5f, 8.5f)}, true, true);
    } else if (n == L"record") {
        p.circle(12, 12, 8.5f);
        p.dot(12, 12, 4.5f);
    } else if (n == L"clipboard") {
        p.rrect(5, 4.5f, 14, 16.5f, 2.2f);
        p.frrect(8.5f, 3, 7, 3.5f, 1.3f);
        p.line(8.5f, 11, 15.5f, 11);
        p.line(8.5f, 15, 13.5f, 15);
    } else if (n == L"wallpaper") {
        p.rrect(3, 5, 18, 14, 2.4f);
        p.poly({P(5.5f, 16.5f), P(10, 11), P(13, 14), P(15, 12), P(18.5f, 16.5f)}, false, false);
        p.dot(15.5f, 8.8f, 1.6f);
    } else if (n == L"lock") {
        p.frrect(5, 10.5f, 14, 10.5f, 2.2f);
        p.arc(12, 9, 4.5f, -90, 180);
        p.line(7.5f, 9, 7.5f, 10.5f);
        p.line(16.5f, 9, 16.5f, 10.5f);
    } else if (n == L"power") {
        p.arc(12, 12.5f, 8, 35, 290);
        p.line(12, 3, 12, 11);
    } else if (n == L"timer") {
        p.circle(12, 13, 8);
        p.line(12, 13, 12, 8.5f);
        p.line(10, 2.8f, 14, 2.8f);
        p.line(18.5f, 6.5f, 19.8f, 5.2f);
    } else if (n == L"gear") {
        for (int i = 0; i < 8; i++) {
            float a = i * PI / 4;
            p.line(12 + 6.3f * std::cos(a), 12 + 6.3f * std::sin(a), 12 + 9 * std::cos(a), 12 + 9 * std::sin(a));
        }
        p.circle(12, 12, 6.3f);
        p.circle(12, 12, 2.6f);
    } else if (n == L"folder") {
        p.poly({P(3, 18.5f), P(3, 5.5f), P(9.5f, 5.5f), P(11.5f, 8), P(21, 8), P(21, 18.5f)}, true, false);
    } else if (n == L"pencil") {
        p.poly({P(15.5f, 4.5f), P(19.5f, 8.5f), P(8.5f, 19.5f), P(4, 20), P(4.5f, 15.5f)}, true, false);
        p.line(13.5f, 6.5f, 17.5f, 10.5f);
    } else if (n == L"check") {
        p.poly({P(4.5f, 12.5f), P(9.5f, 17.5f), P(19.5f, 6.5f)}, false, false);
    } else if (n == L"plus") {
        p.line(12, 5, 12, 19);
        p.line(5, 12, 19, 12);
    } else if (n == L"minus") {
        p.line(5, 12, 19, 12);
    } else if (n == L"close") {
        p.line(6, 6, 18, 18);
        p.line(18, 6, 6, 18);
    } else if (n == L"play") {
        p.poly({P(7, 4), P(20, 12), P(7, 20)}, true, true);
    } else if (n == L"pause") {
        p.frrect(6, 4, 4, 16, 1.2f);
        p.frrect(14, 4, 4, 16, 1.2f);
    } else if (n == L"next" || n == L"prev") {
        bool nx = n == L"next";
        auto m = [&](float x) { return nx ? x : 24 - x; };
        p.poly({P(m(2.5f), 6), P(m(12), 12), P(m(2.5f), 18)}, true, true);
        p.poly({P(m(11.5f), 6), P(m(21), 12), P(m(11.5f), 18)}, true, true);
    } else if (n == L"battery") {
        p.rrect(2.5f, 7, 17, 10, 2.5f);
        p.frrect(20.5f, 10, 1.6f, 4, 0.8f);
    } else if (n == L"bolt") {
        p.poly({P(13.5f, 2.5f), P(5.5f, 13.5f), P(11.5f, 13.5f), P(10.5f, 21.5f), P(18.5f, 10.5f), P(12.5f, 10.5f)}, true, true);
    } else if (n == L"capslock") {
        p.poly({P(12, 3.5f), P(20, 12), P(16, 12), P(16, 15.5f), P(8, 15.5f), P(8, 12), P(4, 12)}, true, false);
        p.line(8, 19.5f, 16, 19.5f);
    } else if (n == L"download") {
        p.line(12, 3.5f, 12, 15);
        p.poly({P(7, 10.5f), P(12, 15.5f), P(17, 10.5f)}, false, false);
        p.line(5, 20, 19, 20);
    } else if (n == L"terminal") {
        p.rrect(2.5f, 4, 19, 16, 3);
        p.poly({P(6.5f, 9), P(9.5f, 12), P(6.5f, 15)}, false, false);
        p.line(11.5f, 15, 16.5f, 15);
    } else if (n == L"cloud") {
        p.arc(9, 13, 4.5f, 180, 180);
        p.arc(14, 11, 5.5f, -80, 170);
        p.line(4.5f, 17.5f, 18, 17.5f);
        p.arc(18, 14, 3.5f, 0, 180);
    } else if (n == L"rain") {
        DrawIcon(c, L"cloud", cx, cy - size * 0.12f, size * 0.85f, col, stroke);
        p.line(8, 18, 7, 21);
        p.line(12, 18, 11, 21);
        p.line(16, 18, 15, 21);
    } else if (n == L"snow") {
        DrawIcon(c, L"cloud", cx, cy - size * 0.12f, size * 0.85f, col, stroke);
        p.dot(8, 19.5f, 1); p.dot(12, 20.5f, 1); p.dot(16, 19.5f, 1);
    } else if (n == L"storm") {
        DrawIcon(c, L"cloud", cx, cy - size * 0.12f, size * 0.85f, col, stroke);
        p.poly({P(13, 16.5f), P(10.5f, 20), P(13.5f, 20), P(11.5f, 23)}, false, false);
    } else if (n == L"fog") {
        p.line(4, 9, 20, 9);
        p.line(6, 13, 18, 13);
        p.line(4, 17, 20, 17);
    } else if (n == L"night") {
        DrawIcon(c, L"moon", cx, cy, size * 0.85f, col, stroke);
    } else if (n == L"chevron") {
        p.poly({P(9, 5), P(16, 12), P(9, 19)}, false, false);
    } else if (n == L"palette") {
        p.circle(12, 12, 8.5f);
        p.dot(8.5f, 9.5f, 1.4f); p.dot(12, 7.5f, 1.4f); p.dot(15.5f, 9.5f, 1.4f); p.dot(15.5f, 13.5f, 1.4f);
    } else if (n == L"island") {
        p.frrect(4, 9, 16, 6, 3);
    } else if (n == L"sparkles") {
        p.poly({P(10, 3), P(11.8f, 8.2f), P(17, 10), P(11.8f, 11.8f), P(10, 17), P(8.2f, 11.8f), P(3, 10), P(8.2f, 8.2f)}, true, true);
        p.poly({P(18, 14), P(18.9f, 16.1f), P(21, 17), P(18.9f, 17.9f), P(18, 20), P(17.1f, 17.9f), P(15, 17), P(17.1f, 16.1f)}, true, true);
    } else if (n == L"keyboard") {
        p.rrect(2.5f, 6, 19, 12, 2.5f);
        for (int i = 0; i < 5; i++) p.dot(6 + i * 3, 10, 0.9f);
        p.line(8, 14.5f, 16, 14.5f);
    } else if (n == L"info") {
        p.circle(12, 12, 8.5f);
        p.line(12, 11, 12, 16.5f);
        p.dot(12, 7.8f, 1.1f);
    } else if (n == L"bell") {
        p.poly({P(5, 17), P(6.5f, 14.5f), P(6.5f, 10)}, false, false);
        p.arc(12, 10, 5.5f, -90, 180);
        p.poly({P(17.5f, 10), P(17.5f, 14.5f), P(19, 17), P(5, 17)}, false, false);
        p.arc(12, 18.5f, 2, 90, 180);
    } else if (n == L"music") {
        p.line(9, 17, 9, 5);
        p.line(9, 5, 19, 3.5f);
        p.line(19, 3.5f, 19, 15);
        p.dot(6.8f, 17.5f, 2.6f);
        p.dot(16.8f, 15.5f, 2.6f);
    } else if (n == L"grid") {
        p.rrect(4, 4, 7, 7, 2); p.rrect(13, 4, 7, 7, 2); p.rrect(4, 13, 7, 7, 2); p.rrect(13, 13, 7, 7, 2);
    } else {
        p.circle(12, 12, 7);
    }
}

void DrawSpark(Canvas& c, float cx, float cy, float size, Color color, float angle) {
    float k = size / 24.f;
    D2D1_MATRIX_3X2_F old = c.Transform();
    for (int i = 0; i < 12; i++) {
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Rotation(angle + i * 30.f) * D2D1::Matrix3x2F::Translation(cx, cy) * old;
        c.SetTransform(m);
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g_gfx.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            s->BeginFigure(D2D1::Point2F(0, -11 * k), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(D2D1::Point2F(1.7f * k, -3 * k));
            s->AddLine(D2D1::Point2F(-1.7f * k, -3 * k));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            c.FillGeometry(g, color);
        }
        SafeRelease(s);
        SafeRelease(g);
    }
    c.SetTransform(old);
    c.FillCircle(cx, cy, 2.6f * k, color);
}

void DrawWindowsMark(Canvas& c, float cx, float cy, float size, Color color) {
    float g = std::max(1.5f, size * 0.12f), h = (size - g) / 2;
    float x = cx - size / 2, y = cy - size / 2;
    c.FillRect(Rect(x, y, h, h), color);
    c.FillRect(Rect(x + h + g, y, h, h), color);
    c.FillRect(Rect(x, y + h + g, h, h), color);
    c.FillRect(Rect(x + h + g, y + h + g, h, h), color);
}
