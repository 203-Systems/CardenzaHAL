// The system menu's frames: the 203 Launcher's menuFrame, status rail and dialog on the Cardenza (its
// src/display.cpp, with `wide` and `square` false), ported line by line onto the Launcher's own drawing
// code (launcher/), and the pages it draws inline elsewhere: its Backlight page (src/settings.cpp
// setBrightnessMenu) for a value, and the rows of its About page (drawAbout) for what the board is.
// Only what the menu's pages use is here: rows with a label, a description and a value; no buttons in rows,
// signal bars or software rows (Launch / More); no touch, no hold to confirm.
// tools/launcher_ui.py --compare checks the lists and the dialog against the Launcher's real menuFrame and
// drawDialog, pixel for pixel.
#include "system/menu/frame.h"

#include <cmath>
#include <cstdio>
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
    String value;
};

#include "system/menu/launcher/labels.inc"

// Short page names for the side rail (the Launcher's railTitle, without its table of its own pages' short
// names): the first word, and only then an ellipsis. `limit` in pixels along the rail.
String railTitle(const String& title, int limit, int scale = 1)
{
    if (textWidth(title, scale) <= limit) return title;
    const int last = title.lastIndexOf(' ');
    if (last > 0 && title.indexOf('/', last) > last && textWidth(i18nTail(title, last + 1), scale) <= limit)
        return i18nTail(title, last + 1);
    const int space = title.indexOf(' ');
    if (space > 0 && textWidth(title.substring(0, space), scale) <= limit) return title.substring(0, space);
    return uiFitText(title, limit, scale);
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
// with white text and its description under it, a value on the right. `index`: the row the bar goes to.
// `slide`: the bar's motion (ListSlide, aimed at that row as the frame is drawn); without it the bar is on
// the selected row. Rows are drawn in three passes (barPass), so text is white on the bar and dark off it
// pixel by pixel.
void menuFrame(const std::vector<Option>& list, float selected, float scroll, ListSlide* slide, int index,
               uint32_t now)
{
    const int focusRow = std::clamp(index < 0 ? int(std::lround(selected)) : index, 0, int(list.size()) - 1);
    tft->greyRect(bx, by, cw, ch, 255);
    const int top = by + 4, rows = 4, bottom = SCREEN_HEIGHT, step = (bottom - top) / 4;
    const int extra = 0;
    const int textX = bx + 12, rightX = bx + cw - 12;
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
    // A row's label, detail and value: white on the bar, dark off it.
    const auto paint = [&](int i, int y) __attribute__((noinline)) {
        const int scale = 1;
        // A long value is cut so the label always keeps most of the row.
        const String value = uiFitText(list[i].value, (cw / 2) / 6 * 6, 1);
        const int valueWidth = textWidth(value, 1);
        const int reserved = value.isEmpty() ? 0 : valueWidth + 12;
        (void)rightX;
        const int labelWidth = std::max(6 * scale, (cw - 28 - reserved) / (6 * scale) * 6 * scale);
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
            int detailWidth = (cw - 28) / 6 * 6;
            // The detail ends before a value on its right instead of running under it.
            if (!value.isEmpty()) detailWidth = std::min(detailWidth, (bx + cw - 12 - valueWidth - 6 - textX) / 6 * 6);
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
            if (!value.isEmpty()) tft->text(value, bx + cw - 12 - valueWidth, y + (step - 7) / 2, 1, lit ? 205 : 115);
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

// ---- The Launcher's dialog (src/display.cpp drawDialog and its buttons, on the Cardenza) ---------------------

constexpr int contentX = bx + 12, contentW = cw - 24;
constexpr int buttonH = 31, buttonY = SCREEN_HEIGHT - 8 - buttonH;

String fitText(const String& text, int width, int scale)
{
    return uiFitText(text, std::max(12 * scale, width), scale);
}

// A label that does not fit at scale 2 drops to scale 1 instead of spilling into the neighbouring button.
void actionButton(const String& label, int x, int width, bool selected, int scale)
{
    tft->greyRect(x, buttonY, width, buttonH, selected ? kUiSelectedGrey : 232);
    if (!scale) scale = textWidth(label, 2) + 6 <= width ? 2 : 1;
    const String shown = fitText(label, width - 4, scale);
    tft->text(shown, x + (width - textWidth(shown, scale) + scale) / 2, buttonY + (buttonH - 7 * scale) / 2, scale,
              selected ? 255 : 0);
}

void actionButtons(const String& left, const String& right, int selected)
{
    const int width = (contentW - 8) / 2;
    // A pair shares one text size.
    const int scale = std::max(textWidth(left, 2), textWidth(right, 2)) + 6 <= width ? 2 : 1;
    actionButton(left, contentX, width, selected == 0, scale);
    actionButton(right, contentX + width + 8, width, selected == 1, scale);
}

// The question and its detail stay on screen and the actions sit at the bottom. Detail uses scale 2 where it
// fits and otherwise small text in near-black, never a faint grey.
void drawDialog(const String& heading, const String& detail, const String& first, const String& second, int selected)
{
    tft->clearClip();
    tft->greyRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 255);
    const bool actions = !first.isEmpty();
    const int floor = (actions ? buttonY : SCREEN_HEIGHT) - 8;
    const int headingScale = 2;
    int y = 12;
    for (const auto& line : uiWrap(heading, contentW, headingScale, 2))
    {
        tft->text(line, contentX, y, headingScale, 0);
        y += uiLineStep(headingScale, 8 * headingScale);
    }
    y += 6;
    int scale = 1;
    const auto step = [](int s) { return uiLineStep(s, s == 1 ? 11 : 18); };
    for (const auto& line : uiWrap(detail, contentW, scale, std::max(1, (floor - y) / step(scale))))
    {
        tft->text(line, contentX, y, scale, scale == 1 ? 30 : 45);
        y += step(scale);
    }
    if (actions && second.isEmpty()) actionButton(first, contentX, contentW, true, 0);
    else if (actions) actionButtons(first, second, selected);
}

// ---- A value: the Launcher's Backlight page on the Cardenza (src/settings.cpp setBrightnessMenu) ----------

// `value` large, the track from `minText` to `maxText` filled to `fraction` (0 to 1), the knob there; `limit`
// (0 to 1, 1: none) the part of the track past which the value cannot go, lighter.
void valuePage(const String& hint, const String& value, const String& minText, const String& maxText, float fraction,
               float limit)
{
    const int x = 24, y = 0;
    const int width = SCREEN_WIDTH - x;
    const int height = SCREEN_HEIGHT - y;
    const int trackX = x + 12;
    const int trackWidth = width - 24;
    const int filled = int(std::lround(trackWidth * std::clamp(fraction, 0.f, 1.f)));
    tft->clearClip();
    tft->greyRect(x, y, width, height, 255);
    // Cut to the page (the Launcher's draws it whole; Indonesian runs past the screen there).
    tft->text(uiFitText(hint, width - 24, 1), x + 12, y + 8, 1, 110);
    tft->text(value, x + 12, y + 24, 3, 0);
    const int trackY = y + height - 32;
    tft->greyRect(trackX, trackY, trackWidth, 5, 220);
    if (limit < 1)
    {
        const int reachable = int(std::lround(trackWidth * std::clamp(limit, 0.f, 1.f)));
        tft->greyRect(trackX + reachable, trackY, trackWidth - reachable, 5, 242);
    }
    tft->greyRect(trackX, trackY, filled, 5, 0);
    tft->greyRect(trackX + filled - 2, trackY - 8, 4, 21, 0);
    tft->text(minText, trackX, trackY + 18, 1, 130);
    tft->text(maxText, trackX + trackWidth - textWidth(maxText, 1), trackY + 18, 1, 130);
}

// ---- What the board is: the rows of the Launcher's About page (src/display.cpp drawAbout) -----------------

struct AboutRow {
    String label, value;
};

// `name` (the firmware) at scale 2 where it fits, `version` under it, a rule, then a row per fact: its label
// grey, its value black after the widest label.
void aboutPage(const String& name, const String& version, const std::vector<AboutRow>& rows)
{
    tft->clearClip();
    tft->greyRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 255);
    const int t = textWidth(name, 2) <= contentW ? 2 : 1, s = 1, step = uiLineStep(s, 11);
    const int titleGap = 8, ruleGap = 6;
    const int height = 7 * t + titleGap + step + 2 * ruleGap + 1 + int(rows.size()) * step - (step - 8 * s);
    int y = by + std::max(4, (SCREEN_HEIGHT - by - height) / 2);
    tft->text(fitText(name, contentW, t), contentX, y, t, 0);
    y += 7 * t + titleGap;
    tft->text(fitText(version, contentW, s), contentX, y, s, 90);
    y += 8 * s + ruleGap;
    tft->greyRect(contentX, y, contentW, 1, 215);
    y += 1 + ruleGap;
    int labelW = 7 * 6 * s;
    for (const auto& row : rows) labelW = std::max(labelW, textWidth(row.label, s) + 6 * s);
    for (const auto& row : rows)
    {
        tft->text(row.label, contentX, y, s, 110);
        // Values are data (names, sizes): the 5x7 font, or the language's words for them.
        tft->text(fitText(row.value, contentW - labelW, s), contentX + labelW, y, s, 0);
        y += step;
    }
}

String Fill(const char* pattern, const String& a, const String& b = String())
{
    // The words' {0} and {1}.
    String out;
    for (const char* p = pattern; *p != 0; ++p)
    {
        if (p[0] == '{' && (p[1] == '0' || p[1] == '1') && p[2] == '}')
        {
            out += p[1] == '0' ? a : b;
            p += 2;
            continue;
        }
        out += *p;
    }
    return out;
}

}  // namespace

}  // namespace CardenzaHAL::SystemMenu::launcher

