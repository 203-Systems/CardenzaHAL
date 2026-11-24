// screenshot/logic.h: HOME + space (every order and window, the menu on release), what the firmware sees of space,
// the files' names, the upload queue, what the community's answers mean, the effect's timing and USB's header.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "screenshot/card.h"
#include "screenshot/logic.h"
#include "system/menu/launcher/motion.h"
#include "system/menu/logic.h"
#include "system/menu/words.inc"

using namespace CardenzaHAL::Screenshot::Logic;
namespace Card = CardenzaHAL::Screenshot::Card;
namespace Menu = CardenzaHAL::SystemMenu::detail;

uint32_t CardenzaHAL::SystemMenu::launcher::launcherMillis() { return 0; }

namespace {

// ---- HOME + space: a timeline polled every 10 ms -------------------------------------------------------------

struct Press {
    uint32_t down, up;  // ms; up == 0: still down at the end
};
struct Script {
    std::vector<Press> home, space;
    bool homeAtStart = false;  // HOME down when watching began
    uint32_t until = 3000;
};
struct Seen {
    std::vector<uint32_t> shots, menus;
};

bool Down(const std::vector<Press>& presses, uint32_t t)
{
    for (const Press& p : presses)
        if (t >= p.down && (p.up == 0 || t < p.up)) return true;
    return false;
}

Seen Run(const Script& script)
{
    HomeGesture gesture;
    gesture.Reset(script.homeAtStart, Down(script.space, 0), 0);
    Seen seen;
    for (uint32_t t = 10; t <= script.until; t += 10)
    {
        const bool home = script.homeAtStart ? (t < 500 || Down(script.home, t)) : Down(script.home, t);
        const GestureEvents events = gesture.Update(home, Down(script.space, t), false, t);
        if (events.screenshot) seen.shots.push_back(t);
        if (events.openMenu) seen.menus.push_back(t);
    }
    return seen;
}

void Expect(const char* name, const Script& script, std::size_t shots, std::size_t menus, uint32_t menuAt = 0)
{
    const Seen seen = Run(script);
    if (seen.shots.size() != shots || seen.menus.size() != menus || (menuAt != 0 && seen.menus.front() != menuAt))
    {
        std::printf("%s: %zu shots (want %zu), %zu menus (want %zu)", name, seen.shots.size(), shots, seen.menus.size(),
                    menus);
        if (!seen.menus.empty()) std::printf(", menu at %u (want %u)", seen.menus.front(), menuAt);
        std::printf("\n");
        assert(false);
    }
}

void TestGestureTable()
{
    constexpr uint32_t W = kComboWindowMs;
    // A short press of HOME alone: the menu, kComboWindowMs after it was let go.
    Expect("HOME tap", {{{100, 200}}, {}}, 0, 1, 200 + W);
    // HOME held, then space: a screenshot; HOME's release opens nothing.
    Expect("HOME then space", {{{100, 600}}, {{300, 400}}}, 1, 0);
    // Space held (long), then HOME: a screenshot.
    Expect("space held then HOME", {{{900, 1000}}, {{100, 1500}}}, 1, 0);
    // Both in the same moment.
    Expect("together", {{{100, 300}}, {{100, 300}}}, 1, 0);
    // Not overlapping, HOME first: space a little after HOME was let go (a stiff key).
    Expect("HOME tap, space 70 ms after", {{{100, 180}}, {{250, 320}}}, 1, 0);
    Expect("HOME tap, space just inside the window", {{{100, 180}}, {{180 + W - 10, 500}}}, 1, 0);
    // ... and too late: the menu (opened before space came), and space is the firmware's.
    Expect("HOME tap, space after the window", {{{100, 180}}, {{180 + W + 30, 600}}}, 0, 1, 180 + W);
    // Not overlapping, space first: HOME a little after space was let go.
    Expect("space tap, HOME 90 ms after", {{{150, 230}}, {{0, 60}}}, 1, 0);
    Expect("space tap, HOME just inside", {{{60 + W - 10, 300}}, {{0, 60}}}, 1, 0);
    Expect("space tap, HOME too late: a menu", {{{60 + W + 20, 400}}, {{0, 60}}}, 0, 1, 400 + W);
    // Held long: nothing on release; just under the limit: a menu.
    Expect("HOME 1.5 s", {{{100, 1600}}, {}}, 0, 0);
    Expect("HOME just short", {{{100, 100 + kShortPressMs - 10}}, {}}, 0, 1, 100 + kShortPressMs - 10 + W);
    Expect("HOME just long", {{{100, 100 + kShortPressMs}}, {}}, 0, 0);
    // A glitch on the pin is no press.
    Expect("glitch", {{{100, 120}}, {}}, 0, 0);
    // HOME held when watching began counts once let go: the next press is a press.
    {
        Script s{{{800, 900}}, {{100, 200}}};
        s.homeAtStart = true;  // down until 500
        Expect("HOME at start", s, 0, 1, 900 + W);
    }
    // Space during the held HOME of the start: nothing.
    {
        Script s{{}, {{200, 300}}};
        s.homeAtStart = true;
        Expect("space with HOME from the start", s, 0, 0);
    }
    // Every press of space while HOME stays down is a screenshot.
    Expect("three shots", {{{100, 1200}}, {{200, 260}, {500, 560}, {800, 860}}}, 3, 0);
    // A double tap of HOME (the second inside the window): one menu, after the second.
    Expect("double tap", {{{100, 160}, {250, 320}}, {}}, 0, 1, 320 + W);
    // A screenshot by a late space after a tap, then a second HOME tap: a menu for the second.
    Expect("shot then tap", {{{100, 160}, {1000, 1080}}, {{200, 260}}}, 1, 1, 1080 + W);
}

void TestGestureDetails()
{
    // The keyboard saw a claimed press of space too short for the poll.
    HomeGesture g;
    g.Reset(false, false, 0);
    assert(!g.Update(true, false, false, 10).screenshot);
    assert(g.Update(true, false, true, 20).screenshot);
    assert(!g.Update(false, false, false, 100).openMenu);
    for (uint32_t t = 110; t < 600; t += 10) assert(!g.Update(false, false, false, t).openMenu);

    // Space belongs to the system while HOME is down and for the window after it is let go.
    HomeGesture s;
    s.Reset(false, false, 0);
    assert(!s.SpaceIsSystems(false, 5));
    assert(s.SpaceIsSystems(true, 5));  // the pin, before the poll saw it
    (void)s.Update(true, false, false, 10);
    assert(s.SpaceIsSystems(false, 15));
    (void)s.Update(false, false, false, 100);
    assert(s.SpaceIsSystems(false, 100 + kComboWindowMs - 1));
    assert(!s.SpaceIsSystems(false, 100 + kComboWindowMs));
    // Not while a HOME from the start is still down.
    HomeGesture held;
    held.Reset(true, false, 0);
    assert(!held.SpaceIsSystems(true, 10));
    // homeDown: the press that starts (the firmware's frames are kept from now), once per press.
    HomeGesture h;
    h.Reset(false, false, 0);
    assert(h.Update(true, false, false, 10).homeDown);
    assert(!h.Update(true, false, false, 20).homeDown);
}

// ---- What the firmware sees of space ----------------------------------------------------------------------

void TestSpaceGate()
{
    // Default: the firmware's at once.
    SpaceGate g;
    assert(!g.Update(false, false, false, 0));
    assert(g.Update(true, false, false, 10));
    assert(g.Update(true, true, false, 20));  // HOME coming later does not take a press already given
    assert(!g.Update(false, false, false, 30));
    assert(!g.TakeClaimed());
    // The system's: hidden until let go, even when HOME has gone meanwhile.
    assert(!g.Update(true, true, false, 40));
    assert(g.TakeClaimed() && !g.TakeClaimed());
    assert(!g.Update(true, false, false, 50));
    assert(!g.Update(false, false, false, 60));
    assert(g.Update(true, false, false, 70));
    assert(!g.Update(false, false, false, 80));

    // Held back: a press waits for the window; HOME in it claims it.
    SpaceGate w;
    assert(!w.Update(true, false, true, 0));
    assert(!w.Update(true, false, true, 100));
    assert(!w.Update(true, true, true, 150));
    assert(w.TakeClaimed());
    assert(!w.Update(true, false, true, 400));
    assert(!w.Update(false, false, true, 410));
    // No HOME: the firmware gets it late, still down.
    assert(!w.Update(true, false, true, 500));
    assert(!w.Update(true, false, true, 500 + kComboWindowMs - 1));
    assert(w.Update(true, false, true, 500 + kComboWindowMs));
    assert(!w.Update(false, false, true, 800));
    // A tap shorter than the window: a tap of kTapMs, late.
    assert(!w.Update(true, false, true, 1000));
    assert(!w.Update(false, false, true, 1050));
    assert(w.Update(false, false, true, 1000 + kComboWindowMs));
    assert(w.Update(false, false, true, 1000 + kComboWindowMs + SpaceGate::kTapMs - 1));
    assert(!w.Update(false, false, true, 1000 + kComboWindowMs + SpaceGate::kTapMs));
    assert(!w.TakeClaimed());
    // A tap, then HOME inside the window: claimed, nothing reaches the firmware.
    assert(!w.Update(true, false, true, 2000));
    assert(!w.Update(false, false, true, 2040));
    assert(!w.Update(false, true, true, 2100));
    assert(w.TakeClaimed());
    assert(!w.Update(false, false, true, 2400));
}

// ---- Names -----------------------------------------------------------------------------------------------

void TestNames()
{
    char out[96];
    Stamp(0, out, sizeof(out));
    assert(std::strcmp(out, "1970-01-01_000000") == 0);
    Stamp(1795534212, out, sizeof(out));
    assert(std::strcmp(out, "2026-11-24_153012") == 0);
    Stamp(951868799, out, sizeof(out));
    assert(std::strcmp(out, "2000-02-29_235959") == 0);
    Stamp(4102358401, out, sizeof(out));
    assert(std::strcmp(out, "2099-12-31_000001") == 0);

    FirmwareTag("CardenzaSynth", out, sizeof(out));
    assert(std::strcmp(out, "CardenzaSynth") == 0);
    FirmwareTag("Cardenza Synth_2.0!", out, sizeof(out));
    assert(std::strcmp(out, "Cardenza-Synth-2-0-") == 0);
    FirmwareTag("", out, sizeof(out));
    assert(std::strcmp(out, "Firmware") == 0);
    FirmwareTag("鸣片", out, sizeof(out));
    assert(std::strcmp(out, "Firmware") == 0);
    FirmwareTag(nullptr, out, sizeof(out));
    assert(std::strcmp(out, "Firmware") == 0);
    FirmwareTag("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", out, sizeof(out));
    assert(std::strlen(out) == kTagMax);

    assert(DatedName(1795534212, 1, "CardenzaSynth", out, sizeof(out)));
    assert(std::strcmp(out, "2026-11-24_153012_CardenzaSynth.png") == 0);
    assert(DatedName(1795534212, 2, "Launcher", out, sizeof(out)));
    assert(std::strcmp(out, "2026-11-24_153012-2_Launcher.png") == 0);
    assert(NumberedName(42, "CardenzaSynth", out, sizeof(out)));
    assert(std::strcmp(out, "shot-0042_CardenzaSynth.png") == 0);
    assert(NumberedName(12345, "X", out, sizeof(out)) && std::strcmp(out, "shot-12345_X.png") == 0);
    assert(!NumberedName(1, "X", out, 8));

    assert(ShotNumber("shot-0042_CardenzaSynth.png") == 42);
    assert(ShotNumber("shot-12345_X.png") == 12345);
    assert(ShotNumber("shot-_X.png") == 0);
    assert(ShotNumber("shot-12X.png") == 0);
    assert(ShotNumber("2026-11-24_153012_X.png") == 0);
    assert(ShotNumber(".shot-0001_X.png.tmp") == 0);

    assert(ValidName("2026-11-24_153012_CardenzaSynth.png"));
    assert(ValidName("shot-0001_Launcher.png"));
    assert(!ValidName(".hidden.png"));
    assert(!ValidName("a b.png"));
    assert(!ValidName("x.PNG"));
    assert(!ValidName("../x.png"));
    assert(!ValidName("x/y.png"));
    assert(!ValidName(".png"));
    assert(!ValidName(std::string(81, 'a').append(".png").c_str()));

    assert(TempName("shot-0001_X.png", out, sizeof(out)) && std::strcmp(out, ".shot-0001_X.png.tmp") == 0);
    assert(ClockSet(1795534212) && !ClockSet(1000) && !ClockSet(kClockSetAfter - 1));
}

// ---- The queue ---------------------------------------------------------------------------------------------

void TestQueue()
{
    char line[96];
    assert(Queue::Line('Q', "shot-0001_X.png", line, sizeof(line)) == 18 && std::strcmp(line, "Q shot-0001_X.png\n") == 0);
    assert(Queue::Line('X', "shot-0001_X.png", line, sizeof(line)) == 0);
    assert(Queue::Line('Q', "../x.png", line, sizeof(line)) == 0);
    assert(Queue::Line('Q', "shot-0001_X.png", line, 10) == 0);

    const std::string text = "Q a1.png\nQ a2.png\nU a1.png\nQ a3.png\nS a2.png\nQ a4.png\nQ a4.png\nQ a5.png";  // torn last
    char first[96];
    std::size_t pending = 0;
    assert(Queue::FirstPending(text.data(), text.size(), first, sizeof(first), &pending));
    assert(std::strcmp(first, "a3.png") == 0 && pending == 2);  // a3, a4 (once); a5 torn
    char compact[256];
    const std::size_t n = Queue::Compact(text.data(), text.size(), compact, sizeof(compact));
    assert(std::string(compact, n) == "Q a3.png\nQ a4.png\n");

    // Uploaded, then queued again: pending again.
    const std::string again = "Q b.png\nU b.png\nQ b.png\n";
    assert(Queue::FirstPending(again.data(), again.size(), first, sizeof(first), &pending) && pending == 1);
    // Nothing pending; CRLF and junk lines are read past.
    const std::string done = "Q c.png\r\nU c.png\r\n# note\nZ d.png\n\n";
    assert(!Queue::FirstPending(done.data(), done.size(), first, sizeof(first), &pending) && pending == 0);
    assert(Queue::Compact(done.data(), done.size(), compact, sizeof(compact)) == 0);
    assert(!Queue::FirstPending("", 0, first, sizeof(first), &pending) && pending == 0);
}

// ---- Answers -------------------------------------------------------------------------------------------------

void TestClassify()
{
    assert(Classify(201, "") == Upload::Done);
    assert(Classify(200, nullptr) == Upload::Done);
    assert(Classify(400, "invalid") == Upload::Skip);
    assert(Classify(413, "too_large") == Upload::Skip);
    assert(Classify(409, "quota_full") == Upload::StopRun);
    assert(Classify(404, "not_found") == Upload::StopRun);
    assert(Classify(429, "rate_limited") == Upload::Later);
    assert(Classify(500, "") == Upload::Later);
    assert(Classify(0, "") == Upload::Later);
    assert(Classify(401, "signin_required") == Upload::SignedOut);
    assert(Classify(403, "banned") == Upload::SignedOut);
    assert(Classify(403, "forbidden") == Upload::StopRun);
}

// ---- The effect --------------------------------------------------------------------------------------------

void TestEffect()
{
    using namespace Effect;
    int covered = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) covered += Flash(0, x, y);
    assert(covered == 16);  // white at first
    covered = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) covered += Flash(kFlashMs / 2, x, y);
    assert(covered == 8);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) assert(!Flash(kFlashMs, x, y));
    // The flash only fades: never more white later.
    for (uint32_t t = 0; t + 10 < kFlashMs; t += 10)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) assert(Flash(t, x, y) || !Flash(t + 10, x, y));
    // The card: not before it comes, whole in the middle, gone after; never outside its box.
    const int cx = kCard.x + 5, cy = kCard.y + 5;
    assert(!Effect::Card(kCardInAtMs - 1, cx, cy));
    assert(Effect::Card(kCardInAtMs + kDissolveMs, cx, cy) && Effect::Card(1000, cx, cy));
    assert(!Effect::Card(kCardOutAtMs + kDissolveMs, cx, cy));
    assert(!Effect::Card(1000, kCard.x - 1, cy) && !Effect::Card(1000, cx, kCard.y + kCard.h));
    for (uint32_t t = kCardInAtMs; t + 10 < kCardInAtMs + kDissolveMs; t += 10)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) assert(!Effect::Card(t, cx + x, cy + y) || Effect::Card(t + 10, cx + x, cy + y));
    assert(kCard.y + kCard.h <= 135 && kCard.x + kCard.w <= 240 && kThumb.y + kThumb.h < kCard.y + kCard.h);
    assert(ThumbSourceX(59) < 240 && ThumbSourceY(33) == 134 && ThumbSourceY(0) == 2);
    assert(Moving(0) && Moving(kCardInAtMs + 1) && !Moving(1000) && Moving(kCardOutAtMs));
    assert(kDoneMs > kCardOutAtMs + kDissolveMs);
}

