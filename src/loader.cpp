// ----------------------------------------------------------------------------
// Mod loader of HT's Mod Loader.
// ----------------------------------------------------------------------------
#include <cmath>
#include <algorithm>
#include "cJSON.h"

#include "includes/htmodloader.h"
#include "utils/texts.h"
#include "htinternal.hpp"
#include "modinspect.hpp"


// Scan all potential mods.
static void scanMods() {
  HANDLE hFindFile;
  WIN32_FIND_DATAW findData;
  std::wstring modsFolderPath(gPathModsWide);

  LOGI("Scanning mods...\n");

  modsFolderPath += L"\\*";
  hFindFile = FindFirstFileW(modsFolderPath.data(), &findData);
  // FindFirstFileW reports failure with INVALID_HANDLE_VALUE, never NULL, so
  // the old `!hFindFile` test could not fire. A missing or unreadable mods
  // folder therefore fell straight into the loop below with findData never
  // written - reading an uninitialised cFileName, and closing a handle that
  // was never opened.
  if (hFindFile == INVALID_HANDLE_VALUE)
    return;

  do {
    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      continue;
    if (!wcscmp(findData.cFileName, L".") || !wcscmp(findData.cFileName, L".."))
      continue;

    LOGI("Found potential mod folder: %ls\n", findData.cFileName);

    // Deciding what a mod is happens in exactly one place, ModInspect, which
    // the launcher's scanner is built from too. Reimplementing any of it here
    // would recreate the disagreement that shared core exists to remove - a mod
    // the loader runs and the launcher calls broken, or the reverse.
    const ModInspect::Result inspected =
      ModInspect::InspectFolder(gPathModsWide, findData.cFileName);
    if (inspected.outcome != ModInspect::Outcome::Ok) {
      LOGW(
        "Skipping %ls: %s%s%s\n",
        findData.cFileName,
        ModInspect::ToString(inspected.outcome),
        inspected.detail.empty() ? "" : " - ",
        inspected.detail.c_str());
      continue;
    }
    for (const ModInspect::Anomaly anomaly : inspected.anomalies) {
      // Cast to void because LOG*() compiles to nothing outside the debug
      // build, which leaves this loop body empty and the variable unused. The
      // anomalies are not the loader's business to act on - it accepted the mod
      // - they are for the scanner to hand the launcher.
      (void)anomaly;
      LOGW(
        "Mod folder %ls: %s\n", findData.cFileName, ModInspect::ToString(anomaly));
    }

    ModManifest manifest;
    manifest.paths.folder =
      HTiPathJoin({std::wstring(gPathModsWide), findData.cFileName});
    manifest.paths.dll = inspected.dllPath;
    manifest.meta.packageName = inspected.identity.packageName;
    // Already accepted by ModInspect::ParsesAsVersion, which is this same
    // parser - so this cannot fail without the two disagreeing.
    manifest.meta.version.read(inspected.identity.version);
    manifest.modName = inspected.identity.modName;
    manifest.description = inspected.identity.description;
    manifest.author = inspected.identity.author;
    manifest.gameEditionFlags = inspected.identity.gameEdition;
    for (const ModInspect::Dependency &dep : inspected.identity.dependencies) {
      ModDependency out;
      out.packageName = dep.packageName;
      out.constraint = dep.versionRange;
      manifest.dependencies.push_back(out);
    }

    if (!HTiBackendCheckEdition(manifest.gameEditionFlags))
      // Skip mods that not compatible with current game edition.
      continue;

    // When scanning mods, the mod data won't be accessed by multiple threads,
    // so we don't need to protect it.
    if (gModDataLoader.find(manifest.meta.packageName) != gModDataLoader.end()) {
      // Skip duplicated mods.
      LOGW("Duplicated package name %s, skipped.\n", manifest.meta.packageName.c_str());
      continue;
    }

    manifest.status = ModStatus_Ok;
    manifest.runtime = nullptr;
    gModDataLoader[manifest.meta.packageName] = manifest;

    LOGI("Scanned mod %s.\n", manifest.modName.data());
  } while (FindNextFileW(hFindFile, &findData));

  FindClose(hFindFile);
}

// Get all exported functions for the loader.
static void getModExportedFunctions(
  ModRuntime *runtimeData
) {
  HMODULE hMod = runtimeData->handle;
  runtimeData->loaderFunc.pfn_HTModRenderGui = (PFN_HTModRenderGui)GetProcAddress(
    hMod, "HTModRenderGui");
  runtimeData->loaderFunc.pfn_HTModOnInit = (PFN_HTModOnInit)GetProcAddress(
    hMod, "HTModOnInit");
  runtimeData->loaderFunc.pfn_HTModOnEnable = (PFN_HTModOnEnable)GetProcAddress(
    hMod, "HTModOnEnable");
}

// Visit state values for topological sort.
enum VisitStates {
  VS_UNVISITED = 0,
  VS_VISITING = 1,
  VS_DONE = 2,
  VS_DEAD = -1
};
 
