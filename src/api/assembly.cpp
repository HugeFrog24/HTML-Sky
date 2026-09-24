// ----------------------------------------------------------------------------
// Hook APIs of HT's Mod Loader.
//
// Creating and enabling a raw hook is what Tibik imports. Hooking by export
// name, hooking from a signature-scan record, disabling, and the patch calls
// (never implemented - they returned HT_FAIL) went with the mods that were
// meant to use them.
// ----------------------------------------------------------------------------
#include <windows.h>
#include <mutex>
#include <shared_mutex>
#include "MinHook.h"

#include "includes/htmodloader.h"
#include "htinternal.hpp"

static HTMutexShared gMutexAsm;
static std::map<void *, ModHook> gHooks;

std::vector<ModHook *> HTiAsmHookFindFor(
  HMODULE owner
) {
  HTLockReadable lock{gMutexAsm};
  std::vector<ModHook *> result;

  for (auto &it: gHooks) {
    if (it.second.owner == owner)
      result.push_back(&it.second);
  }

  return result;
}

static HTStatus createHook(
  HMODULE hModuleOwner,
  const std::string &name,
  void *fn,
  void *detour,
  void **origin
) {
  ModHook hook;
  MH_STATUS s;

  if (!hModuleOwner || !fn || !detour)
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);
  if (!HTiCheckHandleType(hModuleOwner, HTHandleType_Mod))
    return HTiErrAndRet(HTError_InvalidHandle, HT_FAIL);

  if (!HTiIsExecutableAddr(fn) || !HTiIsExecutableAddr(detour))
    // Not executable address.
    return HTiErrAndRet(HTError_AccessDenied, HT_FAIL);

  if (gHooks.find(fn) != gHooks.end())
    // Already hooked.
    return HTiErrAndRet(HTError_AlreadyExists, HT_FAIL);

  hook.intent = hook.actual = (PFN_HTVoidFunction)fn;
  hook.detour = (PFN_HTVoidFunction)detour;
  hook.owner = hModuleOwner;
  hook.name = name;
  hook.isEnabled = false;

  s = MH_CreateHook(
    (void *)hook.actual,
    (void *)hook.detour,
    (void **)&hook.trampoline);

  if (s != MH_OK)
    return HTiErrAndRet(HTError_AccessDenied, HT_FAIL);

  if (origin)
    *origin = (void *)hook.trampoline;

  gHooks[(void *)hook.intent] = hook;

  return HTiErrAndRet(HTError_Success, HT_SUCCESS);
}

HTMLAPIATTR HTStatus HTMLAPI HTAsmHookCreateRaw(
  HMODULE hModuleOwner,
  LPVOID fn,
  LPVOID detour,
  LPVOID *origin
) {
  HTLockShared lock{gMutexAsm};

  return createHook(
    hModuleOwner,
    "<Raw>",
    fn,
    detour,
    origin);
}

static HTStatus enableHook(
  HMODULE hModuleOwner,
  LPVOID fn
) {
  MH_STATUS s;

  if (!hModuleOwner)
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);
  if (!HTiCheckHandleType(hModuleOwner, HTHandleType_Mod))
    return HTiErrAndRet(HTError_InvalidHandle, HT_FAIL);

  auto hook = gHooks.find(fn);
  if (hook == gHooks.end())
    // Not hooked.
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);

  if (fn != HT_ALL_HOOKS) {
    s = MH_EnableHook((LPVOID)fn);

    if (s != MH_OK)
      return HTiErrAndRet(HTError_AccessDenied, HT_FAIL);

    hook->second.isEnabled = true;

    return HTiErrAndRet(HTError_Success, HT_SUCCESS);
  }

  for (auto it = gHooks.begin(); it != gHooks.end(); it++) {
    if (it->second.owner != hModuleOwner)
      continue;

    s = MH_EnableHook((LPVOID)it->second.actual);

    if (s != MH_OK)
      return HTiErrAndRet(HTError_AccessDenied, HT_FAIL);

    it->second.isEnabled = true;
  }

  return HTiErrAndRet(HTError_Success, HT_SUCCESS);
}

HTMLAPIATTR HTStatus HTMLAPI HTAsmHookEnable(
  HMODULE hModuleOwner,
  LPVOID fn
) {
  HTLockShared lock{gMutexAsm};

  return enableHook(hModuleOwner, fn);
}
