#include <cctype>
#include "htinternal.hpp"

// ----------------------------------------------------------------------------
// [SECTION] Internal utilities (anonymous namespace)
// ----------------------------------------------------------------------------

namespace {

std::string trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    ++start;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  return s.substr(start, end - start);
}

bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isNumericStr(const std::string &s) {
  if (s.empty())
    return false;
  for (char c : s)
    if (!isDigit(c))
      return false;
  return true;
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// [SECTION] HTiSemVer implementation
// ----------------------------------------------------------------------------

HTiSemVer::HTiSemVer(): major(0), minor(0), patch(0) { }

HTiSemVer::HTiSemVer(
  int major,
  int minor,
  int patch,
  const std::vector<std::string> &prerelease,
  const std::vector<std::string> &build)
  : major(major)
  , minor(minor)
  , patch(patch)
  , prerelease(prerelease)
  , build(build)
{ }

bool HTiSemVer::read(const std::string &version) {
  return parse(version, false);
}

bool HTiSemVer::parse(const std::string &input, bool loose) {
  std::string s = trim(input);
  if (s.empty())
    return false;

  // Remove optional leading 'v' or '='
  if (s[0] == 'v' || s[0] == 'V' || s[0] == '=') {
    s = s.substr(1);
    if (s.empty())
      return false;
  }

  size_t pos = 0;

  // Parse major.
  while (pos < s.size() && isDigit(s[pos]))
    ++pos;
  if (pos == 0)
    return false;
  major = std::stoi(s.substr(0, pos));
  if (pos >= s.size() || s[pos] != '.')
    return false;
  // Skip '.'
  pos++;

  // Parse minor.
  size_t start = pos;
  while (pos < s.size() && isDigit(s[pos]))
    pos++;
  if (pos == start)
    return false;
  minor = std::stoi(s.substr(start, pos - start));
  if (pos >= s.size() || s[pos] != '.')
    return false;
  // Skip '.'
  pos++;

  // Parse patch.
  start = pos;
  while (pos < s.size() && isDigit(s[pos]))
    ++pos;
  if (pos == start)
    return false;
  patch = std::stoi(s.substr(start, pos - start));

  prerelease.clear();
  build.clear();

  if (pos >= s.size())
    return true;

  // Parse prerelease identifiers.
  if (s[pos] == '-') {
    ++pos;
    start = pos;
    while (pos < s.size() && s[pos] != '+')
      ++pos;
    std::string preStr = s.substr(start, pos - start);
    if (preStr.empty())
      return false;

    size_t ppos = 0;
    while (ppos < preStr.size()) {
      size_t next = preStr.find('.', ppos);
      std::string part = preStr.substr(
        ppos,
        (next == std::string::npos) ? std::string::npos : next - ppos);
      if (part.empty())
        return false;

      // Validate allowed characters.
      for (char c : part) {
        if (!isDigit(c) && !isAlpha(c) && c != '-')
          return false;
      }

      // Numeric identifiers must not have leading zeros (except "0").
      if (isNumericStr(part) && part.size() > 1 && part[0] == '0')
        return false;
      prerelease.push_back(part);
      if (next == std::string::npos)
        break;
      ppos = next + 1;
    }
  }

  // Parse build metadata.
  if (pos < s.size() && s[pos] == '+') {
    pos++;
    std::string buildStr = s.substr(pos);
    if (buildStr.empty())
      return false;

    size_t ppos = 0;
    while (ppos < buildStr.size()) {
      size_t next = buildStr.find('.', ppos);
      std::string part = buildStr.substr(
        ppos,
        (next == std::string::npos) ? std::string::npos : next - ppos);
      if (part.empty())
        return false;

      for (char c : part) {
        if (!isDigit(c) && !isAlpha(c) && c != '-')
          return false;
      }
      build.push_back(part);

      if (next == std::string::npos)
        break;

      ppos = next + 1;
    }
  }

  return true;
}

std::string HTiSemVer::write() const {
  std::string result =
    std::to_string(major) + "." +
    std::to_string(minor) + "." +
    std::to_string(patch);

  if (!prerelease.empty()) {
    result += "-";
    for (size_t i = 0; i < prerelease.size(); ++i) {
      if (i > 0)
        result += ".";
      result += prerelease[i];
    }
  }

  if (!build.empty()) {
    result += "+";
    for (size_t i = 0; i < build.size(); ++i) {
      if (i > 0)
        result += ".";
      result += build[i];
    }
  }

  return result;
}

bool HTiSemVer::valid(
  const std::string &v,
  bool loose
) {
  HTiSemVer tmp;
  return tmp.parse(v, loose);
}