// Recursively visit a mod and its dependencies for topological sorting.
// Adds resolved mods to `result` in dependency-first order.
// Returns false if the package should be discarded.
static bool visitMod(
  const std::string &pkg,
  std::map<std::string, int> &states,
  std::vector<ModManifest *> &result
) {
  int &state = states[pkg];
 
  if (state == VS_DONE)
    return true;
  if (state == VS_DEAD)
    return false;

  ModManifest &manifest = gModDataLoader[pkg];

  if (state == VS_VISITING) {
    // Back-edge detected: this package is part of a dependency cycle.
    LOGW("Dependency cycle detected at %s, discarding.\n", pkg.c_str());
    state = VS_DEAD;
    manifest.setStatus(ModStatus_CycleDep);
    return false;
  }
 
  state = VS_VISITING;

  for (const ModDependency &dep: manifest.dependencies) {
    auto it = gModDataLoader.find(dep.packageName);
 
    if (it == gModDataLoader.end()) {
      LOGW("Dependency %s not found for %s, discarding.\n",
        dep.packageName.c_str(),
        pkg.c_str());

      state = VS_DEAD;
      manifest.setStatus(ModStatus_MissingDep);

      return false;
    }
 
    if (
      !dep.constraint.empty()
      && !HTiSemVer::satisfies(it->second.meta.version, dep.constraint)
    ) {
      LOGW("Dependency %s version unsatisfied for %s, discarding.\n",
        dep.packageName.c_str(),
        pkg.c_str());

      state = VS_DEAD;
      manifest.setStatus(ModStatus_MismatchDep);

      return false;
    }
 
    if (!visitMod(dep.packageName, states, result)) {
      LOGW("Package %s discarded due to failed dependency %s.\n",
        pkg.c_str(),
        dep.packageName.c_str());

      state = VS_DEAD;
      manifest.setStatus(ModStatus_RemoveByDep);

      return false;
    }
  }
 
  state = VS_DONE;
  result.push_back(&manifest);

  return true;
}
 
// Construct the dependency tree and the mod loading order, remove invalid packages.
static std::vector<ModManifest *> resolveMods() {
  std::map<std::string, int> states;
  std::vector<ModManifest *> result;
 
  for (auto &pair: gModDataLoader)
    states[pair.first] = VS_UNVISITED;
 
  for (auto &pair: gModDataLoader) {
    if (states[pair.first] == VS_UNVISITED)
      visitMod(pair.first, states, result);
  }

  return result;
}

// Load all avaliable mods into the game process and register mod runtime data.
static void expandMods(
  const std::vector<ModManifest *> &order
) {
  HMODULE hMod;
  ModRuntime *runtimeData;
  std::wstring oldPath;

  for (auto mod: order) {
    const char *modName = mod->modName.c_str();

    (void)modName;

    if (mod->meta.packageName == HTTexts_ModLoaderPackageName)
      // The data of mod loader itself is set in bootstrap(), so we don't need
      // to load it again.
      continue;

    // Save previous dll directory.
    u32 needed = GetDllDirectoryW(0, nullptr);
    if (needed) {
      oldPath.resize(needed + 1);
      GetDllDirectoryW(needed + 1, oldPath.data());
    }

    // Set new dll searching directory.
    SetDllDirectoryW(mod->paths.folder.c_str());

    // Load library.
    hMod = LoadLibraryW(mod->paths.dll.c_str());
    if (hMod) {
      LOGI("Loaded mod %s.\n", modName);
    } else {
      // Report the real reason. "No such file" was printed for EVERY failure,
      // which is actively misleading for the most likely one: a mod built
      // against a newer loader fails with ERROR_PROC_NOT_FOUND because an
      // import cannot bind, and the file is sitting right there.
      DWORD err = GetLastError();
      mod->loadError = static_cast<unsigned long>(err);
      LOGW("Load mod %s failed: error %lu\n", modName, mod->loadError);

      // The enum has carried this value since before it meant anything. Now a
      // mod that cannot load stays in the list, named, with a reason - which
      // is the whole point of reading its metadata without loading it.
      mod->setStatus(ModStatus_DllErr);
    }

    // Restore saved dll searching directory.
    if (needed)
      SetDllDirectoryW(oldPath.c_str());
    else
      SetDllDirectoryW(nullptr);

    // Save runtime data.
    if (hMod) {
      // While the mods are loading one by one, subthreads created by the mods
      // may access mod data structs at the same time, so we need a global
      // lock.
      std::lock_guard<std::mutex> lock(gModDataLock);
      runtimeData = &gModDataRuntime[hMod];
      runtimeData->handle = hMod;
      runtimeData->manifest = mod;
      getModExportedFunctions(runtimeData);
      mod->runtime = runtimeData;

      HTiRegisterHandle(hMod, HTHandleType_Mod);

      HTiOptionsLoadFor(runtimeData);
    }
  }
}

// Call the HTModOnInit() functions exported by the mods one by one. Mods
// can only use HTAPI within and after the function is called.
static void initMods(
  const std::vector<ModManifest *> &order
) {
  PFN_HTModOnInit fn;

  for (auto mod: order) {
    const char *modName = mod->modName.c_str();
    (void)modName;

    if (!mod->runtime)
      continue;

    fn = mod->runtime->loaderFunc.pfn_HTModOnInit;

    // We assume that mod that does not export HTModOnInit() has an independent
    // initialization (or enabling, see HTiEnableMods() below) process, so we
    // consider that they are initialized successfully
    if (!fn || fn(nullptr) == HT_SUCCESS)
      LOGI("Initialized mod %s.\n", modName);
    else
      LOGW("Failed to initialize mod %s.\n", modName);
  }
}

HTStatus HTiLoadMods() {
  HTiBootstrap();
  scanMods();
  auto order = resolveMods();
  expandMods(order);
  initMods(order);

  return HT_SUCCESS;
}

HTStatus HTiEnableMods() {
  PFN_HTModOnEnable fn;

  for (auto it = gModDataRuntime.begin(); it != gModDataRuntime.end(); it++) {
    const char *modName = it->second.manifest->modName.c_str();
    (void)modName;

    fn = it->second.loaderFunc.pfn_HTModOnEnable;

    if (!fn || fn(nullptr) == HT_SUCCESS)
      LOGI("Enabled mod %s.\n", modName);
    else
      LOGW("Failed to enable mod %s.\n", modName);
  }

  return HT_SUCCESS;
}

HTStatus HTiInjectDll(
  const wchar_t *path
) {
  return HT_SUCCESS;
}
