// ----------------------------------------------------------------------------
// Mod loader of HT's Mod Loader.
// ----------------------------------------------------------------------------
#include <cmath>
#include <algorithm>
#include "cJSON.h"

#include "includes/htmodloader.h"
#include "utils/texts.h"
#include "htinternal.hpp"

// Package name should only contains `a-z A-Z 0-9 _ . @ / -`.
static inline bool validatePackageName(
  const std::string &packageName
) {
  return std::all_of(
    packageName.begin(),
    packageName.end(),
    [](char ch) -> bool {
      return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '.'
        || ch == '-'
        || ch == '_'
        || ch == '@'
        || ch == '/';
    }
  );
}

// Dll must not be located out of `mods` folder.
static inline bool validateDllPath(
  const std::wstring &path
) {
  std::wstring rel = HTiPathRelative(
    gPathModsWide,
    path);

  if (rel.empty())
    return false;
  if (HTiPathIsAbsolute(rel))
    return false;
  if (rel.size() > 2 && rel[0] == L'.' && rel[1] == L'.')
    return false;

  return true;
}

// Helper function for get string value with cJSON.
static inline std::string getStringValueFrom(
  const cJSON *json,
  const char *key
) {
  char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
  return s ? s : "";
}

bool ModManifest::readFromFile(
  const std::wstring &modFolderName
) {
  std::wstring folder(gPathModsWide);

  // Get the mod folder.
  folder = HTiPathJoin({folder, modFolderName});

  // Check the manifest.json.
  std::wstring jsonPath = HTiPathJoin({folder, L"\\manifest.json"});
  if (!HTiFileExists(jsonPath.data()))
    return false;

  // Save paths.
  paths.folder = folder;
  paths.json = jsonPath;

  // Open manifest.json.
  std::string content = HTiReadFileAsUtf8(jsonPath);

  // Parse and deserialize the file.
  cJSON *json = cJSON_Parse(content.c_str());
  if (!json)
    return false;

  bool ret = read(json);

  cJSON_Delete(json);

  return ret;
}

// Resource type and name the mod metadata is published under. A string name
// rather than an integer id, so no toolchain-assigned ordinal can collide.
//
// RT_RCDATA itself is MAKEINTRESOURCE(10), which resolves to the ANSI form
// unless UNICODE is defined - and this project does not define it - so it
// cannot be handed to FindResourceW. Spell the wide form explicitly.
#define HTML_MANIFEST_RESOURCE L"HTMODMANIFEST"
#define HTML_MANIFEST_RESTYPE  MAKEINTRESOURCEW(10)

bool ModManifest::readFromModule(
  const std::wstring &modFolderName,
  const std::wstring &dllPath
) {
  // LOAD_LIBRARY_AS_DATAFILE maps the image as data: no DllMain, no imports
  // resolved, no code executed. That is the whole point - a mod whose imports
  // cannot bind still answers "what are you" here, which is the difference
  // between a named failure and an invisible one.
  HMODULE hMod = LoadLibraryExW(
    dllPath.c_str(),
    nullptr,
    LOAD_LIBRARY_AS_DATAFILE);
  if (!hMod)
    return false;

  bool ret = false;
  HRSRC res = FindResourceW(hMod, HTML_MANIFEST_RESOURCE, HTML_MANIFEST_RESTYPE);

  if (res) {
    DWORD size = SizeofResource(hMod, res);
    HGLOBAL handle = LoadResource(hMod, res);
    const char *data = handle
      ? reinterpret_cast<const char *>(LockResource(handle))
      : nullptr;

    if (data && size) {
      // The resource is NOT null-terminated, and cJSON_Parse expects a C
      // string. Parsing in place would read past the resource into whatever
      // follows it in .rsrc.
      std::string content(data, size);

      cJSON *json = cJSON_Parse(content.c_str());
      if (json) {
        // The module carrying this manifest IS the mod, so `main` is neither
        // present nor wanted - a filename inside the file it names could only
        // ever disagree with reality. Setting paths.dll here is what tells
        // read() not to look for one.
        paths.folder = HTiPathJoin({std::wstring(gPathModsWide), modFolderName});
        paths.dll = dllPath;

        ret = read(json);
        cJSON_Delete(json);
      } else {
        LOGW("Malformed HTMODMANIFEST resource in: %ls\n", dllPath.c_str());
      }
    }
  }

  // Frees the data-file mapping. LockResource pointers die with it, which is
  // why the bytes were copied above rather than kept.
  FreeLibrary(hMod);

  return ret;
}