// The card in every language: greys only (the system's look), words on it, nothing outside its box; the screen
// composed of the firmware's pixels, the flash's white, the card's greys and the thumbnail.
void TestCard()
{
    using Effect::kCard;
    using Effect::kDoneMs;
    using Effect::kTextX;
    using Effect::kThumb;
    std::vector<uint8_t> grey(Card::kBytes);
    std::vector<uint16_t> firmware(240 * 135), thumb(60 * 34), out(240 * 135);
    for (std::size_t i = 0; i < firmware.size(); ++i) firmware[i] = static_cast<uint16_t>(0x07E0 | (i & 0x1F));  // green
    for (std::size_t i = 0; i < thumb.size(); ++i) thumb[i] = 0xF800;  // red
    for (int language = 0; language < Menu::kLanguageCount; ++language)
        for (Card::Words words : {Card::Words::Saved, Card::Words::Taken, Card::Words::NoCard, Card::Words::Failed,
                                  Card::Words::NoMemory, Card::Words::Unavailable})
        {
            Card::Draw(grey.data(), language, words, "2026-11-24_153012_CardenzaSynth.png", true);
            int ink = 0;
            for (int y = 0; y < Card::kRows; ++y)
                for (int x = 0; x < 240; ++x)
                {
                    const uint8_t g = grey[y * 240 + x];
                    const bool inThumb = kThumb.Contains(x, kCard.y + y);
                    if (!kCard.Contains(x, kCard.y + y)) continue;
                    if (x >= kTextX && g < 128 && !inThumb) ++ink;
                }
            assert(ink > 20);  // the words are there
        }
    Card::Draw(grey.data(), 0, Card::Words::Saved, "shot-0001_X.png", true);
    Card::Scene scene;
    scene.firmware = firmware.data();
    scene.thumb = thumb.data();
    scene.card = grey.data();
    // At the shot: white wherever the card is not yet.
    scene.t = 0;
    Card::Paint(scene, 0, 0, 240, 135, out.data());
    for (uint16_t p : out) assert(p == 0xFFFF);
    // Later: the firmware's pixels around the card, the thumbnail in it, greys on it.
    scene.t = 1000;
    Card::Paint(scene, 0, 0, 240, 135, out.data());
    for (int y = 0; y < 135; ++y)
        for (int x = 0; x < 240; ++x)
        {
            const uint16_t p = out[y * 240 + x];
            if (kThumb.Contains(x, y)) assert(p == 0xF800);
            else if (kCard.Contains(x, y)) assert(Menu::PanelNeutral(p));
            else assert(p == firmware[y * 240 + x]);
        }
    // A band painted alone is the same as the whole screen's rows.
    std::vector<uint16_t> band(240 * 8);
    Card::Paint(scene, 0, 88, 240, 8, band.data());
    for (int i = 0; i < 240 * 8; ++i) assert(band[i] == out[88 * 240 + i]);
    // Without a firmware frame, black around the card; after the end, the firmware alone.
    scene.firmware = nullptr;
    Card::Paint(scene, 0, 0, 240, 1, out.data());
    for (int x = 0; x < 240; ++x) assert(out[x] == 0);
    scene.firmware = firmware.data();
    scene.t = kDoneMs;
    Card::Paint(scene, 0, 0, 240, 135, out.data());
    for (std::size_t i = 0; i < out.size(); ++i) assert(out[i] == firmware[i]);
}

