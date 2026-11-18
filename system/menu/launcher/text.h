// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/text.h at 6c318426bec3.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

namespace CardenzaHAL::SystemMenu::launcher {

// Service text API, backed by the standard library; no Arduino runtime.

// Marks an English key for tools/i18n.py without translating it here; the
// text is translated where it is shown (platform/i18n.h: tr()).
#ifndef N_
#define N_(text) text
#endif

class String : public std::string {
public:
  using std::string::operator=;
  using std::string::operator+=;
  template <class T,
            std::enable_if_t<
                std::is_arithmetic_v<T> && !std::is_same_v<T, char>, int> = 0>
  String &operator+=(T v) {
    append(String(v));
    return *this;
  }
  String() = default;
  String(const char *s) : std::string(s ? s : "") {}
  String(const char *s, size_t n) : std::string(s, n) {}
  String(const std::string &s) : std::string(s) {}
  String(char c) : std::string(1, c) {}
  template <class T,
            std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char>,
                             int> = 0>
  explicit String(T value, unsigned base = 10) {
    if (base == 10)
      assign(std::to_string(value));
    else if (base >= 2 && base <= 36) {
      char buf[65];
      char *p = buf + sizeof(buf);
      uint64_t v = static_cast<uint64_t>(value);
      do {
        *--p = "0123456789abcdefghijklmnopqrstuvwxyz"[v % base];
        v /= base;
      } while (v);
      assign(p, buf + sizeof(buf));
    }
  }
  // Fixed-point text without printf: the 8 MB board (Cardenza) uses the
  // ROM's nano printf, which has no floating point.
  explicit String(double value, unsigned digits = 2) {
    digits = std::min(digits, 6u);
    double scale = 1;
    for (unsigned i = 0; i < digits; ++i) scale *= 10;
    const bool negative = value < 0;
    const unsigned long long scaled =
        static_cast<unsigned long long>((negative ? -value : value) * scale + 0.5);
    std::string text = std::to_string(scaled);
    if (digits) {
      if (text.size() <= digits) text.insert(0, digits + 1 - text.size(), '0');
      text.insert(text.size() - digits, ".");
    }
    assign((negative && scaled ? "-" : "") + text);
  }
  bool isEmpty() const { return empty(); }
  bool equals(const String &s) const { return compare(s) == 0; }
  size_t write(uint8_t c) {
    push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t *data, size_t n) {
    append(reinterpret_cast<const char *>(data), n);
    return n;
  }
  char charAt(size_t i) const { return i < size() ? at(i) : 0; }
  void setCharAt(size_t i, char c) {
    if (i < size())
      at(i) = c;
  }
  String substring(size_t start, size_t end = npos) const {
    if (end < start)
      std::swap(start, end);
    start = std::min(start, size());
    end = std::min(end, size());
    return substr(start, end - start);
  }
  int indexOf(char c, size_t from = 0) const { return index(find(c, from)); }
  int indexOf(const String &s, size_t from = 0) const {
    return index(find(s, from));
  }
  int lastIndexOf(char c, size_t from = npos) const {
    return index(rfind(c, from));
  }
  int lastIndexOf(const String &s) const { return index(rfind(s)); }
  bool startsWith(const String &s) const {
    return size() >= s.size() && compare(0, s.size(), s) == 0;
  }
  bool endsWith(const String &s) const {
    return size() >= s.size() && compare(size() - s.size(), s.size(), s) == 0;
  }
  bool equalsIgnoreCase(const String &s) const {
    return size() == s.size() &&
           std::equal(begin(), end(), s.begin(),
                      [](unsigned char a, unsigned char b) {
                        return std::tolower(a) == std::tolower(b);
                      });
  }
  void trim() {
    auto a = find_first_not_of(" \r\n\t");
    if (a == npos)
      clear();
    else
      assign(substr(a, find_last_not_of(" \r\n\t") - a + 1));
  }
  void toLowerCase() {
    for (char &c : *this)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  void toUpperCase() {
    for (char &c : *this)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  long toInt() const { return strtol(c_str(), nullptr, 10); }
  float toFloat() const { return strtof(c_str(), nullptr); }
  void remove(size_t start, size_t count = npos) {
    if (start < size())
      erase(start, count);
  }
  void replace(char a, char b) { std::replace(begin(), end(), a, b); }
  void replace(const String &a, const String &b) {
    if (a.empty())
      return;
    for (size_t p = 0; (p = find(a, p)) != npos; p += b.size())
      std::string::replace(p, a.size(), b);
  }
  bool concat(const char *s, size_t n) {
    append(s, n);
    return true;
  }
  bool concat(const String &s) {
    append(s);
    return true;
  }
  void getBytes(uint8_t *out, size_t n, size_t start = 0) const {
    if (!out || !n)
      return;
    size_t count = start >= size() ? 0 : std::min(n - 1, size() - start);
    if (count)
      memcpy(out, data() + start, count);
    out[count] = 0;
  }
  void toCharArray(char *out, size_t n, size_t start = 0) const {
    getBytes(reinterpret_cast<uint8_t *>(out), n, start);
  }

private:
  static int index(size_t p) { return p == npos ? -1 : static_cast<int>(p); }
};
inline String operator+(const String &a, const String &b) {
  String s(a);
  s += b;
  return s;
}
inline String operator+(const char *a, const String &b) {
  return String(a) + b;
}
inline String operator+(const String &a, const char *b) {
  return a + String(b);
}
inline String operator+(const String &a, char b) { return a + String(b); }

template <class T,
          std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, char>,
                           int> = 0>
inline String operator+(const String &s, T value) {
  return s + String(value);
}

}  // namespace CardenzaHAL::SystemMenu::launcher
