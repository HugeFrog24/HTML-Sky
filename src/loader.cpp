// ----------------------------------------------------------------------------
// Mod loader of HT's Mod Loader.
//
// This build loads exactly one mod, Tibik. Upstream scans every folder under
// the mods root, resolves dependencies between the mods it finds and drops
// duplicates. With a single tenant that declares no dependencies all of that
// is machinery with nothing to do, so it is gone: what is left finds the
// tenant, loads it and calls its entry points.
// ----------------------------------------------------------------------------
#include <stdio.h>
#include "includes/htmodloader.h"
#include "utils/texts.h"
#include "htinternal.hpp"
#include "modinspect.hpp"

// The launcher installs a package into the folder named after it, so the
// tenant's folder under the mods root is its package name.
static const wchar_t *const kTenantFolder = L"" HTTexts_TenantPackageName;

// Record the tenant as not running, with the sentence the Mods tab shows for
// it. Without this entry a tenant that was never found leaves nothing behind,
// and the tab says nothing about the one mod it exists to report on.
//
// Keyed by the tenant's name, never by whatever the folder's DLL claims to be:
// one claiming the loader's own name would overwrite the loader's entry.
static void skipTenant(
  const char *problem
) {
  ModManifest &stored = gModDataLoader[HTTexts_TenantPackageName];
  stored.meta.packageName = HTTexts_TenantPackageName;
  stored.modName = HTTexts_TenantName;
  stored.setStatus(ModStatus_Skipped);
  stored.problem = problem;
  LOGW("Not loading %s: %s\n", HTTexts_TenantPackageName, problem);
}

// What the Mods tab says when ModInspect refuses the tenant's folder.
//
// Written here rather than taken from Result::detail, which is documented as
// never shown to a player. And none of them says what to do about it: the
// same folder holds the account key (identity.json), so a line that reads as
// "clear it out and start over" can cost the player the account.
//
// There is no default case, so -Wswitch names any Outcome added later that
// this does not cover.
static const char *refusedProblem(
  ModInspect::Outcome outcome
) {
  switch (outcome) {
  case ModInspect::Outcome::Ok:
    break;
  case ModInspect::Outcome::NoManifest:
    return "Not loaded: its folder has no mod in it.";
  case ModInspect::Outcome::Malformed:
    return "Not loaded: its details could not be read.";
  case ModInspect::Outcome::Ambiguous:
    return "Not loaded: more than one DLL in its folder claims to be the mod.";
  case ModInspect::Outcome::LimitExceeded:
    return "Not loaded: its details are too big to read.";
  case ModInspect::Outcome::IoError:
    return "Not loaded: its folder is missing or could not be read.";
  }
  return "Not loaded.";
}

