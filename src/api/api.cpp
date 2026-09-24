// ----------------------------------------------------------------------------
// Basic APIs of HT's Mod Loader.
//
// Only what Tibik, the one mod this loader serves, imports - plus
// HTGetTenantPackageName, which nothing imports: it is there to be FOUND in
// the export table by a launcher. The version and game-status queries,
// cross-mod handles and manifests, custom options, the path helpers, the ImGui
// context hand-off and the last-error slot served mods written by other
// people, and went with them. The texture functions live with the renderer,
// in backends.cpp.
// ----------------------------------------------------------------------------
#include "includes/htmodloader.h"
#include "utils/texts.h"
#include "htinternal.hpp"

HTMLAPIATTR VOID HTMLAPI HTGetModFolder(
  LPSTR result,
  UINT64 maxLen
) {
  if (!result)
    return;
  strcpy_s(result, maxLen, gPathMods);
}

HTMLAPIATTR LPCSTR HTMLAPI HTGetTenantPackageName(
  VOID
) {
  return HTTexts_TenantPackageName;
}
