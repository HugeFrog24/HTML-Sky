// ----------------------------------------------------------------------------
// HTML Path API
// A C++ port of the 'path' module of Node.js
// <https://github.com/nodejs/node/blob/v24.x/lib/path.js>
// ----------------------------------------------------------------------------

#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>

#include "htinternal.hpp"

#define HTiWin32PathSeparator L'\\'
#define HTiPosixPathSeparator L'/'

static std::wstring join(
  const std::vector<std::wstring> &array,
  wchar_t separator
) {
  if (array.empty())
    return L"";

  std::wstring result;
  size_t totalSize = 0;
  for (const auto &str: array) {
    totalSize += str.size() + 1;
  }
  result.reserve(totalSize);

  for (size_t i = 0; i < array.size(); ++i) {
    if (i > 0)
      result += separator;
    result += array[i];
  }

  return result;
}

static inline bool isWin32PathSeparator(
  wchar_t ch
) {
  return ch == HTiWin32PathSeparator;
}

static inline bool isPosixPathSeparator(
  wchar_t ch
) {
  return ch == HTiPosixPathSeparator;
}

static inline bool isPathSeparator(
  wchar_t ch
) {
  return isPosixPathSeparator(ch) || isWin32PathSeparator(ch);
}

static bool isWin32ReservedName(
  const std::wstring &path,
  size_t colonIndex
) {
  static const wchar_t *WINDOWS_RESERVED_NAMES[] = {
    L"CON", L"PRN", L"AUX", L"NUL",
    L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
    L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    L"COM\xb9", L"COM\xb2", L"COM\xb3",
    L"LPT\xb9", L"LPT\xb2", L"LPT\xb3",
    nullptr
  };

  auto devicePart = path.substr(0, colonIndex);
  for (size_t i = 0; i < devicePart.size(); i++)
    devicePart[i] = std::toupper(devicePart[i]);

  for (int i = 0; WINDOWS_RESERVED_NAMES[i]; i++) {
    if (devicePart == WINDOWS_RESERVED_NAMES[i])
      return true;
  }

  return false;
}

static inline bool isWin32DeviceRoot(
  wchar_t ch
) {
  return (ch >= L'A' && ch <= L'Z')
    || (ch >= L'a' && ch <= L'z');
}

// Resolves . and .. elements in a path with directory names.
static std::wstring normalizeString(
  const std::wstring &path,
  bool allowAboveRoot,
  wchar_t separator
) {
  std::wstring res(L"");
  size_t lastSegmentLength = 0
    , lastSlash = -1
    , dots = 0;
  wchar_t code = 0;

  for (size_t i = 0; i <= path.size(); i++) {
    if (i < path.size())
      code = path[i];
    else if (isPathSeparator(code))
      break;
    else
      code = L'/';

    if (isPathSeparator(code)) {
      if (lastSlash == i - 1 || dots == 1) {
        // NOOP
      } else if (dots == 2) {
        if (
          res.size() < 2
          || lastSegmentLength != 2
          || res[res.size() - 1] != L'.'
          || res[res.size() - 2] != L'.'
        ) {
          if (res.size() > 2) {
            size_t lastSlashIndex = res.size() - lastSegmentLength - 1;
            if (lastSlashIndex == std::wstring::npos) {
              res = L"";
              lastSegmentLength = 0;
            } else {
              res = res.substr(0, lastSlashIndex);
              lastSegmentLength = res.size() - 1 - res.rfind(separator);
            }
            lastSlash = i;
            dots = 0;
            continue;
          } else if (res.size() != 0) {
            res = L"";
            lastSegmentLength = 0;
            lastSlash = i;
            dots = 0;
            continue;
          }
        }
        if (allowAboveRoot) {
          if (res.size() > 0)
            res += separator;
          res += L"..";
          lastSegmentLength = 2;
        }
      } else {
        lastSegmentLength = i - lastSlash - 1;
        if (res.size() > 0) {
          res += separator;
          res += path.substr(lastSlash + 1, lastSegmentLength);
        } else
          res = path.substr(lastSlash + 1, lastSegmentLength);
      }
      lastSlash = i;
      dots = 0;
    } else if (code == '.' && dots != std::wstring::npos) {
      dots++;
    } else {
      dots = std::wstring::npos;
    }
  }

  return res;
}