// Find the tenant and record it for loading. Null when its folder is missing
// or unreadable, when what is there is not the tenant, or when it was built
// for another edition of the game - and in each of those cases the tenant is
// recorded as skipped instead, with the reason.
static ModManifest *findTenant() {
  LOGI("Looking for %s...\n", HTTexts_TenantPackageName);

  // Deciding what a mod is happens in exactly one place, ModInspect, which
  // the launcher's scanner is built from too. Reimplementing any of it here
  // would recreate the disagreement that shared core exists to remove - a mod
  // the loader runs and the launcher calls broken, or the reverse.
  const ModInspect::Result inspected =
    ModInspect::InspectFolder(gPathModsWide, kTenantFolder);
  if (inspected.outcome != ModInspect::Outcome::Ok) {
    LOGW(
      "Inspecting %ls: %s%s%s\n",
      kTenantFolder,
      ModInspect::ToString(inspected.outcome),
      inspected.detail.empty() ? "" : " - ",
      inspected.detail.c_str());
    skipTenant(refusedProblem(inspected.outcome));
    return nullptr;
  }
  for (const ModInspect::Anomaly anomaly : inspected.anomalies) {
    // Cast to void because LOG*() compiles to nothing outside the debug
    // build, which leaves this loop body empty and the variable unused. The
    // anomalies are not the loader's business to act on - it accepted the mod
    // - they are for the scanner to hand the launcher.
    (void)anomaly;
    LOGW("Mod folder %ls: %s\n", kTenantFolder, ModInspect::ToString(anomaly));
  }

  // The folder is the tenant's by convention and the identity inside it by
  // claim, and the two must agree. A DLL there claiming another package is
  // not the mod this loader serves.
  if (inspected.identity.packageName != HTTexts_TenantPackageName) {
    LOGW(
      "Mod folder %ls identifies as %s.\n",
      kTenantFolder,
      inspected.identity.packageName.c_str());
    skipTenant("Not loaded: its folder holds a different mod.");
    return nullptr;
  }

  // Dependencies are not read: with one tenant there is no other mod for one
  // to name. A tenant that declares some anyway loads exactly the same way.
  if (!inspected.identity.dependencies.empty()) {
    LOGW("Mod folder %ls declares dependencies, which are ignored.\n",
      kTenantFolder);
  }

  ModManifest manifest;
  manifest.paths.folder =
    HTiPathJoin({std::wstring(gPathModsWide), kTenantFolder});
  manifest.paths.dll = inspected.dllPath;
  manifest.meta.packageName = inspected.identity.packageName;
  // Already accepted by ModInspect::ParsesAsVersion, which is this same
  // parser - so this cannot fail without the two disagreeing.
  manifest.meta.version.read(inspected.identity.version);
  manifest.modName = inspected.identity.modName;
  manifest.description = inspected.identity.description;
  manifest.author = inspected.identity.author;
  manifest.gameEditionFlags = inspected.identity.gameEdition;

  // A tenant built for another edition of the game is not loaded. The
  // running edition is always known by now: the Sky backend reads it off the
  // game's window before it starts setup at all.
  if (!HTiBackendCheckEdition(manifest.gameEditionFlags)) {
    skipTenant("Not loaded: it is for a different version of Sky.");
    return nullptr;
  }

  manifest.status = ModStatus_Ok;
  manifest.runtime = nullptr;
  // Nothing else touches the mod data while the tenant is being found, so
  // this needs no lock.
  ModManifest &stored = gModDataLoader[HTTexts_TenantPackageName];
  stored = manifest;

  LOGI("Found mod %s.\n", stored.modName.data());
  return &stored;
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

// What the Mods tab says when LoadLibraryW fails with `err`.
static std::string loadProblem(
  DWORD err
) {
  switch (err) {
  case ERROR_PROC_NOT_FOUND:
    // Deliberately does NOT promise that a newer loader fixes it. 127 means
    // some procedure was missing - it may be one of OUR exports, or one in
    // any dependency the mod pulls in, and nothing here can tell those apart.
    // Keep the number so it can be looked up.
    return "Failed to load (127): incompatible loader or a missing"
           " dependency - a required function was not found.";
  case ERROR_MOD_NOT_FOUND:
    return "Failed to load: the DLL, or something it depends on, is missing.";
  case ERROR_BAD_EXE_FORMAT:
    return "Failed to load: wrong architecture.";
  default: {
    char buf[64];
    snprintf(buf, sizeof(buf), "Failed to load (Windows error %lu).",
             (unsigned long)err);
    return buf;
  }
  }
}

// Load the tenant into the game process and register its runtime data.
static void loadTenant(
  ModManifest *mod
) {
  const char *modName = mod->modName.c_str();
  std::wstring oldPath;

  (void)modName;

  // Save previous dll directory.
  u32 needed = GetDllDirectoryW(0, nullptr);
  if (needed) {
    oldPath.resize(needed + 1);
    GetDllDirectoryW(needed + 1, oldPath.data());
  }

  // Set new dll searching directory.
  SetDllDirectoryW(mod->paths.folder.c_str());

  // Load library.
  HMODULE hMod = LoadLibraryW(mod->paths.dll.c_str());
  if (hMod) {
    LOGI("Loaded mod %s.\n", modName);
  } else {
    // Report the real reason. "No such file" was printed for EVERY failure,
    // which is actively misleading for the most likely one: a mod built
    // against a newer loader fails with ERROR_PROC_NOT_FOUND because an
    // import cannot bind, and the file is sitting right there.
    DWORD err = GetLastError();
    LOGW("Load mod %s failed: error %lu\n", modName, (unsigned long)err);

    // A mod that cannot load stays in the list, named, with a reason - which
    // is the whole point of reading its metadata without loading it.
    mod->setStatus(ModStatus_DllErr);
    mod->problem = loadProblem(err);
  }

  // Restore saved dll searching directory.
  if (needed)
    SetDllDirectoryW(oldPath.c_str());
  else
    SetDllDirectoryW(nullptr);

  if (!hMod)
    return;

  // Save runtime data. Threads the tenant starts while it loads may reach the
  // mod data structs at the same time, so this takes the global lock.
  std::lock_guard<std::mutex> lock(gModDataLock);
  ModRuntime *runtimeData = &gModDataRuntime[hMod];
  runtimeData->handle = hMod;
  runtimeData->manifest = mod;
  getModExportedFunctions(runtimeData);
  mod->runtime = runtimeData;

  HTiRegisterHandle(hMod, HTHandleType_Mod);

  HTiOptionsLoadFor(runtimeData);
}

// Call a mod's exported HTModOnInit(). The mod can only use HTAPI within and
// after this call.
static void initMod(
  ModManifest *mod
) {
  const char *modName = mod->modName.c_str();
  (void)modName;

  if (!mod->runtime)
    return;

  PFN_HTModOnInit fn = mod->runtime->loaderFunc.pfn_HTModOnInit;

  // We assume that a mod that does not export HTModOnInit() has an
  // independent initialization (or enabling, see HTiEnableMods() below)
  // process, so we consider that it is initialized successfully.
  if (!fn || fn(nullptr) == HT_SUCCESS)
    LOGI("Initialized mod %s.\n", modName);
  else
    LOGW("Failed to initialize mod %s.\n", modName);
}

HTStatus HTiLoadMods() {
  HTiBootstrap();

  ModManifest *tenant = findTenant();
  if (tenant)
    loadTenant(tenant);

  // The loader is a mod of its own (see HTiBootstrap), and its HTModOnInit()
  // is what registers the menu hotkey. With no list of mods to walk nothing
  // else would call it, so it is called here, and before the tenant's so the
  // menu hotkey is live by the time the tenant initializes.
  initMod(&gModDataLoader[HTTexts_ModLoaderPackageName]);
  if (tenant)
    initMod(tenant);

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
