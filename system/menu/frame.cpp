// The system menu's frame: the 203 Launcher's menuFrame and status rail on the Cardenza (its src/display.cpp,
// with `wide` and `square` false), ported line by line onto the Launcher's own drawing code (launcher/).
// Only what the menu's rows use is here: a label and its description; no values, buttons, signal bars or
// software rows (Launch / More).
// tools/launcher_ui.py --compare checks every frame against the Launcher's real menuFrame, pixel for pixel.
#include "system/menu/frame.h"

#include <cmath>
#include <vector>

#include "system/menu/launcher/lcd.h"
#include "system/menu/launcher/motion.h"
#include "system/menu/launcher/ui_layout.h"

namespace CardenzaHAL::SystemMenu::launcher {

namespace {

// src/display.cpp's layout on the Cardenza.
constexpr int bx = 24, by = 0, cw = SCREEN_WIDTH - bx, ch = SCREEN_HEIGHT - by;
Lcd* tft = nullptr;

struct Option {
    String label;
    String description;
};

#include "system/menu/launcher/labels.inc"

// The rail's title: the Launcher shortens long page names (railTitle); the menu's title always fits.
String railTitle(const String& title, int limit)
{
    return textWidth(title, 1) <= limit ? title : uiFitText(title, limit, 1);
}

// status() on an inner page of the Cardenza: the black rail, the page's title down it and the back arrow
// (grey while Back is held). The Cardenza's HAL reads no battery.
void status(const String& title, bool backHeld)
{
    tft->clearClip();
    tft->greyRect(0, 0, 24, SCREEN_HEIGHT, 0);
    tft->textVertical(railTitle(title, SCREEN_HEIGHT - 36), 8, 8, 240);
    const int x = 3, y = SCREEN_HEIGHT - 20;
    const uint8_t ink = uiBackIconGrey(backHeld);
    tft->greyRect(x + 2, y + 7, 14, 1, ink);
    for (int i = 0; i < 6; ++i)
    {
        tft->greyRect(x + 2 + i, y + 7 - i, 1, 1, ink);
        tft->greyRect(x + 2 + i, y + 7 + i, 1, 1, ink);
    }
}

// The bar's edges for this frame, out of line as the crossfade's weights had to be: inlined into
// menuFrame, ListFade::weight made the ESP-IDF 5.3 Xtensa compiler (GCC 13.2, -O2) stop with an internal
// error (insn does not satisfy its constraints, a float constant in postreload). The slide compiles inlined
// too (-O2, 2026-11-18); out of line it is safe from that whatever the flags.
struct BarEdges {
    float top, bottom;
};
__attribute__((noinline)) BarEdges SlideBar(ListSlide& slide, float t, float b, float scroll, uint32_t now)
{
    slide.aim(t, b, scroll, now);
    return {slide.topAt(now), slide.bottomAt(now)};
}

// menuFrame on the Cardenza (wide, square and software false): rows of 32 pixels, the selection a dark bar
// with white text and its description under it. `index`: the row the bar goes to. `slide`: the bar's motion
// (ListSlide, aimed at that row as the frame is drawn); without it the bar is on the selected row. Rows are
// drawn in three passes (barPass), so text is white on the bar and dark off it pixel by pixel.
void menuFrame(const std::vector<Option>& list, float selected, float scroll, ListSlide* slide, int index,
               uint32_t now)
{
    const int focusRow = std::clamp(index < 0 ? int(std::lround(selected)) : index, 0, int(list.size()) - 1);
    tft->greyRect(bx, by, cw, ch, 255);
    const int top = by + 4, rows = 4, bottom = SCREEN_HEIGHT, step = (bottom - top) / 4;
    const int extra = 0;
    const int textX = bx + 12;
    const int barX = bx + 6, barW = cw - 16;
    auto clip = tft->getClip();
    tft->intersectClip(bx, top, cw, bottom - top);
    const Lcd::Clip area = tft->getClip();
    // The bar, in list pixels: the slide's, or on the selected row.
    float barTop = selected * step, barBottom = barTop + step + extra;
    if (slide)
    {
        const BarEdges edges = SlideBar(*slide, focusRow * step, focusRow * step + step + extra, scroll * step, now);
        barTop = edges.top;
        barBottom = edges.bottom;
    }
    const int hy = top + int(std::lround(barTop - scroll * step)), hb = top + int(std::lround(barBottom - scroll * step));
    tft->greyRect(barX, hy, barW, hb - hy, kUiSelectedGrey);
    // A row's label and detail: white on the bar, dark off it.
    const auto paint = [&](int i, int y) __attribute__((noinline)) {
        const int scale = 1;
        const int labelWidth = std::max(6 * scale, (cw - 28) / (6 * scale) * 6 * scale);
        const LabelLines label = labelLines(list[i].label, labelWidth, scale);
        const int lines = label.count;
        const int rowBottom = y + step;
        // The detail opens as far as the bar covers the row: the label rises a pixel at a time to make
        // room, and the detail shows on the bar alone.
        const float covered = std::clamp(float(std::min(hb, rowBottom) - std::max(hy, y)) / step, 0.f, 1.f);
        const String& description = list[i].description;
        const float detailVisibility = description.isEmpty() ? 0.f : covered;
        // Ink heights: the 5x7 font, or the taller CJK glyphs.
        const int labelHeight = (lines - 1) * uiLineStep(scale, 8 * scale) + uiTextHeight(scale);
        const int detailGap = uiCjkLayout() ? 2 : 5;
        const int detailHeight = int(std::lround((uiTextHeight(1) + detailGap) * detailVisibility));
        const int labelInk = y + (step - labelHeight - detailHeight) / 2;
        String detail;
        if (detailVisibility > 0)
        {
            const int detailWidth = (cw - 28) / 6 * 6;
            detail = uiFitText(description, detailWidth, 1);
        }
        const int detailY = labelInk + labelHeight + detailGap - uiTextTop(1);
        // The row alone while its detail opens or closes, so it never reaches into the next one.
        const bool open = covered >= 1;
        const int rowTop = std::max(area.y, y);
        const int rowEnd = open ? area.y + area.h : std::min(area.y + area.h, rowBottom);
        const Lcd::Clip rowArea = {area.x, rowTop, area.w, rowEnd - rowTop};
        for (int pass = 0; pass < 3; ++pass)
        {
            const bool lit = barPass(pass, rowArea, hy, hb);
            int baseline = labelInk - uiTextTop(scale);
            for (int line = 0; line < lines; ++line)
            {
                tft->text(line == 0 ? label.first : label.second, textX, baseline, scale, lit ? 255 : 0);
                baseline += uiLineStep(scale, 8 * scale);
            }
            if ((lit || open) && !detail.isEmpty()) tft->text(detail, textX, detailY, 1, lit ? 170 : 110);
        }
        tft->clip(area.x, area.y, area.w, area.h);
    };
    for (int i = std::max(0, int(scroll) - 1); i < std::min(int(list.size()), int(scroll) + rows + 2); ++i)
    {
        const int y = top + int(std::lround((i - scroll) * step + extra * std::clamp(float(i) - selected, 0.f, 1.f)));
        paint(i, y);
    }
    tft->clip(clip.x, clip.y, clip.w, clip.h);
    if (list.size() > size_t(rows))
    {
        // The track spans the list area: the same gap below the status bar and above the screen edge.
        const int trackBottom = SCREEN_HEIGHT - 4, track = trackBottom - top;
        const int thumb = std::max(10, track * rows / int(list.size()));
        const int y = top + int(std::lround((track - thumb) * scroll / (list.size() - rows)));
        tft->greyRect(236, top, 2, track, 218);
        tft->greyRect(236, y, 2, thumb, 80);
    }
}

}  // namespace

}  // namespace CardenzaHAL::SystemMenu::launcher

namespace CardenzaHAL::SystemMenu::detail {

void PaintGrey(const View& view, launcher::ListSlide* slide, uint32_t now, int y, int rows, uint8_t* grey)
{
    using namespace launcher;
    Lcd canvas;
    canvas.band(grey, y, rows);
    tft = &canvas;
    const int language = std::clamp(view.language, 0, kLanguageCount - 1);
    i18nSetLanguage(i18nLanguageFromCode(kLanguageCodes[language]));
    const char* const* texts = kTexts[language];
    std::vector<Option> list = {
        {texts[kContinue], view.firmware},
        {texts[kLauncher], texts[kLauncherDetail]},
        {texts[kRestart], texts[kRestartDetail]},
        {texts[kOff], texts[kOffDetail]},
        {texts[kDownload], texts[kDownloadDetail]},
    };
    const int selected = std::clamp(view.state.selected, 0, kItemCount - 1);
    if (view.releasePrompt) list[selected].description = texts[kRelease];
    menuFrame(list, float(selected), float(view.state.scroll), slide, selected, now);
    status(texts[kTitle], view.backHeld);
    tft = nullptr;
}

}  // namespace CardenzaHAL::SystemMenu::detail