std::wstring HTiPathNormalize(
  const std::wstring &path
) {
  size_t len = wcslen(path.c_str());
  if (!len)
    return L".";

  size_t rootEnd = 0;
  std::wstring device;
  bool isAbsolute = false;
  wchar_t code = path[0];

  // Try to match a root.
  if (len == 1) {
    // `path` contains just a single char, exit early to avoid unnecessary work.
    return isPosixPathSeparator(code) ? L"\\" : path;
  }

  if (isPathSeparator(code)) {
    // Possible UNC root.
    //
    // If we started with a separator, we know we at least have an absolute
    // path of some kind (UNC or otherwise).
    isAbsolute = true;

    if (isPathSeparator(path[1])) {
      // Matched double path separator at beginning.
      size_t j = 2
        , last = j;
      // Match 1 or more non-path separators.
      while (
        j < len
        && !isPathSeparator(path[j])
      )
        j++;
      if (j < len && j != last) {
        std::wstring firstPart = path.substr(last, j - last);
        // Matched.
        last = j;
        // Match 1 or more path separators.
        while (
          j < len
          && isPathSeparator(path[j])
        )
          j++;
        if (j < len && j != last) {
          // Matched.
          last = j;
          // Match 1 or more non-path separators.
          while (
            j < len
            && !isPathSeparator(path[j])
          )
            j++;
        }
        if (j == len || j != last) {
          if (firstPart == L"." || firstPart == L"?") {
            // We matched a device root (e.g. \\\\.\\PHYSICALDRIVE0).
            device = L"\\\\" + firstPart;
            rootEnd = 4;
            size_t colonIndex = path.find(L":");
            // Special case: handle \\?\COM1: or similar reserved device paths.
            std::wstring possibleDevice = path.substr(4, colonIndex + 1 - 4);
            if (isWin32ReservedName(possibleDevice, possibleDevice.size() - 1)) {
              device = L"\\\\?\\" + possibleDevice;
              rootEnd = 4 + possibleDevice.size();
            }
          } else if (j == len) {
            // We matched a UNC root only.
            // Return the normalized version of the UNC root since there
            // is nothing left to process.
            return L"\\\\" + firstPart + L"\\" + path.substr(last) + L"\\";
          } else {
            // We matched a UNC root with leftovers.
            device = L"\\\\" + firstPart + L"\\" + path.substr(last, j - last) + L"\\";
            rootEnd = j;
          }
        }
      }
    } else {
      rootEnd = 1;
    }
  } else {
    size_t colonIndex = path.find(L":");
    if (colonIndex > 0) {
      if (isWin32DeviceRoot(code) && colonIndex == 1) {
        device = path.substr(0, 2);
        rootEnd = 2;
        if (len > 2 && isPathSeparator(path[2])) {
          isAbsolute = true;
          rootEnd = 3;
        }
      } else if (isWin32ReservedName(path, colonIndex)) {
        device = path.substr(0, colonIndex + 1);
        rootEnd = colonIndex + 1;
      }
    }
  }

  std::wstring tail = rootEnd < len
    ? normalizeString(
      path.substr(rootEnd),
      !isAbsolute,
      L'\\')
    : L"";
  if (tail.size() == 0 && !isAbsolute)
    tail = '.';
  if (
    tail.size() > 0
    && isPathSeparator(path[len - 1])
  )
    tail += '\\';
  if (!isAbsolute && !device.size() && path.find(L':') != std::wstring::npos) {
    // If the original path was not absolute and if we have not been able to
    // resolve it relative to a particular device, we need to ensure that the
    // `tail` has not become something that Windows might interpret as an
    // absolute path. See CVE-2024-36139.
    if (
      tail.size() >= 2
      && isWin32DeviceRoot(tail[0])
      && tail[1] == L':') {
      return L".\\" + tail;
    }
    size_t index = path.find(L':');

    do {
      if (index == len - 1 || isPathSeparator(path[index + 1])) {
        return L".\\" + tail;
      }
      index = path.find(L':');
    } while (index != std::wstring::npos);
  }

  size_t colonIndex = path.find(L':');
  if (isWin32ReservedName(path, colonIndex)) {
    return L".\\" + (device.size() ? device : L"") + tail;
  }
  if (!device.size()) {
    return isAbsolute ? L"\\" + tail : tail;
  }
  return isAbsolute ? device + L"\\" + tail : device + tail;
}

