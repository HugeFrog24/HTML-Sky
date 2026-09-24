// ----------------------------------------------------------------------------
// DLL entry point and initializer of HT's Mod Loader.
// ----------------------------------------------------------------------------
#include <windows.h>
#include <ntstatus.h>
#include <stdio.h>
#include <unordered_map>
#include "MinHook.h"

#include "proxy/winhttp-proxy.h"
#include "utils/texts.h"
#include "htinternal.hpp"
#include "modroot.hpp"

static HMODULE hWinHttp;

/**
 * Get path to the dll and the layer config file.
 */
// Append `tail` to `dst`, or fail rather than truncate a path. The code this
// replaces used strcat() into MAX_PATH buffers under a comment that simply
// assumed the result would fit.
static i32 appendW(
  wchar_t *dst,
  const wchar_t *tail
) {
  if (wcslen(dst) + wcslen(tail) + 1 > MAX_PATH)
    return 0;
  wcscat(dst, tail);
  return 1;
}

// Narrow copy of a wide path in the active code page, for the two consumers
// that genuinely need one: the public HTGetModFolder ABI, and the ANSI
// RegEnumValueA hook. Deliberately produces the same bytes GetModuleFileNameA
// used to, so neither contract changes.
static void narrowFromWide(
  char *dst,
  const wchar_t *src
) {
  WideCharToMultiByte(CP_ACP, 0, src, -1, dst, MAX_PATH, "?", nullptr);
}

// Whether the mod root was taken from the loader's own folder rather than the
// game's. Recorded rather than logged, because initPaths() runs before the
// logger exists - and this is the one fact worth reporting afterwards, since a
// mod root in an unexpected place looks exactly like a fresh install.
static i32 gModRootBesideLoader = 0;

// The filesystem, as ModRoot::Choose() wants it. A thin adapter so the rule can
// be tested against a table of directories instead of a disk.
static int modRootFolderExists(
  const wchar_t *path
) {
  return HTiFolderExists(path) ? 1 : 0;
}

static i32 initPaths(
  HMODULE hModule
) {
  wchar_t *p;
  wchar_t tmp[MAX_PATH];

  // Wide first; the narrow paths are derived from these.
  //
  // It used to be the other way round, and that lost the path outright on any
  // machine whose game folder is not representable in its own ANSI code page.
  // GetModuleFileNameA substitutes '?' for every character it cannot encode,
  // and '?' is not a legal filename character, so "C:\<cyrillic>\Sky" became
  // "C:\????\Sky" - a path that cannot exist, with every later file operation
  // failing for a reason nothing reported. Splitting with strrchr() was unsafe
  // for a second reason: it is not DBCS-aware, and on a double-byte code page
  // a trail byte can equal '\\' (U+8868 encodes as 95 5C in CP932), so the
  // split could land inside a character.
  if (!GetModuleFileNameW(hModule, gPathDllWide, MAX_PATH))
    return 0;
  if (!GetModuleFileNameW(nullptr, gPathGameExeWide, MAX_PATH))
    return 0;

  p = wcsrchr(gPathDllWide, L'\\');
  if (!p)
    return 0;
  *p = 0;

  p = wcsrchr(gPathGameExeWide, L'\\');
  if (!p)
    return 0;
  *p = 0;

  // The rule, and why it is the way it is, lives in modroot.hpp.
  {
    const ModRoot::Choice choice = ModRoot::Choose(
      gPathDllWide, gPathGameExeWide, modRootFolderExists);
    // appendW() used to be what kept this inside MAX_PATH. Choose() returns a
    // std::wstring and cannot know about the fixed buffer, so the bound is
    // re-imposed here rather than quietly lost in the refactor.
    if (choice.root.size() >= MAX_PATH)
      return 0;
    wcscpy(gPathDataWide, choice.root.c_str());
    gModRootBesideLoader = (choice.source == ModRoot::kFromLoader);
  }

  wcscpy(gPathModsWide, gPathDataWide);
  if (!appendW(gPathModsWide, L"\\mods"))
    return 0;

  narrowFromWide(gPathDll, gPathDllWide);
  narrowFromWide(gPathMods, gPathModsWide);

  // Create mod data folders.
  if (!HTiFolderExists(gPathDataWide))
    CreateDirectoryW(gPathDataWide, nullptr);
  if (!HTiFolderExists(gPathModsWide))
    CreateDirectoryW(gPathModsWide, nullptr);

  // ImGui uses UTF-8 codepage in paths, so we need the conversion below.
  wcscpy(tmp, gPathDataWide);
  if (!appendW(tmp, L"\\htmlgui.ini"))
    return 0;
  wcstoutf8(tmp, gPathGuiIni, MAX_PATH);

  return 1;
}

static DWORD WINAPI onAttach(
  LPVOID lpParam
) {
  HMODULE hModule = (HMODULE)lpParam;

  (void)hModule;

  // Enable mods after the menu is created.
  if (WaitForSingleObject(gEventGuiInit, 30000) == WAIT_TIMEOUT) {
    // The most annoying error message of hSC Plugin LOL :P
    // This error is considered "NEVER TRIGGERED".
    LOGEF("Gui init timed out after 30 seconds.\n");
    return 0;
  }

  return 0;
}

BOOL APIENTRY DllMain(
  HMODULE hModule,
  DWORD dwReason,
  LPVOID lpReserved
) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    gModLoaderHandle = hModule;

    // Build proxy dispatch table.
    hWinHttp = LoadLibraryA("C:\\Windows\\System32\\winhttp.dll");
    proxy_importFunctions(hWinHttp);

    if (!HTiBackendExpectProcess())
      // Not the correct game process, act as winhttp.dll.
      return TRUE;

    initPaths(hModule);

    // No log file and console by default.
#ifdef HTML_ENABLE_LOGGER
    // Absolute, under the mod root. A bare filename is resolved against the
    // process cwd, which Steam sets to the game folder - so the log landed in
    // Program Files even when everything else had been moved out of it, and it
    // did not follow a portable root. initPaths() has already created this
    // directory by the time we get here.
    {
      wchar_t logPath[MAX_PATH];
      wcscpy(logPath, gPathDataWide);
      wcscat(logPath, L"\\html-log.log");
      HTiInitLogger(logPath, 0);
    }
#endif
    LOGI("HTML attatched.\n");
    // Say where the mods and their data are being read from, and which of the
    // two candidate roots won. Mods keep credentials under this path, so a
    // silently different root is the difference between "my account is gone"
    // and "the loader looked somewhere else"; only one of those is a bug, and
    // the log has to be able to tell them apart.
    LOGI("Mod root: %ls (%s)\n", gPathModsWide,
         gModRootBesideLoader ? "beside the loader" : "beside the game exe");

    MH_Initialize();

    HTiBackendSetupAll();

    gEventGuiInit = CreateEventA(nullptr, 0, 0, nullptr);

    CreateThread(
      nullptr, 0, onAttach, (LPVOID)hModule, 0, nullptr);
  } else if (dwReason == DLL_PROCESS_DETACH) {
    // Forcely update all options.
    HTiOptionsUpdate(114514.1919810f);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    FreeLibrary(hWinHttp);
  }

  return TRUE;
}
