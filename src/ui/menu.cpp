#include "imgui.h"

#include "utils/texts.h"
#include "htinternal.hpp"

// Widget widths.
#define HOTKEY_DISPLAY_WIDTH 120.0
#define HOTKEY_RESET_WIDTH 65.0
#define HOTKEY_BUTTONS_WIDTH (HOTKEY_DISPLAY_WIDTH + HOTKEY_RESET_WIDTH)

static const ImVec4 modNameColor(1, 1, 1, 1)
  , modFailedColor(1.0f, 0.45f, 0.45f, 1.0f)
  , modDescColor(0.75, 0.75, 0.75, 1);
static char gFakeBuffer[5] = {0};
static float gMenuKeyBindMaxPosX = 0;
static ModKeyBind *gActiveKey = nullptr;

// ----------------------------------------------------------------------------
// [SECTION] Key binding implementations.
// ----------------------------------------------------------------------------

static HTKeyCode waitForKeyPress() {
  ImGuiIO &io = ImGui::GetIO();
  for (HTKeyCode i = HTKey_NamedKey_BEGIN; i < HTKey_NamedKey_END; i = (HTKeyCode)(i + 1)) {
    ImGuiKey keyCode = HTKeyToImGuiKey(i);
    if (!ImGui::IsKeyPressed(keyCode))
      continue;
    if (keyCode == ImGuiKey_MouseWheelY) {
      if (io.MouseWheel > 0)
        return HTKey_MouseWheelUp;
      if (io.MouseWheel < 0)
        return HTKey_MouseWheelDown;
    } else if (keyCode == ImGuiKey_MouseWheelX) {
      if (io.MouseWheelH > 0)
        return HTKey_MouseWheelLeft;
      if (io.MouseWheelH < 0)
        return HTKey_MouseWheelRight;
    }
    return i;
  }
  return HTKey_None;
}

static i32 keyBindWidget(HTKeyCode *key) {
  ImGuiIO &io = ImGui::GetIO();

  gFakeBuffer[0] = 0;

  // Modify a key.
  io.WantCaptureKeyboard = true;
  ImGui::SetNextItemWidth(HOTKEY_DISPLAY_WIDTH);
  ImGui::InputText(
    "##KeyModify",
    gFakeBuffer,
    sizeof(gFakeBuffer));
  bool hovered = ImGui::IsItemHovered();

  if (hovered)
    // Forcely capture the keyboard inputs if the input area is hovered.
    ImGui::SetKeyboardFocusHere(-1);

  HTKeyCode keyPressed = waitForKeyPress();
  if (keyPressed == HTKey_None)
    // There's no key's pressed, do nothing.
    return 0;
  else if ((keyPressed == HTKey_MouseLeft || keyPressed == HTKey_MouseRight) && !hovered)
    // If clicked other region, then cancel current key editing.
    return -1;
  else if (keyPressed == HTKey_Escape)
    // Clear key binding, which means setting it to HTKey_None.
    *key = HTKey_None;
  else
    // Capture any key inputs and write the captured key into the ModKeyBind
    // struct.
    *key = keyPressed;

  return 1;
}

static void showSingleKeyBind(
  ModKeyBind *kb,
  f32 cursor
) {
  f32 x;

  // Show key display name.
  ImGui::AlignTextToFramePadding();
  ImGui::Text(kb->displayName.c_str());
  ImGui::SameLine();

  // Calculate the max pos X of all the texts, for a better align.
  x = ImGui::GetCursorPosX();
  if (gMenuKeyBindMaxPosX < x)
    gMenuKeyBindMaxPosX = x;

  // Right align, show current key.
  ImGui::SetCursorPosX(cursor);
  if (gActiveKey == kb) {
    // If
    HTKeyCode key;
    i32 t = keyBindWidget(&key);
    if (t) {
      gActiveKey = nullptr;
      if (t == 1)
        (void)HTHotkeyBind(kb, key);
    }
  } else {
    if (ImGui::Button(HTHotkeyGetName(kb->key), ImVec2(HOTKEY_DISPLAY_WIDTH, 0)))
      // Trigger key modification.
      gActiveKey = kb;
  }

  ImGui::SameLine();
  // Show reset button.
  ImGui::BeginDisabled(kb->defaultKey == kb->key);
  if (ImGui::Button("Reset", ImVec2(HOTKEY_RESET_WIDTH, 0)))
    // Reset key bind.
    (void)HTHotkeyBind((HTHandle)kb, kb->defaultKey);
  ImGui::EndDisabled();

  if (gActiveKey)
    HTiHotkeySetCooldown();
  HTiHotkeyUpdateCooldown();
}

