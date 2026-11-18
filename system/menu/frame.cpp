// The system menu's frame: the 203 Launcher's menuFrame and status rail on the Cardenza (its src/display.cpp,
// with `wide` and `square` false), ported line by line onto the Launcher's own drawing code (launcher/).
// Only what the menu's rows use is here: a label and its description; no values, buttons or signal bars.
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

// The fade's weights out of line: inlined into menuFrame, the ESP-IDF 5.3 Xtensa compiler (GCC 13.2, -O2)
// stops with an internal error (insn does not satisfy its constraints, a float constant in postreload).
__attribute__((noinline)) float Lit(const ListFade& fade, int index, uint32_t now)
{
    return fade.weight(index, now);
}
__attribute__((noinline)) float Ink(ListFade& fade, uint32_t now)
{
    return fade.alpha(now);
}

// menuFrame on the Cardenza (textAlpha 1): rows of 32 pixels, the selection a dark block with white text and
// its description under it; with a ListFade nothing moves, the selection crossfades and a scroll fades
// the rows out and the new ones in.
void menuFrame(const std::vector<Option>& list, float selected, float scroll, ListFade* fade, uint32_t now)
{
    constexpr float textAlpha = 1;
    tft->greyRect(bx, by, cw, ch, 255);
    const int top = by + 4, rows = 4, bottom = SCREEN_HEIGHT, step = (bottom - top) / 4;
    const int textX = bx + 12;
    auto clip = tft->getClip();
    tft->intersectClip(bx, top, cw, bottom - top);
    const float alpha = fade ? Ink(*fade, now) : 1.f;
    if (fade) scroll = float(fade->shown);  // the rows drawn while a scroll fades
    const uint8_t selectedGrey = kUiSelectedGrey;
    const int highlight = top + int(std::lround((selected - scroll) * step));
    if (!fade) tft->greyRect(bx + 6, highlight, cw - 16, step, selectedGrey);
    // A row's text, lit (on the selection, at highlightTop, its detail shown as near selectedAt) or not, at
    // an opacity.
    const auto paint = [&](int i, int y, int highlight, float selected, float opacity) __attribute__((noinline)) {
        const int scale = 1;
        const int labelWidth = std::max(6 * scale, (cw - 28) / (6 * scale) * 6 * scale);
        const LabelLines label = labelLines(list[i].label, labelWidth, scale);
        const int lines = label.count;
        const String& description = list[i].description;
        const float detailVisibility =
            description.isEmpty() ? 0.f : std::clamp(1.f - std::fabs(float(i) - selected), 0.f, 1.f);
        // Ink heights: the 5x7 font, or the taller CJK glyphs.
        const int labelHeight = (lines - 1) * uiLineStep(scale, 8 * scale) + uiTextHeight(scale);
        const int detailGap = uiCjkLayout() ? 2 : 5;
        const int detailHeight = int(std::lround((uiTextHeight(1) + detailGap) * detailVisibility));
        const int labelInk = y + (step - labelHeight - detailHeight) / 2;
        const int labelTop = labelInk - uiTextTop(scale);
        int baseline = labelTop;
        for (int line = 0; line < lines; ++line)
        {
            const String& part = line == 0 ? label.first : label.second;
            const bool dark = baseline >= highlight && baseline + 7 * scale <= highlight + step;
            const uint8_t color = dark ? uint8_t(20 + 235 * textAlpha) : uint8_t(255 * (1 - textAlpha));
            tft->text(part, textX, baseline, scale, color, uint8_t(std::lround(255 * opacity)));
            baseline += uiLineStep(scale, 8 * scale);
        }
        if (detailVisibility > 0)
        {
            const int detailWidth = (cw - 28) / 6 * 6;
            const String detail = uiFitText(description, detailWidth, 1);
            const int detailY = labelInk + labelHeight + detailGap - uiTextTop(1);
            const bool dark = detailY >= highlight && detailY + 7 <= highlight + step;
            tft->text(detail, textX, detailY, 1, dark ? 170 : 110,
                      uint8_t(std::lround(255 * detailVisibility * textAlpha * opacity)));
        }
    };
    for (int i = std::max(0, int(scroll) - 1); i < std::min(int(list.size()), int(scroll) + rows + 2); ++i)
    {
        const int y = top + int(std::lround((i - scroll) * step));
        if (!fade)
        {
            paint(i, y, highlight, selected, 1);
            continue;
        }
        // A crossfade: the row as it is without the selection, the highlight blended over it by its weight,
        // then the lit row at that opacity.
        const float lit = Lit(*fade, i, now);
        if (lit < 1) paint(i, y, -10000, float(i) + 100, 1);
        if (lit <= 0) continue;
        tft->veil(bx + 6, y, cw - 16, step, selectedGrey, uint8_t(std::lround(255 * lit)));
        paint(i, y, y, float(i), lit);
    }
    // A scroll: the rows fade out, and the new ones in.
    if (fade && alpha < 1) tft->veil(bx, top, cw, bottom - top, 255, uint8_t(std::lround(255 * (1 - alpha))));
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

void PaintGrey(const View& view, launcher::ListFade* fade, uint32_t now, int y, int rows, uint8_t* grey)
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
    menuFrame(list, float(selected), float(view.state.scroll), fade, now);
    status(texts[kTitle], view.backHeld);
    tft = nullptr;
}

}  // namespace CardenzaHAL::SystemMenu::detail