bool ModManifest::read(
  const cJSON *json
) {
  const wchar_t *manifestPath = paths.json.c_str();
  (void)manifestPath;

  // Get package name.
  meta.packageName = getStringValueFrom(json, "package_name");
  if (meta.packageName.empty() || !validatePackageName(meta.packageName)) {
    LOGW(
      "Invalid package name \"%s\" in: %ls\n",
      meta.packageName.c_str(),
      manifestPath);
    return false;
  }

  // Get dll path from manifest.
  //
  // Skipped entirely when paths.dll is already set, which is the case for a
  // manifest read out of the module's own resources: the file we opened IS the
  // mod, so there is no pointer to follow and nothing to validate. Only the
  // sidecar path needs `main`, and for that path it stays mandatory.
  if (paths.dll.empty()) {
    const char *parsedStr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "main"));
    if (!parsedStr) {
      LOGW("Missing dll path of: %ls\n", manifestPath);
      return false;
    }

    // Dll path must not be an absolute path.
    std::wstring dllPathOrigin = HTiUtf8ToWstring(parsedStr);
    if (HTiPathIsAbsolute(dllPathOrigin))
      goto InvalidDllPath;

    // Dll must be located in `mods/` folder.
    paths.dll = HTiPathJoin({paths.folder, dllPathOrigin});
    if (!validateDllPath(paths.dll)) {
InvalidDllPath:
      LOGW("Invalid dll path of: %ls\n", manifestPath);
      return false;
    }
  }

  // Get mod version.
  std::string version = getStringValueFrom(json, "version");
  if (!meta.version.read(version)) {
    LOGW("Invalid mod version of: %ls\n", manifestPath);
    return false;
  }

  // Get compatible game edition of the mod.
  f64 editionFlag = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "game_edition"));
  if (std::isnan(editionFlag)) {
    // Invalid game edition.
    LOGW("Invalid game edition of: %ls\n", manifestPath);
    return false;
  }
  gameEditionFlags = (i32)editionFlag;

  // Get display info.
  modName = getStringValueFrom(json, "mod_name");
  description = getStringValueFrom(json, "description");
  author = getStringValueFrom(json, "author");

  const cJSON *deps = cJSON_GetObjectItemCaseSensitive(json, "dependencies");
  if (deps)
    readDependencies(deps);

  return true;
}

bool ModManifest::readDependencies(
  const cJSON *deps
) {
  if (!cJSON_IsObject(deps))
    return false;

  const cJSON *item;
  cJSON_ArrayForEach(item, deps) {
    if (!cJSON_IsString(item))
      continue;
    dependencies.push_back({
      item->string,
      cJSON_GetStringValue(item)
    });
  }

  return true;
}

// Scan all potential mods.
static void scanMods() {
  HANDLE hFindFile;
  WIN32_FIND_DATAW findData;
  std::wstring modsFolderPath(gPathModsWide);

  LOGI("Scanning mods...\n");

  modsFolderPath += L"\\*";
  hFindFile = FindFirstFileW(modsFolderPath.data(), &findData);
  if (!hFindFile)
    return;

  do {
    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      continue;
    if (!wcscmp(findData.cFileName, L".") || !wcscmp(findData.cFileName, L".."))
      continue;

    LOGI("Found potential mod folder: %ls\n", findData.cFileName);

    ModManifest manifest;

    // Resource first, sidecar second.
    //
    // A mod folder may contain several DLLs (a mod plus its own helper
    // libraries), and only the mod itself carries the manifest resource, so
    // every DLL is offered to the reader and the first one that answers wins.
    // Nothing else identifies which is which without a sidecar to say so.
    bool found = false;
    bool ambiguous = false;
    std::wstring dllGlob = HTiPathJoin(
      {std::wstring(gPathModsWide), findData.cFileName, L"\\*.dll"});
    WIN32_FIND_DATAW dllData;
    HANDLE hFindDll = FindFirstFileW(dllGlob.data(), &dllData);
    if (hFindDll != INVALID_HANDLE_VALUE) {
      do {
        if (dllData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
          continue;
        std::wstring dllPath = HTiPathJoin(
          {std::wstring(gPathModsWide), findData.cFileName,
           std::wstring(L"\\") + dllData.cFileName});
        ModManifest candidate;
        if (candidate.readFromModule(findData.cFileName, dllPath)) {
          if (found) {
            // Two DLLs in one folder both claiming to be the mod. Refuse
            // rather than pick: FindFirstFileW/FindNextFileW define no
            // ordering, so "first wins" would resolve differently on different
            // machines or after a defragment - a mod that works here and not
            // there, with nothing to point at.
            //
            // The realistic cause is a build directory copied wholesale: an
            // unstripped or backup copy of the mod carries the same resource
            // as the real one.
            LOGW("Folder %ls has more than one mod DLL with a manifest "
                 "resource - skipping it. Leave exactly one.\n",
                 findData.cFileName);
            ambiguous = true;
            break;
          }
          manifest = candidate;
          found = true;
        }
      } while (FindNextFileW(hFindDll, &dllData));
      FindClose(hFindDll);
    }

    if (ambiguous)
      continue;
    if (!found && !manifest.readFromFile(findData.cFileName))
      continue;
    if (!HTiFileExists(manifest.paths.dll.data()))
      continue;

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
