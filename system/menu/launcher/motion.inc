// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/display.cpp at 6c318426bec3.
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
// The selection bar of a list (the dark bar behind the selected row) moves
// by its two edges (the user, 2026-11-18: its size changes on the way, its
// motion is not linear, it never fades out and back in). The edge on the side
// it goes leads, at once and fast (the Launcher's ease-out, kLeadMs); the
// other starts from rest and follows (kTrailMs): the bar stretches toward the
// new row and settles back to the row's height, at most kStretch of it
// longer. A press during the slide goes on from where the edges are, at the
// speed they have. Positions in list pixels (from the top of the first row,
// unscrolled). keep: where the rows jump to a new scroll in one frame (the
// Cardenza, loopMenu), the bar keeps its place on screen and slides on from
// there. One a list, kept by its loop; drawing aims it at the selected row
// every frame.
struct ListSlide {
  static constexpr uint32_t kLeadMs = 150, kTrailMs = 230;
  static constexpr float kStretch = 0.75f;
  // An edge: a cubic from `from` at `speed` (px/ms) to `to`, at rest when
  // `ms` have passed: speed 3 * distance / ms is the ease-out cubic (ease()),
  // 0 a smoothstep.
  struct Edge {
    float from = 0, to = 0, speed = 0;
    uint32_t time = 0, ms = 0;
    float part(uint32_t now) const { return now - time < ms ? float(now - time) / ms : 1.f; }
    float at(uint32_t now) const {
      const float p = part(now), q = 1 - p;
      return to + (from - to) * q * q * (1 + 2 * p) + speed * ms * p * q * q;
    }
    float velocity(uint32_t now) const {
      const float p = part(now), q = 1 - p;
      return p < 1 ? 6 * p * q * (to - from) / ms + speed * q * (1 - 3 * p) : 0.f;
    }
    // Toward target from x, never past it: the lead at the ease-out's speed,
    // the trail at the speed it has, if that goes toward the target.
    void go(float x, float target, bool lead, uint32_t duration, uint32_t now) {
      const float most = 3 * (target - x) / duration, v = velocity(now);
      speed = lead ? most : most > 0 ? std::clamp(v, 0.f, most) : std::clamp(v, most, 0.f);
      from = x;
      to = target;
      time = now;
      ms = duration;
    }
  };
  Edge top, bottom;
  float view = 0;                         // the scroll it was last drawn at
  bool placed = false, down = true;       // down: the bottom edge leads
  bool keep = true;                       // rows jump to a scroll: the bar stays on screen
  // The edges as drawn: the trailing one at most kStretch target heights behind.
  float topAt(uint32_t now) const {
    const float most = (bottom.to - top.to) * (1 + kStretch);
    return down ? std::max(top.at(now), bottom.at(now) - most) : top.at(now);
  }
  float bottomAt(uint32_t now) const {
    const float most = (bottom.to - top.to) * (1 + kStretch);
    return down ? bottom.at(now) : std::min(bottom.at(now), top.at(now) + most);
  }
  bool active(uint32_t now) const {
    return motion && (now - top.time < top.ms || now - bottom.time < bottom.ms);
  }
  // Every frame: the selected row's edges and the list's scroll (pixels).
  void aim(float t, float b, float scroll, uint32_t now) {
    if (keep && placed && scroll != view)
      for (Edge *edge : {&top, &bottom}) {
        edge->from += scroll - view;
        edge->to += scroll - view;
      }
    view = scroll;
    if (!placed || !motion) {
      top = {t, t};
      bottom = {b, b};
      placed = true;
      return;
    }
    if (t == top.to && b == bottom.to) return;
    const float x0 = topAt(now), x1 = bottomAt(now);
    down = t + b > x0 + x1;
    top.go(x0, t, !down, down ? kTrailMs : kLeadMs, now);
    bottom.go(x1, b, down, down ? kLeadMs : kTrailMs, now);
  }
};
