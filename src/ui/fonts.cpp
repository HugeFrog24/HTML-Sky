// ----------------------------------------------------------------------------
// UI fonts.
// ----------------------------------------------------------------------------
#include <windows.h>
#include "imgui.h"

#include "htinternal.hpp"

// RT_RCDATA spelled out wide for the same reason as in modinspect.cpp: this
// project does not define UNICODE, so RT_RCDATA would be the narrow form.
#define HTI_FONT_TYPE MAKEINTRESOURCEW(10)

// One of the fonts compiled into this DLL by src/ui/fonts.rc. The bytes live
// in the mapped image for as long as the loader is loaded.
static bool HTiFontResource(const wchar_t *name, void **data, int *size) {
  HRSRC resource = FindResourceW(gModLoaderHandle, name, HTI_FONT_TYPE);
  if (!resource)
    return false;
  const DWORD bytes = SizeofResource(gModLoaderHandle, resource);
  HGLOBAL handle = LoadResource(gModLoaderHandle, resource);
  void *mapped = handle ? LockResource(handle) : nullptr;
  if (!mapped || bytes == 0)
    return false;
  *data = mapped;
  *size = static_cast<int>(bytes);
  return true;
}

/**
 * Give ImGui a font that covers the scripts real UI text uses - Latin,
 * Cyrillic, Georgian and Vietnamese from DejaVu Sans, merged with CJK from
 * DroidSansFallback. Without it ImGui bakes ProggyClean, which is ASCII-only,
 * so Chinese/Russian/Georgian text (including arbitrary player nicknames)
 * renders as '?'. ImGui 1.92 loads glyphs on demand - the Vulkan backend sets
 * ImGuiBackendFlags_RendererHasTextures - so no glyph ranges are needed and a
 * nickname rasterises the first time it is shown.
 *
 * The fonts are resources, not files beside the DLL: the launcher installs
 * only the files it names and refuses an archive carrying anything else, and
 * its loader moves and removals know only winhttp.dll and html-config.json.
 */
void HTiLoadFonts(ImGuiIO &io) {
  void *data = nullptr;
  int size = 0;
  if (!HTiFontResource(L"FONT_DEJAVUSANS", &data, &size))
    return; // no resource: ImGui bakes its ASCII default, exactly as before.

  // The bytes belong to this DLL's image, so the atlas must never free them.
  // ImGui 1.92 copies non-owned data into memory it owns (AddFont in
  // imgui_draw.cpp), so the flag costs nothing.
  const float kFontSize = 16.0f;
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  if (!io.Fonts->AddFontFromMemoryTTF(data, size, kFontSize, &cfg))
    return;

  if (HTiFontResource(L"FONT_DROIDSANSFALLBACK", &data, &size)) {
    cfg.MergeMode = true; // fold CJK into the base font, one ImFont
    io.Fonts->AddFontFromMemoryTTF(data, size, kFontSize, &cfg);
  }
}