/**
 * Display key binds menu, and handle key bind modification.
 *
 * Only the loader's own keys are listed, because only the loader has any: the
 * call a mod would register one with is not exported.
 */
static void displayAndUpdateKeys() {
  f32 windowPadding = ImGui::GetStyle().WindowPadding.x
    , cursor;

  ModRuntime *rt = HTiGetModRuntime(gModLoaderHandle);
  if (!rt)
    return;

  // Calculate cursor pos for right alignment.
  cursor = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX();
  cursor -= HOTKEY_BUTTONS_WIDTH + windowPadding;
  if (cursor <= gMenuKeyBindMaxPosX)
    cursor = gMenuKeyBindMaxPosX;

  ImGui::PushTextWrapPos(500.0);
  for (auto keyIt = rt->keyBinds.begin(); keyIt != rt->keyBinds.end(); keyIt++) {
    ModKeyBind *kb = &keyIt->second;
    // Read back from options.json but never registered: a key nothing
    // listens to.
    if (!kb->isRegistered)
      continue;
    ImGui::PushID((void *)kb);
    showSingleKeyBind(kb, cursor);
    ImGui::PopID();
  }
  ImGui::PopTextWrapPos();
}

// ----------------------------------------------------------------------------
// [SECTION] Settings implementations.
// ----------------------------------------------------------------------------

static void displayLoaderSettings() {
#ifndef HTML_ENABLE_DEBUGGER
  ImGui::BeginDisabled();
  ImGui::Checkbox("Show debugger", &gShowDebugger);
  ImGui::EndDisabled();
#else
  ImGui::Checkbox("Show debugger", &gShowDebugger);
#endif
}

/**
 * Render settings menu.
 */
void HTiMenuSettings() {
  if (ImGui::CollapsingHeader("Loader Settings"))
    displayLoaderSettings();
  if (ImGui::CollapsingHeader("Key Bindings"))
    displayAndUpdateKeys();
}

// ----------------------------------------------------------------------------
// [SECTION] Other submenu implementations.
// ----------------------------------------------------------------------------

/**
 * Render the about tab item.
 */
void HTiMenuAbouts() {
  ImGui::Text("HT's Mod Loader v" HTML_VERSION_NAME " by HTMonkeyG");
  ImGui::Text("This build is modified to run one mod, " HTTexts_TenantName ".");
  ImGui::TextLinkOpenURL(
    "<https://www.github.com/HTMonkeyG/HTML-Sky>",
    "https://www.github.com/HTMonkeyG/HTML-Sky");
}

/**
 * Render the mods tab item: the one mod this loader runs, and whether it is
 * running. A mod that is not shows the reason instead of its description,
 * which in the build people actually run is the only place that reason goes.
 */
void HTiMenuModList() {
  // HTiLoadMods() records the tenant whether or not it loads, so the entry is
  // missing only until that has run.
  auto it = gModDataLoader.find(HTTexts_TenantPackageName);
  if (it == gModDataLoader.end())
    return;
  const ModManifest &tenant = it->second;
  const bool running = tenant.runtime != nullptr;

  ImGui::TextColored(
    running ? modNameColor : modFailedColor,
    "%s", tenant.modName.c_str());

  ImGui::PushStyleColor(ImGuiCol_Text, modDescColor);
  ImGui::TextWrapped(
    "%s",
    running ? tenant.description.c_str() : tenant.problem.c_str());
  ImGui::PopStyleColor();
}
