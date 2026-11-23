#include "screens.h"

#include <cstdio>
#include <cstring>

#include "canvas.h"
#include "font.h"

namespace MicCheck {

namespace {

constexpr int kMargin = 8;
constexpr int kFooterY = 119;   // the key hints' row
constexpr int kCapHeight = 15;
constexpr float kMeterFloorDb = -90.0f;

// "30 厘米"; "" for the step without a distance.
const char* DistanceWords(const char* distance)
{
    if (std::strcmp(distance, "10cm") == 0) return "10 厘米";
    if (std::strcmp(distance, "30cm") == 0) return "30 厘米";
    if (std::strcmp(distance, "1m") == 0) return "1 米";
    if (std::strcmp(distance, "2m") == 0) return "2 米";
    return "";
}

// "距离 30 厘米"; for the step without a distance, that the person need not move.
void DistanceLine(char* out, std::size_t size, const char* distance)
{
    const char* words = DistanceWords(distance);
    if (*words == 0) std::snprintf(out, size, "不用动");
    else std::snprintf(out, size, "距离 %s", words);
}

const char* Title(Sound sound)
{
    switch (sound)
    {
    case Sound::Quiet: return "保持安静";
    case Sound::Soft: return "轻声说话";
    case Sound::Normal: return "正常说话";
    case Sound::Loud: return "大声说话";
    case Sound::Clap: return "拍手";
    case Sound::Music: return "手机外放音乐";
    }
    return "";
}

const char* HintWords(Sound sound)
{
    switch (sound)
    {
    case Sound::Quiet: return "测底噪：5 秒都不要出声";
    case Sound::Soft: return "轻声数数，像说悄悄话";
    case Sound::Normal: return "正常数数，像平时聊天";
    case Sound::Loud: return "大声数数，像隔着房间喊人";
    case Sound::Clap: return "每秒拍一下手";
    case Sound::Music: return "手机放歌，平时听歌的音量";
    }
    return "";
}

const char* ShortWords(Sound sound)
{
    switch (sound)
    {
    case Sound::Quiet: return "安静";
    case Sound::Soft: return "轻声";
    case Sound::Normal: return "正常";
    case Sound::Loud: return "大声";
    case Sound::Clap: return "拍手";
    case Sound::Music: return "音乐";
    }
    return "";
}

const char* CardWords(CardState card)
{
    switch (card)
    {
    case CardState::None: return "无 SD 卡";
    case CardState::Ready: return "SD 卡";
    case CardState::Saving: return "存卡中…";
    case CardState::Saved: return "已存卡";
    case CardState::Failed: return "存卡失败";
    }
    return "";
}

void DrawRight(uint16_t* frame, const Font& font, int right, int y, const char* text, uint16_t colour)
{
    DrawText(frame, font, right - TextWidth(font, text), y, text, colour);
}

// A key cap: a light box with the key's name in black. Returns the x after it.
int KeyCap(uint16_t* frame, int x, int y, const char* key)
{
    const int width = TextWidth(kTiny, key) + 8;
    FillRect(frame, x + 1, y, width - 2, kCapHeight, kText);
    FillRect(frame, x, y + 1, width, kCapHeight - 2, kText);
    DrawText(frame, kTiny, x + 4, y + (kCapHeight - kTiny.height) / 2, key, kBlack);
    return x + width;
}

// A key and what it does, on the footer's row. Returns the x after it (with a gap).
int KeyHint(uint16_t* frame, int x, const char* key, const char* words)
{
    x = KeyCap(frame, x, kFooterY, key) + 4;
    x = DrawText(frame, kSmall, x, kFooterY + (kCapHeight - kSmall.height) / 2, words, kText);
    return x + 10;
}

int MeterX(float db, int x, int width)
{
    if (db < kMeterFloorDb) db = kMeterFloorDb;
    if (db > 0.0f) db = 0.0f;
    return x + static_cast<int>((db - kMeterFloorDb) / -kMeterFloorDb * width + 0.5f);
}

// The live level: a bar to the 50 ms level, a mark at the held peak, ticks every 20 dB.
void Meter(uint16_t* frame, int x, int y, int width, int height, float db, float peakDb)
{
    FillRect(frame, x, y, width, height, kShade);
    const int level = MeterX(db, x, width);
    FillRect(frame, x, y, level - x, height, kText);
    if (peakDb > kMeterFloorDb)
    {
        const int peak = MeterX(peakDb, x, width);
        FillRect(frame, peak - 1, y - 2, 2, height + 4, kWhite);
    }
    for (int tick = -80; tick <= 0; tick += 20)
    {
        const int at = MeterX(static_cast<float>(tick), x, width);
        FillRect(frame, at, y + height, 1, 3, kDim);
    }
}

void MeterScale(uint16_t* frame, int x, int y, int width)
{
    char label[8];
    for (int tick = -80; tick <= 0; tick += 20)
    {
        std::snprintf(label, sizeof(label), "%d", tick);
        const int at = MeterX(static_cast<float>(tick), x, width);
        int left = at - TextWidth(kTiny, label) / 2;
        if (left + TextWidth(kTiny, label) > x + width) left = x + width - TextWidth(kTiny, label);
        DrawText(frame, kTiny, left, y, label, kDim);
    }
}

void FormatDb(char* out, std::size_t size, float db)
{
    if (db <= kFloorDb) std::snprintf(out, size, "-inf");
    else std::snprintf(out, size, "%.1f", db);
}

// The live level as a line under the screens that wait: a bar and its value.
void LiveLevel(uint16_t* frame, const View& view, int y)
{
    DrawText(frame, kSmall, kMargin, y - 3, "电平", kDim);
    const int x = kMargin + 34;
    const int width = kWidth - x - kMargin - TextWidth(kSmall, "-100.0 dB") - 4;
    Meter(frame, x, y, width, 8, view.levelDb, view.peakHoldDb);
    char value[16];
    FormatDb(value, sizeof(value), view.levelDb);
    std::strncat(value, " dB", sizeof(value) - std::strlen(value) - 1);
    DrawRight(frame, kSmall, kWidth - kMargin, y - 3, value, kText);
}

void StepCounter(char* out, std::size_t size, int step) { std::snprintf(out, size, "第 %d 步 / 共 %d 步", step + 1, kStepCount); }

void DrawIntro(uint16_t* frame, const View& view)
{
    DrawText(frame, kLarge, kMargin, 6, "麦克风收音测试", kWhite);
    char line[64];
    std::snprintf(line, sizeof(line), "共 %d 步，每步录 %u 秒", kStepCount, static_cast<unsigned>(kTakeSeconds));
    DrawText(frame, kSmall, kMargin, 40, line, kText);
    DrawText(frame, kSmall, kMargin, 58, "站在板子旁，照屏幕上说的出声", kText);
    DrawText(frame, kSmall, kMargin, 76, "录原始声音，不加增益", kDim);
    DrawRight(frame, kTiny, kWidth - kMargin, 10, CardWords(view.card), kDim);
    LiveLevel(frame, view, 100);
    int x = KeyHint(frame, kMargin, "OK", "开始");
    if (view.canResume) KeyHint(frame, x, "ESC", "返回结果");
}

void DrawReady(uint16_t* frame, const View& view)
{
    const Step& step = kPlan[view.step];
    char line[64];
    StepCounter(line, sizeof(line), view.step);
    DrawText(frame, kSmall, kMargin, 2, line, kDim);
    DrawRight(frame, kTiny, kWidth - kMargin, 4, CardWords(view.card == CardState::Saved ? CardState::Ready : view.card),
              kDim);
    char distance[32];
    DistanceLine(distance, sizeof(distance), step.distance);
    DrawText(frame, kMedium, kMargin, 22, distance, kText);
    DrawText(frame, kLarge, kMargin, 46, Title(step.sound), kWhite);
    DrawText(frame, kSmall, kMargin, 78, HintWords(step.sound), kDim);
    LiveLevel(frame, view, 103);
    if (view.waiting)
    {
        KeyHint(frame, kMargin, "OK", "等存卡完就开始…");
        return;
    }
    int x = KeyHint(frame, kMargin, "OK", "开始录音");
    x = KeyHint(frame, x, "→", "跳过");
    if (view.step > 0) KeyHint(frame, x, "←", "上一步");
}

void DrawRecording(uint16_t* frame, const View& view)
{
    const Step& step = kPlan[view.step];
    char line[64];
    // A recording dot and the step.
    FillRect(frame, kMargin + 1, 5, 7, 7, kWhite);
    FillRect(frame, kMargin, 6, 9, 5, kWhite);
    DrawText(frame, kSmall, kMargin + 14, 2, "录音中", kWhite);
    std::snprintf(line, sizeof(line), "%d / %d", view.step + 1, kStepCount);
    DrawRight(frame, kSmall, kWidth - kMargin, 2, line, kDim);

    // The seconds left, big; what to do beside them.
    float left = static_cast<float>(kTakeSeconds) - view.elapsed;
    if (left < 0.0f) left = 0.0f;
    std::snprintf(line, sizeof(line), "%d", static_cast<int>(left + 0.999f));
    const int digitsRight = DrawText(frame, kDigits, kMargin, 20, line, kWhite);
    DrawText(frame, kSmall, digitsRight + 2, 20 + kDigits.height - kSmall.height, "秒", kDim);
    const int textX = 70;
    char distance[32];
    DistanceLine(distance, sizeof(distance), step.distance);
    DrawText(frame, kMedium, textX, 24, distance, kText);
    DrawText(frame, kMedium, textX, 48, Title(step.sound), kWhite);

    // How far the take is.
    const int barWidth = kWidth - 2 * kMargin;
    int done = static_cast<int>(view.elapsed / kTakeSeconds * barWidth + 0.5f);
    if (done > barWidth) done = barWidth;
    FillRect(frame, kMargin, 76, barWidth, 2, kFaint);
    FillRect(frame, kMargin, 76, done, 2, kWhite);

    // The level, large.
    Meter(frame, kMargin, 84, barWidth, 12, view.levelDb, view.peakHoldDb);
    MeterScale(frame, kMargin, 99, barWidth);

    char level[16];
    char peak[16];
    FormatDb(level, sizeof(level), view.levelDb);
    FormatDb(peak, sizeof(peak), view.peakHoldDb);
    const int x = KeyHint(frame, kMargin, "ESC", "取消");
    std::snprintf(line, sizeof(line), "%s  峰值 %s dB", level, peak);
    DrawRight(frame, kSmall, kWidth - kMargin, kFooterY + (kCapHeight - kSmall.height) / 2, line, kText);
    (void)x;
}

// A label and its value in one of the result's cells.
void Cell(uint16_t* frame, int x, int y, const char* label, const char* value, bool alarm)
{
    DrawText(frame, kSmall, x, y + kMedium.height - kSmall.height, label, kDim);
    const int valueRight = x + 104;
    if (alarm)
    {
        const int width = TextWidth(kMedium, value) + 6;
        FillRect(frame, valueRight - width + 3, y, width, kMedium.height, kWhite);
        DrawRight(frame, kMedium, valueRight, y, value, kBlack);
    }
    else
    {
        DrawRight(frame, kMedium, valueRight, y, value, kWhite);
    }
}

void DrawResult(uint16_t* frame, const View& view)
{
    const Step& step = kPlan[view.step];
    const TakeRecord& take = view.results->takes[view.step];
    const Levels& l = take.levels;
    char line[64];
    const char* distance = DistanceWords(step.distance);
    std::snprintf(line, sizeof(line), "第 %d 步 · %s%s%s", view.step + 1, distance, *distance != 0 ? " · " : "",
                  ShortWords(step.sound));
    DrawText(frame, kSmall, kMargin, 2, line, kDim);
    if (take.overflows != 0)
    {
        std::snprintf(line, sizeof(line), "丢了 %u 块", static_cast<unsigned>(take.overflows));
        DrawRight(frame, kSmall, kWidth - kMargin, 2, line, kWhite);
    }
    else
    {
        DrawRight(frame, kSmall, kWidth - kMargin, 2, CardWords(view.card), kDim);
    }
    FillRect(frame, kMargin, 20, kWidth - 2 * kMargin, 1, kFaint);

    char a[16];
    char b[16];
    const int left = kMargin;
    const int right = kWidth / 2 + 4;
    FormatDb(a, sizeof(a), l.rms);
    FormatDb(b, sizeof(b), l.peak);
    Cell(frame, left, 25, "RMS", a, false);
    Cell(frame, right, 25, "峰值", b, l.peak > -1.0f);
    FormatDb(a, sizeof(a), l.p95);
    FormatDb(b, sizeof(b), l.p10);
    Cell(frame, left, 47, "P95", a, false);
    Cell(frame, right, 47, "P10", b, false);
    FormatDb(a, sizeof(a), l.loud1s);
    std::snprintf(b, sizeof(b), "%u", static_cast<unsigned>(l.clipped));
    Cell(frame, left, 69, "最响1秒", a, false);
    Cell(frame, right, 69, "削波", b, l.clipped != 0);
    std::snprintf(a, sizeof(a), "%.1f", l.dc);
    std::snprintf(b, sizeof(b), "%.0f%%", l.fullScale);
    Cell(frame, left, 91, "直流", a, false);
    Cell(frame, right, 91, "满幅", b, false);

    int x = KeyHint(frame, kMargin, "OK", view.step + 1 < kStepCount ? "下一步" : "看结果");
    KeyHint(frame, x, "ESC", "重录");
    DrawRight(frame, kTiny, kWidth - kMargin, kFooterY + 2, "dBFS", kDim);
}

void DrawTable(uint16_t* frame, const View& view)
{
    const Results& results = *view.results;
    char line[64];
    const char* title = view.fromBoot ? "上次的结果" : view.canResume ? "结果" : "测试完成";
    std::snprintf(line, sizeof(line), "%s · 第 %u 轮", title, static_cast<unsigned>(results.run));
    DrawText(frame, kSmall, kMargin, 1, line, kWhite);
    const int last = view.tableFirst + kTableRows < kStepCount ? view.tableFirst + kTableRows : kStepCount;
    std::snprintf(line, sizeof(line), "%d-%d / %d", view.tableFirst + 1, last, kStepCount);
    DrawRight(frame, kTiny, kWidth - kMargin, 3, line, kDim);

    // Columns: the step's number and the numbers align right, the distance and the sound left. Whole dB here
    // (the log and each take's screen have tenths).
    constexpr int kStepRight = 21;
    constexpr int kDistanceX = 27;
    constexpr int kSoundX = 64;
    constexpr int kRight[] = {118, 146, 173, 203, 232};  // RMS, peak, P95, loudest second, clipped
    const char* headings[] = {"RMS", "峰值", "P95", "1秒", "削波"};
    const int headY = 17;
    DrawText(frame, kTiny, kDistanceX, headY, "距离", kDim);
    DrawText(frame, kTiny, kSoundX, headY, "声音", kDim);
    for (int column = 0; column < 5; ++column) DrawRight(frame, kTiny, kRight[column], headY, headings[column], kDim);
    FillRect(frame, kMargin, headY + kTiny.height + 1, kWidth - 2 * kMargin, 1, kFaint);

    constexpr int kRowHeight = 13;
    for (int row = 0; row < kTableRows && view.tableFirst + row < kStepCount; ++row)
    {
        const int step = view.tableFirst + row;
        const int y = headY + kTiny.height + 4 + row * kRowHeight;
        const TakeRecord& take = results.takes[step];
        const uint16_t ink = take.state == kMeasured ? kText : kDim;
        std::snprintf(line, sizeof(line), "%d", step + 1);
        DrawRight(frame, kTiny, kStepRight, y, line, kDim);
        DrawText(frame, kTiny, kDistanceX, y, kPlan[step].distance, ink);
        DrawText(frame, kTiny, kSoundX, y, ShortWords(kPlan[step].sound), ink);
        if (take.state != kMeasured)
        {
            DrawRight(frame, kTiny, kRight[4], y, take.state == kSkipped ? "跳过" : "未测", kDim);
            continue;
        }
        const Levels& l = take.levels;
        const float values[] = {l.rms, l.peak, l.p95, l.loud1s};
        for (int column = 0; column < 4; ++column)
        {
            if (values[column] <= kFloorDb) std::snprintf(line, sizeof(line), "-inf");
            else std::snprintf(line, sizeof(line), "%.0f", values[column]);
            DrawRight(frame, kTiny, kRight[column], y, line, kText);
        }
        std::snprintf(line, sizeof(line), "%u", static_cast<unsigned>(l.clipped));
        DrawRight(frame, kTiny, kRight[4], y, line, l.clipped != 0 ? kWhite : kText);
    }

    int x = kMargin;
    if (view.canResume) x = KeyHint(frame, x, "→", "继续");
    x = KeyHint(frame, x, "OK", "重新测试");
    x = KeyHint(frame, x, "↑↓", "滚动");
    if (!view.canResume)
    {
        // Where the takes are.
        if (view.card == CardState::None || view.card == CardState::Failed) std::snprintf(line, sizeof(line), "无卡");
        else std::snprintf(line, sizeof(line), "卡 run-%02d", view.run);
        DrawRight(frame, kTiny, kWidth - kMargin, kFooterY + 2, line, kDim);
    }
}

}  // namespace

void Draw(uint16_t* frame, const View& view)
{
    FillRect(frame, 0, 0, kWidth, kHeight, kBlack);
    switch (view.screen)
    {
    case Screen::Intro: DrawIntro(frame, view); break;
    case Screen::Ready: DrawReady(frame, view); break;
    case Screen::Recording: DrawRecording(frame, view); break;
    case Screen::Result: DrawResult(frame, view); break;
    case Screen::Table: DrawTable(frame, view); break;
    }
}

}  // namespace MicCheck
