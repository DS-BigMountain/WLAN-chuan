#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <string>

namespace ui {
// The same device-independent drawing primitives are used for the main surface
// and buffered native controls. Native controls retain keyboard and UIA behavior.
struct Canvas {
    ID2D1RenderTarget *target;
    ID2D1SolidColorBrush *brush = nullptr;
    explicit Canvas(ID2D1RenderTarget *t) : target(t) {
        target->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f), &brush);
    }
    ~Canvas() {
        if (brush)
            brush->Release();
    }
    void Color(float gray) {
        brush->SetColor(D2D1::ColorF(gray, gray, gray));
    }
    void Box(float x, float y, float w, float h, float radius, float fill, float border = -1) {
        auto r = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius);
        Color(fill);
        target->FillRoundedRectangle(r, brush);
        if (border >= 0) {
            Color(border);
            target->DrawRoundedRectangle(r, brush, 1);
        }
    }
    void Line(float x, float y, float x2, float y2, float gray, float weight = 1.5f) {
        Color(gray);
        target->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x2, y2), brush, weight);
    }
    void Circle(float x, float y, float radius, float gray, bool fill = false) {
        Color(gray);
        auto e = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
        if (fill)
            target->FillEllipse(e, brush);
        else
            target->DrawEllipse(e, brush, 1.5f);
    }
    void Text(const std::wstring &s, float x, float y, float w, float h, IDWriteTextFormat *font, float gray,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!brush || !font)
            return;
        font->SetTextAlignment(align);
        font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Color(gray);
        target->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), font, D2D1::RectF(x, y, x + w, y + h),
                          brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    // Icons use a 24 DIP grid. Colors remain neutral throughout the application.
    void Icon(int kind, float x, float y, float gray, float factor = 1) {
        auto old = D2D1::Matrix3x2F::Identity();
        target->GetTransform(&old);
        target->SetTransform(D2D1::Matrix3x2F::Scale(factor, factor) * D2D1::Matrix3x2F::Translation(x, y) *
                             old);
        switch (kind) {
        case 0: // transfer mark
            Line(4, 7, 19, 7, gray, 2);
            Line(15, 3, 19, 7, gray, 2);
            Line(19, 7, 15, 11, gray, 2);
            Line(20, 17, 5, 17, gray, 2);
            Line(9, 13, 5, 17, gray, 2);
            Line(5, 17, 9, 21, gray, 2);
            break;
        case 1: // computer
            Box(3, 4, 18, 13, 2, .96f, gray);
            Line(12, 17, 12, 21, gray);
            Line(8, 21, 16, 21, gray);
            break;
        case 2: // file
            Color(gray);
            target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(5, 3, 19, 21), 2, 2), brush, 1.5f);
            Line(9, 10, 15, 10, gray);
            Line(9, 14, 15, 14, gray);
            break;
        case 3: // folder
            Line(3, 7, 3, 20, gray);
            Line(3, 20, 21, 20, gray);
            Line(21, 20, 21, 8, gray);
            Line(21, 8, 12, 8, gray);
            Line(12, 8, 9, 5, gray);
            Line(9, 5, 3, 5, gray);
            Line(3, 5, 3, 7, gray);
            break;
        case 4: // settings
            Circle(12, 12, 7, gray);
            Circle(12, 12, 2.5f, gray);
            Line(12, 2, 12, 5, gray);
            Line(12, 19, 12, 22, gray);
            Line(2, 12, 5, 12, gray);
            Line(19, 12, 22, 12, gray);
            Line(5, 5, 7, 7, gray);
            Line(17, 17, 19, 19, gray);
            Line(5, 19, 7, 17, gray);
            Line(17, 7, 19, 5, gray);
            break;
        case 5: // connect
            Line(5, 7, 10, 7, gray);
            Line(10, 7, 10, 17, gray);
            Line(10, 17, 5, 17, gray);
            Line(19, 7, 14, 7, gray);
            Line(14, 7, 14, 17, gray);
            Line(14, 17, 19, 17, gray);
            Line(2, 12, 10, 12, gray);
            Line(14, 12, 22, 12, gray);
            break;
        case 6: // refresh
            Circle(12, 12, 7, gray);
            Line(19, 3, 19, 9, gray);
            Line(19, 9, 13, 9, gray);
            break;
        case 7: // upload
            Line(12, 16, 12, 3, gray);
            Line(7, 8, 12, 3, gray);
            Line(12, 3, 17, 8, gray);
            Line(4, 14, 4, 21, gray);
            Line(4, 21, 20, 21, gray);
            Line(20, 21, 20, 14, gray);
            break;
        case 8: // Web service
            Circle(12, 12, 9, gray);
            Color(gray);
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12, 12), 4, 9), brush, 1.5f);
            Line(3, 12, 21, 12, gray);
            break;
        case 9: // Ping
            Circle(12, 12, 8, gray);
            Circle(12, 12, 3, gray);
            Line(12, 1, 12, 5, gray);
            Line(12, 19, 12, 23, gray);
            Line(1, 12, 5, 12, gray);
            Line(19, 12, 23, 12, gray);
            break;
        case 10: // note
            Line(5, 18, 6, 13, gray);
            Line(6, 13, 16, 3, gray);
            Line(16, 3, 21, 8, gray);
            Line(21, 8, 11, 18, gray);
            Line(11, 18, 5, 18, gray);
            Line(4, 22, 20, 22, gray);
            break;
        case 11: // delete
            Line(4, 6, 20, 6, gray);
            Line(9, 3, 15, 3, gray);
            Line(6, 9, 7, 21, gray);
            Line(7, 21, 17, 21, gray);
            Line(17, 21, 18, 9, gray);
            Line(10, 10, 10, 17, gray);
            Line(14, 10, 14, 17, gray);
            break;
        case 12: // browser
            Color(gray);
            target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2, 3, 22, 21), 2, 2), brush, 1.5f);
            Line(2, 8, 22, 8, gray);
            Circle(6, 5.5f, .7f, gray, true);
            Circle(9, 5.5f, .7f, gray, true);
            break;
        case 13: // stop
            Color(gray);
            target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(5, 5, 19, 19), 2, 2), brush, 1.5f);
            break;
        }
        target->SetTransform(old);
    }
};
} // namespace ui