std::wstring HTiPathJoin(
  const std::vector<std::wstring> &args
) {
  if (!args.size())
    return std::wstring{L"."};

  std::wstring firstPart = args[0]
    , joined = join(args, L'\\');

  // Make sure that the joined path doesn't start with two slashes, because
  // HTiPathNormalize() will mistake it for a UNC path then.
  //
  // This step is skipped when it is very clear that the user actually
  // intended to point at a UNC path. This is assumed when the first
  // non-empty string arguments starts with exactly two slashes followed by
  // at least one more non-slash character.
  //
  // Note that for HTiPathNormalize() to treat a path as a UNC path it needs to
  // have at least 2 components, so we don't filter for that here.
  // This means that the user can use join to construct UNC paths from
  // a server name and a share name; for example:
  //
  // std::vector<const wchar_t *> args = {
  //   L"//server", L"share"
  // };
  // HTiPathJoin(args); -> L"\\\\server\\share\\"
  bool needsReplace = true;
  size_t slashCount = 0;
  if (isPathSeparator(firstPart[0])) {
    slashCount++;
    size_t firstLen = firstPart.size();
    if (
      firstLen > 1
      && isPathSeparator(firstPart[1])
    ) {
      slashCount++;
      if (firstLen > 2) {
        if (isPathSeparator(firstPart[2]))
          slashCount++;
        else {
          // We matched a UNC path in the first part.
          needsReplace = false;
        }
      }
    }
  }
  if (needsReplace) {
    // Find any more consecutive slashes we need to replace.
    while (
      slashCount < joined.size()
      && isPathSeparator(joined[slashCount])
    )
      slashCount++;

    // Replace the slashes if needed.
    if (slashCount >= 2)
      joined = L"\\" + joined.substr(slashCount);
  }

  // Skip normalization when reserved device names are present.
  std::vector<std::wstring> parts;
  std::wstring part;

  for (size_t i = 0; i < joined.size(); i++) {
    if (joined[i] == L'\\') {
      if (part.size()) parts.push_back(part);
      part.clear();
      // Skip consecutive backslashes.
      while (i + 1 < joined.size() && joined[i + 1] == L'\\') i++;
    } else {
      part += joined[i];
    }
  }
  // Add the final part if any.
  if (part.size()) parts.push_back(part);

  // Check if any part has a Windows reserved name.
  if (std::any_of(parts.begin(), parts.end(), [](const std::wstring &p) -> bool {
    size_t colonIndex = p.find(L':');
    return colonIndex != std::wstring::npos && isWin32ReservedName(p, colonIndex);
  })) {
    // Replace forward slashes with backslashes.
    std::wstring result;
    for (auto &element: joined) {
      result += element == L'/' ? L'\\' : element;
    }
    return result;
  }

  return HTiPathNormalize(joined);
}

bool HTiPathIsAbsolute(
  const std::wstring &path
) {
  if (path.empty())
    return false;

  auto code = path[0];
  return isPathSeparator(code)
    || (
      path.size() > 2
      && isWin32DeviceRoot(code)
      && path[1] == L':'
      && isPathSeparator(path[2])
    );
}
