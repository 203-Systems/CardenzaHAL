// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/display.cpp at 264061de6d32.
// Do not edit: change it in the Launcher and copy it again.
// Included inside namespace CardenzaHAL::SystemMenu::launcher (see the files that include it).
// Changed here: ease is inline (here it is in a header, included by more than one file).

inline float ease(float p) {
  p = std::clamp(p, 0.f, 1.f);
  const float inverse = 1.f - p;
  return 1.f - inverse * inverse * inverse;
}
struct Tween {
  float start = 0, target = 0;
  uint32_t time = 0, duration = 260;
  float value() const {
    return start +
           (target - start) *
               ease(motion ? float(launcherMillis() - time) / duration : 1.f);
  }
  void to(float v) {
    start = value();
    target = v;
    time = launcherMillis();
  }
  bool active() const {
    return motion && launcherMillis() - time < duration && start != target;
  }
  // Straight to v, with no frames in between.
  void set(float v) {
    start = target = v;
    time = launcherMillis();
  }
};
// The Cardenza's lists change without moving anything (its panel has no
// tearing-effect line, so a highlight sliding over frames shows torn): the
// selection crossfades from row to row (kFadeMs, the Launcher's ease-out) and
// a scroll fades the rows out and the new ones in (kScrollMs each way). A new
// press in a fade starts from where it is. One fade a list, kept by its loop.
struct ListFade {
  static constexpr uint32_t kFadeMs = 120, kScrollMs = 45;
  // Highlights fading: a weight from 0 (none) to 1 (selected); a few at
  // most (key repeat), the faintest gives way.
  struct Row { int index; float from, to; };
  Row rows[4];
  int count = 0;
  uint32_t time = 0, scrollTime = 0;
  int shown = 0, target = 0; // the scroll drawn, and the one wanted
  float alphaFrom = 1;       // the rows' ink when the scroll's fade set out
  void reset(int index, int scroll) {
    rows[0] = {index, 1, 1};
    count = 1;
    shown = target = scroll;
    alphaFrom = 1;
  }
  float weightOf(const Row &row, uint32_t now) const {
    return row.from + (row.to - row.from) * ease(motion ? float(now - time) / kFadeMs : 1.f);
  }
  float weight(int index, uint32_t now) const {
    for (int i = 0; i < count; ++i)
      if (rows[i].index == index) return weightOf(rows[i], now);
    return 0;
  }
  void select(int index, uint32_t now) {
    int kept = 0, found = -1;
    for (int i = 0; i < count; ++i) {
      const Row row = {rows[i].index, weightOf(rows[i], now), float(rows[i].index == index)};
      if (row.from < 0.01f && !row.to) continue; // gone
      if (row.to) found = kept;
      rows[kept++] = row;
    }
    if (found < 0) {
      if (kept == 4) { // the faintest gives way
        int faintest = 0;
        for (int i = 1; i < 4; ++i)
          if (rows[i].from < rows[faintest].from) faintest = i;
        rows[faintest] = rows[--kept];
      }
      rows[kept++] = {index, 0, 1};
    }
    count = kept;
    time = now;
  }
  // The rows' ink (1 whole, 0 gone); a scroll switches to the new rows when
  // the old ones are gone, and they come in from there (no blank frame).
  float alpha(uint32_t now) {
    float part = motion ? std::min(1.f, float(now - scrollTime) / kScrollMs) : 1.f;
    if (shown != target) {
      if (part < 1) return alphaFrom * (1 - part);
      shown = target;
      scrollTime += kScrollMs; // the fade in started when the fade out ended
      alphaFrom = 0;
      part = motion ? std::min(1.f, float(now - scrollTime) / kScrollMs) : 1.f;
    }
    return alphaFrom + (1 - alphaFrom) * ease(part);
  }
  void scrollTo(int scroll, uint32_t now) {
    if (scroll == target) return;
    alphaFrom = alpha(now);
    target = scroll;
    scrollTime = now;
  }
  bool active(uint32_t now) {
    return motion && (now - time < kFadeMs || shown != target || now - scrollTime < kScrollMs);
  }
};
