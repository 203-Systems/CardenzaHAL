// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/display.cpp at 264061de6d32.
// Do not edit: change it in the Launcher and copy it again.
// Included inside namespace CardenzaHAL::SystemMenu::launcher (see the files that include it).
// Changed here: labelLines is inline (included by more than one file).

struct LabelLines {
  String first, second;
  int count;
};
inline LabelLines labelLines(const String &label, int width, int scale) {
  if (textWidth(label, scale) <= width) return {label, "", 1};
  const UiLineFit fit = uiFitLine(label, width, scale);
  size_t split = 0, next = 0;
  for (size_t at = std::min(fit.fits, label.size() - 1); at > 0; --at)
    if (label[at] == ' ') { split = at; next = at + 1; break; }
    else if ((label[at] == '-' || label[at] == '_') && at + 1 <= fit.fits) { split = next = at + 1; break; }
  if (fit.breakAt > split) { split = fit.breakAt; next = fit.resumeAt; }
  if (!split) return {uiFitText(label, width, scale), "", 1};
  String second = i18nTail(label, next);
  if (textWidth(second, scale) > width)
    second = second.substring(0, uiFitLine(second, width - 18 * scale, scale).fits) + "...";
  return {label.substring(0, split), second, 2};
}
