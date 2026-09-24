// ----------------------------------------------------------------------------
// Hotkeys of HT's Mod Loader.
//
// Internal now: the loader binds its own keys (the menu toggle, and the
// rebinding UI in the menu), and no mod calls this API. The calls only mods
// used - reading a binding, resetting it, polling a key, unlistening - went.
// ----------------------------------------------------------------------------
#include <string>
#include "imgui.h"
#include "utils/texts.h"
#include "includes/htmodloader.h"
#include "htinternal.hpp"

// Cooldown timer in frames.
#define HOTKEY_MODIFY_COOLDOWN 5

static std::map<HTKeyCode, std::set<ModKeyBind *>> gHotkeyCallbacks;
static i32 gKeyModifyCooldown = 0;

/**
 * Dispatch a key event to all related callbacks.
 */
void HTiHotkeyDispatch(
  HTKeyCode key,
  HTKeyEventFlags flags,
  u08 *userSetBlocked
) {
  std::vector<ModKeyBind *> localCallbacks;
  bool blocked = flags & HTKeyEventFlags_Blocked;

  if (gKeyModifyCooldown)
    return;
  if (flags & HTKeyEventFlags_Repeat)
    // We only dispatch the "real" physical key event.
    return;
  flags &= HTKeyEventFlags_Mask;

  {
    std::lock_guard<std::mutex> lock(gModDataLock);
    auto it = gHotkeyCallbacks.find(key);
    if (it == gHotkeyCallbacks.end())
      return;
    localCallbacks.assign(it->second.begin(), it->second.end());
  }

  HTKeyEvent event;
  event.key = key;
  event.flags = flags;
  event.preventFlags = HTKeyEventPreventFlags_None;

  for (auto it = localCallbacks.begin(); it != localCallbacks.end(); it++) {
    if (blocked && !((*it)->flags & HTHotkeyFlags_NoBlock))
      // Skip all key binds which isn't marked as NoBlock when the key event
      // is intented to be blocked.
      continue;

    // Trigger the event callback.
    PFN_HTHotkeyCallback cb = (*it)->listener;
    if (cb) {
      event.hKey = *it;
      cb(&event);
    }

    if (event.preventFlags & HTKeyEventPreventFlags_Next)
      // Prevent the event pass to the next callback.
      break;
  }

  if (event.preventFlags & HTKeyEventPreventFlags_Game)
    *userSetBlocked = 1;
}

/**
 * Call this function to set the cooldown and block key events.
 */
void HTiHotkeySetCooldown() {
  gKeyModifyCooldown = HOTKEY_MODIFY_COOLDOWN;
}

/**
 * After changing the key binding, there is a cooldown to prevent key message
 * transmission. This function is used to update the cooldown.
 * 
 * Call this function every frame.
 */
void HTiHotkeyUpdateCooldown() {
  if (gKeyModifyCooldown > 0)
    gKeyModifyCooldown--;
}

HTHandle HTMLAPI HTHotkeyRegister(
  HMODULE hModule,
  LPCSTR name,
  HTKeyCode defaultCode
) {
  return HTHotkeyRegisterEx(hModule, name, defaultCode, HTHotkeyFlags_None);
}

HTHandle HTMLAPI HTHotkeyRegisterEx(
  HMODULE hModule,
  LPCSTR name,
  HTKeyCode defaultCode,
  HTHotkeyFlags flags
) {
  std::lock_guard<std::mutex> lock(gModDataLock);
  ModRuntime *rt;
  ModKeyBind *result;

  if (!hModule || !name)
    // Invalid param.
    return (HTHandle)HTiErrAndRet(HTError_InvalidParam, HT_INVALID_HANDLE);

  rt = HTiGetModRuntime(hModule);
  if (!rt)
    // Module handle is invalid.
    return (HTHandle)HTiErrAndRet(HTError_InvalidHandle, HT_INVALID_HANDLE);

  if (defaultCode != HTKey_None && !HTiIsNamedKey(defaultCode))
    // Register fails when the defaultCode is invalid.
    return (HTHandle)HTiErrAndRet(HTError_InvalidParam, HT_INVALID_HANDLE);

  auto it = rt->keyBinds.find(name);
  if (it != rt->keyBinds.end())
    // We won't override prewritten key code.
    result = &it->second;
  else {
    result = &rt->keyBinds[name];

    // The first time to register the key, set the key code to default.
    result->key = defaultCode;
  }

  result->isRegistered = 1;
  result->defaultKey = defaultCode;
  result->keyName = name;
  result->displayName = name;
  result->flags = flags;

  gHotkeyCallbacks[result->key].insert(result);
  HTiRegisterHandle(result, HTHandleType_Hotkey);

  return HTiErrAndRet(HTError_Success, result);
}

HTStatus HTMLAPI HTHotkeyBind(
  HTHandle hKey,
  HTKeyCode keyCode
) {
  ModKeyBind *kb;

  if (!hKey)
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);
  if (!HTiCheckHandleType(hKey, HTHandleType_Hotkey))
    return HTiErrAndRet(HTError_InvalidHandle, HT_FAIL);

  kb = (ModKeyBind *)hKey;

  if (kb->key == keyCode)
    // If the key is not changed, we won't actually set the key.
    return HTiErrAndRet(HTError_Success, HT_SUCCESS);

  // A rebind reports ChangeBind. The helper this was shared with the reset
  // path had the two swapped, so every rebind claimed to be a reset - which
  // nothing noticed, because the menu listener only acts on key-down.
  HTKeyEvent event = {};
  event.flags = HTKeyEventFlags_ChangeBind;
  event.hKey = (HTHandle)kb;
  event.key = kb->key;

  {
    std::lock_guard<std::mutex> lock(gModDataLock);
    gHotkeyCallbacks[kb->key].erase(kb);
    kb->key = keyCode;
    if (keyCode)
      // We won't dispatch HTKey_None as an event.
      gHotkeyCallbacks[keyCode].insert(kb);
  }

  // Trigger an event.
  if (kb->listener)
    kb->listener(&event);

  // Mark the options as "dirty", so we can save it after a delay.
  HTiOptionsMarkDirty();

  return HTiErrAndRet(HTError_Success, HT_SUCCESS);
}

HTStatus HTMLAPI HTHotkeyListen(
  HTHandle hKey,
  PFN_HTHotkeyCallback callback
) {
  ModKeyBind *kb;

  if (!hKey || !callback)
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);
  if (!HTiCheckHandleType(hKey, HTHandleType_Hotkey))
    return HTiErrAndRet(HTError_InvalidHandle, HT_FAIL);

  kb = (ModKeyBind *)hKey;
  {
    std::lock_guard<std::mutex> lock(gModDataLock);
    kb->listener = callback;
  }

  return HT_SUCCESS;
}

/**
 * Modified from ImGui. Get the name string of a key.
 */
const char *HTMLAPI HTHotkeyGetName(HTKeyCode key) {
  if (key == HTKey_None)
    return "None";
  if (!HTiIsNamedKey(key))
    return "Unknown";

  return HTKeyNames[key - HTKey_NamedKey_BEGIN];
}
