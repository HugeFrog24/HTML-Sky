// ----------------------------------------------------------------------------
// tibik-wrapper - a Steam %command% wrapper that points the game's DLL search
// at a loader staged outside the game folder.
//
// WHY THIS EXISTS. The loader gets in by DLL search-order hijack: a winhttp.dll
// beside Sky.exe that the game's own import pulls in. Move that file elsewhere
// and there is nothing to hijack - and the obvious repair, having the Vulkan
// loader find the layer by environment, does not work. Sky.exe imports
// WINHTTP.dll!WinHttpCloseHandle, so by the time any layer runs, System32's
// copy already owns that base name. A mod importing from winhttp.dll then fails
// LoadLibraryW with 127 (ERROR_PROC_NOT_FOUND) before any loader code runs.
// Discovery was never the problem; BINDING was.
//
// SetDllDirectoryW fixes the binding. The configured directory is searched
// after the executable's own directory and before System32, and it applies to
// child processes created afterwards - so a parent can hand it to the game.
// A parent's LoadLibrary would NOT do: module handles are not inherited, the
// search directory is.
//
// Being the parent is what Steam's launch options buy. "wrapper.exe %command%"
// expands %command% to the command Steam would otherwise have run, so the
// wrapper is created first and the game becomes its child. (The env-var-prefix
// form, VAR=x %command%, is Linux/Proton only and is not this.)
//
// This is NOT a way to run the game without Steam. It works THROUGH Steam, and
// the game's own platform check passes precisely because Steam set the
// environment the wrapper inherits and passes on.
//
// Exit codes: 2 - nothing to run (%command% missing or unexpanded)
//             3 - CreateProcessW failed
//             otherwise the game's own exit code, passed through.
// ----------------------------------------------------------------------------
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace {

FILE *g_log = nullptr;

std::wstring LogPath() {
  wchar_t base[MAX_PATH] = {0};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return L"tibik-wrapper.log";
  return std::wstring(base) + L"\\tibik-wrapper.log";
}

void Say(const char *fmt, ...) {
  if (!g_log)
    return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(g_log, fmt, ap);
  std::fputc('\n', g_log);
  va_end(ap);
  // Flushed per line because the game may take the whole process down, and a
  // buffered tail is exactly the part that would have said why.
  std::fflush(g_log);
}

void SayEnv(const char *name) {
  char buf[4096];
  buf[0] = 0;
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    Say("  %-28s <unset>", name);
  else
    Say("  %-28s %s", name, buf);
}

// Skip n whitespace-separated tokens, honouring quotes, and return the rest.
//
// Taken from the raw command line rather than rebuilt from a parsed argv:
// Steam's expansion carries its own quoting, and re-joining parsed tokens would
// change it. The game must receive byte-for-byte what Steam intended.
std::wstring SkipTokens(const wchar_t *p, int n) {
  for (int i = 0; i < n && *p; ++i) {
    while (*p == L' ' || *p == L'\t')
      ++p;
    if (*p == L'"') {
      ++p;
      while (*p && *p != L'"')
        ++p;
      if (*p == L'"')
        ++p;
    } else {
      while (*p && *p != L' ' && *p != L'\t')
        ++p;
    }
    while (*p == L' ' || *p == L'\t')
      ++p;
  }
  return std::wstring(p);
}

// The first token, by the same rules, so "read a token" and "skip a token"
// cannot drift apart.
std::wstring FirstToken(const wchar_t *p) {
  while (*p == L' ' || *p == L'\t')
    ++p;
  const wchar_t *start = p;
  if (*p == L'"') {
    ++p;
    while (*p && *p != L'"')
      ++p;
    if (*p == L'"')
      ++p;
  } else {
    while (*p && *p != L' ' && *p != L'\t')
      ++p;
  }
  return std::wstring(start, p);
}

std::wstring Unquote(std::wstring s) {
  if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
    return s.substr(1, s.size() - 2);
  return s;
}

// Hand-rolled rather than PathRemoveFileSpecW, so this needs no shlwapi import.
std::wstring ParentDir(const std::wstring &path) {
  const size_t cut = path.find_last_of(L"\\/");
  return (cut == std::wstring::npos) ? std::wstring() : path.substr(0, cut);
}

bool FileExists(const std::wstring &path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Name everything in the directory about to join the search path.
//
// SetDllDirectory changes dependency resolution GENERALLY, not only for
// winhttp.dll: anything here can satisfy any by-name lookup the game or its
// mods make. Listing the contents makes that blast radius a fact in the log
// rather than an assumption, and a folder holding more than the loader is a
// finding worth seeing. Subdirectories are skipped because DLL search does not
// recurse - a portable mod root nested here cannot answer a lookup.
int LogDirectoryContents(const std::wstring &dir) {
  WIN32_FIND_DATAW fd;
  const std::wstring glob = dir + L"\\*";
  HANDLE h = FindFirstFileW(glob.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return -1;
  int files = 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    ++files;
    Say("    %ls  (%llu bytes)", fd.cFileName,
        ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return files;
}

}  // namespace

// wWinMain, not wmain: built with -mwindows so Steam does not flash a console
// on every launch. The command line is read from GetCommandLineW() regardless,
// so the unused parameters cost nothing.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Truncated every run, never appended. This file exists so support can ask
  // "what happened when you last launched", and a log that grows without bound
  // for the life of an install answers that worse, not better - the relevant
  // lines end up buried under every previous launch.
  const std::wstring logPath = LogPath();
  g_log = _wfopen(logPath.c_str(), L"w");

  SYSTEMTIME st;
  GetLocalTime(&st);
  Say("tibik-wrapper  %04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth,
      st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

  wchar_t self[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  Say("  self                         %ls", self);

  wchar_t cwd[MAX_PATH] = {0};
  GetCurrentDirectoryW(MAX_PATH, cwd);
  // The working directory Steam chose. Recorded and left alone: a wrapper that
  // changed it would break a game resolving its data relative to cwd.
  Say("  cwd                          %ls", cwd);
  Say("  full command line            %ls", GetCommandLineW());

  // Our own flags come BEFORE %command% in the launch option:
  //   tibik-wrapper.exe --loader-dir "C:\...\loader" %command%
  // Everything after them is the game's command, passed through verbatim.
  //
  // With no flag a "loader" folder beside the wrapper is used if present. That
  // is a DEVELOPMENT convenience only. A packaged launcher must always pass
  // --loader-dir explicitly, because the wrapper then lives in the app's
  // resources directory, which is replaced wholesale on every app update - a
  // staged loader sitting beside it would disappear under the user.
  const wchar_t *raw = GetCommandLineW();
  std::wstring loaderDir;
  const char *loaderDirFrom = "";
  int selfTokens = 1;  // argv[0]
  if (FirstToken(SkipTokens(raw, 1).c_str()) == L"--loader-dir") {
    loaderDir = Unquote(FirstToken(SkipTokens(raw, 2).c_str()));
    loaderDirFrom = "--loader-dir";
    selfTokens = 3;
  } else {
    const std::wstring beside = ParentDir(self) + L"\\loader";
    if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) {
      loaderDir = beside;
      loaderDirFrom = "folder beside the wrapper (development fallback)";
    }
  }

  const std::wstring child = SkipTokens(raw, selfTokens);
  Say("  %%command%% expanded to         %ls",
      child.empty() ? L"<EMPTY - Steam passed nothing>" : child.c_str());

  Say("  --- environment ---");
  // If these are present the wrapper inherited Steam's environment, which is
  // what the game's own platform check reads and what the child inherits next.
  SayEnv("SteamAppId");
  SayEnv("SteamGameId");
  SayEnv("SteamClientLaunch");
  SayEnv("VK_ADD_IMPLICIT_LAYER_PATH");
  SayEnv("DISABLE_HT_MOD_LOADER");

  if (child.empty()) {
    Say("  VERDICT: nothing to run. Either the launch option omitted");
    Say("           %%command%%, or Steam did not expand it.");
    if (g_log)
      std::fclose(g_log);
    return 2;
  }

  // The redirect, before CreateProcessW: the configured directory applies to
  // children created AFTERWARDS.
  if (!loaderDir.empty()) {
    Say("  --- DLL search redirect ---");
    Say("    loader dir                 %ls  (from %s)", loaderDir.c_str(),
        loaderDirFrom);
    if (!FileExists(loaderDir + L"\\winhttp.dll")) {
      // A search directory without the loader in it changes how every by-name
      // dependency resolves and buys nothing. Refusing beats setting it and
      // letting the run look like the redirect itself not working.
      Say("    REFUSING: no winhttp.dll there. Not setting the directory.");
      loaderDir.clear();
    }
  }

  if (loaderDir.empty()) {
    Say("  no loader dir - plain passthrough, nothing redirected");
  } else {
    Say("    contents:");
    const int n = LogDirectoryContents(loaderDir);
    if (n > 2)
      Say("    NOTE: %d files here. Each one can answer a by-name lookup for"
          " the game and everything it loads.", n);

    // The application directory is searched BEFORE the configured one, so a
    // loader left in the game folder wins and this redirect is inert. Named
    // here because otherwise a working run would prove nothing and a failing
    // one would be blamed on the wrong thing.
    const std::wstring gameDir = ParentDir(Unquote(FirstToken(child.c_str())));
    Say("    game dir                   %ls", gameDir.c_str());
    if (gameDir.empty() || FileExists(gameDir + L"\\winhttp.dll"))
      Say("    WARNING: winhttp.dll is ALSO in the game folder - that copy wins,"
          " so the staged loader is NOT what will load.");
    else
      Say("    game folder clear of winhttp.dll - portable mode is live");

    if (SetDllDirectoryW(loaderDir.c_str()))
      Say("    SetDllDirectoryW           ok");
    else
      Say("    SetDllDirectoryW           FAILED %lu", GetLastError());
  }

  std::wstring mutableCmd = child;  // CreateProcessW wants a writable buffer
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &si, &pi)) {
    Say("  CreateProcessW FAILED: %lu", GetLastError());
    if (g_log)
      std::fclose(g_log);
    return 3;
  }
  Say("  child started, pid %lu", pi.dwProcessId);

  // Steam watches the process it launched to decide when the game stopped, so
  // the wrapper stays alive for the game's whole lifetime. Exiting straight
  // after spawning would make Steam report the game as closed immediately.
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  GetLocalTime(&st);
  Say("  child exited %lu at %02d:%02d:%02d", code, st.wHour, st.wMinute,
      st.wSecond);
  if (g_log)
    std::fclose(g_log);
  return (int)code;
}