void TestUsbHeader()
{
    uint8_t out[128];
    const std::size_t n = Usb::Header(Usb::Format::Png, Usb::Saved::Saved, 240, 135, "shot-0001_X.png", 0x01020304, out, sizeof(out));
    assert(n == 8 + 1 + 15 + 4);
    const uint8_t head[9] = {'K', 1, 0, 1, 240, 0, 135, 0, 15};
    assert(std::memcmp(out, head, 9) == 0 && std::memcmp(out + 9, "shot-0001_X.png", 15) == 0);
    assert(out[24] == 4 && out[25] == 3 && out[26] == 2 && out[27] == 1);
    assert(Usb::Header(Usb::Format::Raw, Usb::Saved::NoCard, 240, 135, "", 64800, out, sizeof(out)) == 13);
    assert(out[2] == 1 && out[3] == 2 && out[8] == 0);
    assert(Usb::Header(Usb::Format::Raw, Usb::Saved::NoCard, 240, 135, "x.png", 0, out, 10) == 0);
}

}  // namespace

int main()
{
    TestGestureTable();
    TestGestureDetails();
    TestSpaceGate();
    TestNames();
    TestQueue();
    TestClassify();
    TestEffect();
    TestCard();
    TestUsbHeader();
    std::printf("screenshot logic: ok\n");
    return 0;
}
