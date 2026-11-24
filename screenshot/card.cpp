// The screenshot's effect (screenshot/card.h), on the 203 Launcher's drawing code. Pure.
#include "screenshot/card.h"

#include <algorithm>

#include "system/menu/launcher/lcd.h"
#include "system/menu/launcher/ui_layout.h"
#include "system/menu/logic.h"

namespace CardenzaHAL::Screenshot::Card {

namespace {

using Logic::Effect::kCard;
using Logic::Effect::kTextX;
using Logic::Effect::kThumb;
namespace launcher = SystemMenu::launcher;
namespace menu = SystemMenu::detail;

menu::TextId WordsId(Words words)
{
    switch (words)
    {
        case Words::Saved: return menu::kShotSaved;
        case Words::Taken: return menu::kShotTaken;
        case Words::NoCard: return menu::kShotNocard;
        case Words::Failed: return menu::kShotFailed;
        case Words::NoMemory: return menu::kShotNomemory;
        case Words::Unavailable: return menu::kShotUnavailable;
    }
    return menu::kShotTaken;
}

}  // namespace

void Draw(uint8_t* grey, int language, Words words, const char* name, bool thumb)
{
    using launcher::String;
    launcher::Lcd tft;
    tft.band(grey, kCard.y, kRows);
    language = std::clamp(language, 0, menu::kLanguageCount - 1);
    launcher::i18nSetLanguage(launcher::i18nLanguageFromCode(menu::kLanguageCodes[language]));
    const char* const* texts = menu::kTexts[language];
    // The Launcher's card: white, flat, a line of black around it so it reads over a white screen too.
    tft.greyRect(0, kCard.y, launcher::SCREEN_WIDTH, kRows, 0);
    tft.greyRect(kCard.x, kCard.y, kCard.w, kCard.h, 0);
    tft.greyRect(kCard.x + 1, kCard.y + 1, kCard.w - 2, kCard.h - 2, 255);
    // The shot's frame (its pixels are pasted in by Paint), or a grey box where there is none.
    tft.greyRect(kThumb.x - 1, kThumb.y - 1, kThumb.w + 2, kThumb.h + 2, 215);
    tft.greyRect(kThumb.x, kThumb.y, kThumb.w, kThumb.h, thumb ? 0 : 232);
    const int width = kCard.x + kCard.w - 8 - kTextX;
    tft.text(launcher::uiFitText(String(texts[WordsId(words)]), width, 1), kTextX, kCard.y + 10, 1, 0);
    const bool saved = words == Words::Saved || words == Words::Taken;
    const String second = saved ? String(name != nullptr ? name : "") : String(texts[menu::kShotNotsaved]);
    if (!second.isEmpty()) tft.text(launcher::uiFitText(second, width, 1), kTextX, kCard.y + 28, 1, 110);
}

void Paint(const Scene& scene, int x, int y, int width, int rows, uint16_t* out)
{
    const uint16_t white = menu::PanelGrey(255);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < width; ++c)
        {
            const int px = x + c, py = y + r;
            uint16_t pixel;
            if (Logic::Effect::Card(scene.t, px, py))
            {
                if (scene.thumb != nullptr && kThumb.Contains(px, py))
                    pixel = scene.thumb[(py - kThumb.y) * kThumb.w + (px - kThumb.x)];
                else
                    pixel = scene.card != nullptr ? menu::PanelGrey(scene.card[(py - kCard.y) * 240 + px]) : white;
            }
            else if (Logic::Effect::Flash(scene.t, px, py))
                pixel = white;
            else
                pixel = scene.firmware != nullptr ? scene.firmware[py * 240 + px] : 0;
            out[r * width + c] = pixel;
        }
}

}  // namespace CardenzaHAL::Screenshot::Card
