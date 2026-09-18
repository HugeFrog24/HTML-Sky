// ----------------------------------------------------------------------------
// Which directory holds htmodloader\ - the mod root decision, on its own.
//
// Split out of initPaths() because of what it decides rather than how complex
// it is. Mods keep their credentials under this path, so choosing the wrong one
// does not present as "the loader looked in the wrong place" - it presents as a
// player's account being gone. That deserves a test, and initPaths() cannot
// have one: it is static, it writes six globals, and every input it reads comes
// from GetModuleFileNameW.
//
// So the rule lives here as a pure function with the filesystem injected, and
// initPaths() is left holding only the Win32 plumbing.
// ----------------------------------------------------------------------------
#pragma once

#include <string>

namespace ModRoot {

enum Source {
  kFromGameExe = 0,  // the historical location, and still the default
  kFromLoader = 1,   // a loader running outside the game folder, opted in
};

struct Choice {
  std::wstring root;  // full path to the htmodloader folder
  Source source;
};

typedef int (*FolderExistsFn)(const wchar_t *path);

// Prefer htmodloader\ beside the LOADER, but only when it already exists.
//
// The existence test IS the opt-in, and that is the whole point. Preferring the
// loader's own folder unconditionally would give a loader staged outside the
// game folder an EMPTY mod root, and an empty mod root is indistinguishable at
// a glance from a fresh account. Requiring the folder to be there already makes
// that switch something a person performed, not something the loader did to
// them on an ordinary launch.
//
// Installed, the two candidates are the same directory, so this decides nothing
// and no existing install changes behaviour.
inline Choice Choose(
  const std::wstring &loaderDir,
  const std::wstring &gameExeDir,
  FolderExistsFn folderExists
) {
  const std::wstring beside = loaderDir + L"\\htmodloader";
  if (folderExists && folderExists(beside.c_str())) {
    Choice c;
    c.root = beside;
    c.source = kFromLoader;
    return c;
  }
  Choice c;
  c.root = gameExeDir + L"\\htmodloader";
  c.source = kFromGameExe;
  return c;
}

}  // namespace ModRoot