namespace CardenzaHAL::SystemMenu::detail {

namespace {

using Settings::Id;
using Settings::detail::Index;

// The rows of the Settings page.
std::vector<launcher::Option> SettingsRows(const View& view, const char* const* texts)
{
    const auto& values = view.state.values;
    const auto value = [&](Id id) {
        char text[16];
        ValueText(id, values[Index(id)], text, sizeof(text));
        return launcher::String(text);
    };
    const int language = std::max(0, Settings::detail::LanguageIndex(values[Index(Id::Language)]));
    return {
        {texts[kVolume], texts[kVolumeDetail], value(Id::Volume)},
        {texts[kVolumeLimit], texts[kVolumeLimitDetail], value(Id::VolumeLimit)},
        {texts[kScreen], texts[kSharedDetail], value(Id::ScreenBrightness)},
        {texts[kKeys], texts[kKeysDetail], value(Id::KeyBrightness)},
        {texts[kLanguage], texts[kSharedDetail], kLanguageNames[language]},
        {texts[kReset], texts[kResetDetail], ""},
    };
}

// The Language page: each in its own script, and named in the current one when that differs (the Launcher's
// languageSetting).
std::vector<launcher::Option> LanguageRows(const char* const* texts)
{
    static constexpr TextId kNames[kLanguageCount] = {kEnglish, kChinese, kJapanese, kIndonesian, kVietnamese};
    std::vector<launcher::Option> rows;
    for (int i = 0; i < kLanguageCount; ++i)
    {
        const launcher::String name = kLanguageNames[i], translated = texts[kNames[i]];
        rows.push_back({name, launcher::i18nPlain(translated) == launcher::i18nPlain(name) ? launcher::String() : translated, ""});
    }
    return rows;
}

void PaintValue(const View& view, const char* const* texts)
{
    using namespace launcher;
    const Id id = view.state.editing;
    const Settings::Info& info = Settings::detail::kInfo[Index(id)];
    const int value = view.state.values[Index(id)];
    char now[16], low[16], high[16];
    ValueText(id, value, now, sizeof(now));
    ValueText(id, info.min, low, sizeof(low));
    ValueText(id, info.max, high, sizeof(high));
    const float span = float(info.max - info.min);
    float limit = 1;
    if (id == Id::Volume) limit = float(view.state.values[Index(Id::VolumeLimit)] - info.min) / span;
    valuePage(texts[kSelectSaves], now, low, high, float(value - info.min) / span, limit);
}

void PaintAbout(const View& view, const char* const* texts)
{
    using namespace launcher;
    static const AboutInfo kNothing;
    const AboutInfo& about = view.about != nullptr ? *view.about : kNothing;
    char flash[16], psram[16], total[16], free[16];
    SizeText(about.flashBytes, flash, sizeof(flash));
    SizeText(about.psramBytes, psram, sizeof(psram));
    SizeText(about.cardBytes, total, sizeof(total));
    SizeText(about.cardFree, free, sizeof(free));
    const String none = texts[kNone];
    std::vector<AboutRow> rows = {
        {texts[kAboutLauncher], about.launcher[0] != 0 ? String(about.launcher) : none},
        {"HAL", about.hal[0] != 0 ? String(about.hal) : none},
        {"MAC", about.mac},
        {texts[kAboutFlash], flash},
        {"PSRAM", about.psramBytes != 0 ? String(psram) : none},
        {texts[kAboutCard], about.card ? Fill(texts[kAboutFree], total, free) : none},
    };
    aboutPage(about.firmware, Fill(texts[kVersion], about.version), rows);
}

}  // namespace

void PaintGrey(const View& view, launcher::ListSlide* slide, uint32_t now, int y, int rows, uint8_t* grey)
{
    using namespace launcher;
    Lcd canvas;
    canvas.band(grey, y, rows);
    tft = &canvas;
    const int language = std::clamp(view.language, 0, kLanguageCount - 1);
    i18nSetLanguage(i18nLanguageFromCode(kLanguageCodes[language]));
    const char* const* texts = kTexts[language];
    const State& state = view.state;
    const char* title = texts[kTitle];
    switch (state.page)
    {
        case Page::Main:
        {
            std::vector<Option> list = {
                {texts[kContinue], view.firmware, ""},
                {texts[kSettings], texts[kSettingsDetail], ""},
                {texts[kLauncher], texts[kLauncherDetail], ""},
                {texts[kRestart], texts[kRestartDetail], ""},
                {texts[kOff], texts[kOffDetail], ""},
                {texts[kAbout], texts[kAboutDetail], ""},
                {texts[kDownload], texts[kDownloadDetail], ""},
            };
            const int selected = std::clamp(state.main.selected, 0, kItemCount - 1);
            if (view.releasePrompt) list[selected].description = texts[kRelease];
            menuFrame(list, float(selected), float(state.main.scroll), slide, selected, now);
            break;
        }
        case Page::Settings:
        {
            const int selected = std::clamp(state.settings.selected, 0, kRowCount - 1);
            menuFrame(SettingsRows(view, texts), float(selected), float(state.settings.scroll), slide, selected, now);
            title = texts[kSettings];
            break;
        }
        case Page::Language:
        {
            const int selected = std::clamp(state.language.selected, 0, kLanguageCount - 1);
            menuFrame(LanguageRows(texts), float(selected), float(state.language.scroll), slide, selected, now);
            title = texts[kLanguage];
            break;
        }
        case Page::Value:
        {
            // The rail's titles: the setting's name, shortened where the rail has no room for it.
            static constexpr TextId kTitles[Settings::kCount] = {kVolume, kVolumeLimitRail, kScreenRail, kKeys, kLanguage};
            PaintValue(view, texts);
            title = texts[kTitles[Index(state.editing)]];
            break;
        }
        case Page::Reset:
            drawDialog(texts[kResetQuestion], texts[kResetExplain], texts[kResetButton], texts[kCancel], state.button);
            title = texts[kResetButton];
            break;
        case Page::About:
            PaintAbout(view, texts);
            title = texts[kAboutRail];
            break;
    }
    status(title, view.backHeld);
    tft = nullptr;
}

}  // namespace CardenzaHAL::SystemMenu::detail
