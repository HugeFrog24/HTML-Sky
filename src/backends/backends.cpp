// ----------------------------------------------------------------------------
// Backend dispatcher.
// You must put your backend initialize functions under the function below.
// ----------------------------------------------------------------------------
#include <windows.h>
#include <climits>
#include <map>
#include <mutex>
#include <vector>
#include "imgui.h"

#include "htinternal.hpp"
#include "includes/htconfig.h"

typedef int (HTMLAPI *PFN_HTiGameEditionCheck)(
  HTGameEdition);

static i32 checkEditionDefault(
  HTGameEdition);
static CRITICAL_SECTION gGraphicInitMutex;
static PFN_HTiGameEditionCheck gEditionCheck = checkEditionDefault;

char gActiveGameBackendName[32] = {0};
char gActiveGLBackendName[32] = {0};
std::wstring gGameProcessName;

static i32 checkEditionDefault(
  HTGameEdition edition
) {
  return edition == gGameStatus.edition;
}

int HTiBackendGLEnterCritical() {
  EnterCriticalSection(&gGraphicInitMutex);
  return !ImGui::GetIO().BackendRendererUserData;
}

int HTiBackendGLLeaveCritical() {
  LeaveCriticalSection(&gGraphicInitMutex);
  return 1;
}

int HTiBackendGLInitComplete() {
  SetEvent(gEventGuiInit);
  // Enable all mods.
  HTiEnableMods();
  return 1;
}

int HTiBackendCheckEdition(
  HTGameEdition edition
) {
  if ((u32)edition == (u32)HT_ImplNull_EditionAll)
    return 1;

  return gEditionCheck(edition);
}

int HTiBackendSetEditionCheckFunc(
  PFN_HTVoidFunction func
) {
  gEditionCheck = (PFN_HTiGameEditionCheck)func;
  return 1;
}

// ----------------------------------------------------------------------------
// [SECTION] Mod-facing texture creation.
//
// The active renderer backend publishes its ImTextureData handler here at init.
// Both shipped backends already have one with exactly this signature, so this
// costs a function pointer rather than an abstraction layer.
//
// Textures are tracked by the ImTextureID the backend assigns, because that is
// the only handle a mod ever sees - it passes the same value to ImGui::Image().
// The map is what lets HTImGuiDestroyTexture() find the owning ImTextureData
// again.
//
// WHY NOT ImGui::RegisterUserTexture()
//
// It looks like the right primitive: it pushes into g.UserTextures, which
// UpdateTexturesEndFrame() folds into PlatformIO.Textures, which is the list
// both backends already service every frame. Two reasons it is not used. It
// carries an "EXPERIMENTAL: DO NOT USE YET" marker in imgui.cpp, and it would
// not actually solve the destroy problem below - core only ticks UnusedFrames
// inside ImFontAtlasUpdateNewFrame(), walking atlas->TexList, so a registered
// user texture's counter still never moves.
//
// WHY DESTROY IS DEFERRED
//
// Neither backend honours WantDestroy on the spot. Vulkan waits for
// UnusedFrames >= ImageCount and OpenGL for UnusedFrames > 0, because a texture
// the mod has finished with may still be referenced by draw data already
// recorded, or by a frame still in flight. Since nothing ticks that counter for
// a texture outside a font atlas, a mod texture marked WantDestroy would sit at
// 0 forever: the backend would never free the VkImage / VkDeviceMemory /
// VkDescriptorSet behind it, and HTImGuiDestroyTexture would report success
// having freed nothing. We own these textures, so we tick the counter
// ourselves, one frame at a time, and delete only once the backend reports
// Destroyed.
//
// Ids cannot collide during that window: the backend has not released the
// VkDescriptorSet or GL texture name yet, so it cannot hand the same one out
// again.
//
// THREADING
//
// Everything here is render-thread-only, and that is checked rather than merely
// documented. The loader calls mods from more than one thread -
// HTiHotkeyDispatch() runs mod callbacks on the window message thread - so a
// mod uploading from a hotkey handler would both race this map and issue
// graphics work on a thread holding no GL context and no exclusive claim on the
// Vulkan queue. A lock would fix the first half and not the second; rejecting
// the call fixes both.

static PFN_HTiUpdateTexture gUpdateTexture = nullptr;
static DWORD gRenderThreadId = 0;
static std::map<u64, ImTextureData *> gModTextures;
static std::vector<ImTextureData *> gDyingTextures;

int HTiBackendSetTextureUpdateFunc(
  PFN_HTiUpdateTexture func
) {
  gUpdateTexture = func;
  // Backends publish from inside their one-shot init, which runs on the thread
  // that renders. Recording it here and not only in the per-frame pump matters:
  // HTiBackendGLInitComplete() calls HTiEnableMods() straight after this, so a
  // mod creating a texture from HTModOnEnable() is already legitimately on the
  // render thread one frame before the pump would first run.
  gRenderThreadId = GetCurrentThreadId();
  return 1;
}

/**
 * Hand a texture to HTiBackendUpdateModTextures(), which frees it once the
 * backend has actually let go of it. See WHY DESTROY IS DEFERRED above.
 */
static void retireTexture(
  ImTextureData *tex
) {
  tex->SetStatus(ImTextureStatus_WantDestroy);
  gDyingTextures.push_back(tex);
}

/**
 * Advance every texture waiting to be destroyed by one frame, and re-record the
 * thread the renderer runs on. Called once per frame by HTiUpdateGUI(), before
 * any mod is asked to draw.
 */
void HTiBackendUpdateModTextures() {
  // Re-sampled rather than left as latched at init, so the check stays correct
  // if the game ever starts presenting from a different thread.
  gRenderThreadId = GetCurrentThreadId();

  // gDyingTextures only becomes non-empty after a successful create, which
  // requires gUpdateTexture, and nothing ever clears it - so it is non-null
  // wherever this loop runs.
  for (size_t i = 0; i < gDyingTextures.size();) {
    ImTextureData *tex = gDyingTextures[i];
    tex->UnusedFrames++;
    gUpdateTexture(tex);
    if (tex->Status != ImTextureStatus_Destroyed) {
      i++;
      continue;
    }
    LOG("[Texture][INFO] destroyed after %d frames\n", tex->UnusedFrames);
    IM_DELETE(tex);
    gDyingTextures.erase(gDyingTextures.begin() + i);
  }
}

HTMLAPIATTR HTStatus HTMLAPI HTImGuiCreateTextureRGBA32(
  const void *pixels,
  UINT32 width,
  UINT32 height,
  UINT64 *outTextureId
) {
  if (!pixels || !width || !height || !outTextureId) {
    LOG("[Texture][ERR] bad args: pixels=%p %ux%u out=%p\n", pixels, width,
        height, outTextureId);
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);
  }
  // ImTextureData::Create() sizes its allocation with an int multiply, so a big
  // enough image wraps it while the memcpy below - computed in 64 bits - does
  // not, and the copy overruns a short buffer. Mods are third-party DLLs
  // feeding us dimensions out of image headers, so this is reachable from an
  // ordinary mod bug and not only from malice.
  if ((u64)width * (u64)height * 4 > (u64)INT_MAX) {
    LOG("[Texture][ERR] %ux%u is larger than a texture can be\n", width, height);
    return HTiErrAndRet(HTError_InvalidParam, HT_FAIL);
  }
  // No renderer yet: the first frame has not run, so there is nothing to upload
  // into. HTError_NotReady is what tells a mod to retry rather than give up.
  if (!gUpdateTexture) {
    LOG("[Texture][ERR] no renderer backend has published an update function "
        "yet (%ux%u)\n", width, height);
    return HTiErrAndRet(HTError_NotReady, HT_FAIL);
  }
  if (GetCurrentThreadId() != gRenderThreadId) {
    LOG("[Texture][ERR] create from thread %lu, not the render thread (%lu)\n",
        GetCurrentThreadId(), gRenderThreadId);
    return HTiErrAndRet(HTError_InvalidThread, HT_FAIL);
  }

  ImTextureData *tex = IM_NEW(ImTextureData)();
  tex->Create(ImTextureFormat_RGBA32, (int)width, (int)height);
  memcpy(tex->GetPixels(), pixels, (size_t)width * (size_t)height * 4);
  tex->SetStatus(ImTextureStatus_WantCreate);

  gUpdateTexture(tex);

  // Both backends set Status to _OK unconditionally at the end of their upload
  // block, so the status alone cannot report a failure: Vulkan routes every
  // VkResult through check_vk_result(), which is inert because the layer sets
  // no CheckVkResultFn, and OpenGL never reads glGetError. TexID is the field
  // that does tell us - a failed vkAllocateDescriptorSets leaves it null, and
  // handing that back as a valid id binds a null descriptor set and loses the
  // device.
  if (tex->GetTexID() == ImTextureID_Invalid) {
    LOG("[Texture][ERR] backend produced no texture for %ux%u (status=%d)\n",
        width, height, (int)tex->Status);
    // Retired rather than deleted outright: the Vulkan backend fills in
    // BackendUserData even when it was the descriptor set allocation that
    // failed, so a VkImage and its memory exist and only
    // ImGui_ImplVulkan_DestroyTexture can free them.
    retireTexture(tex);
    return HTiErrAndRet(HTError_NotFound, HT_FAIL);
  }
  // The upload has completed - Vulkan waits on vkQueueWaitIdle before it
  // returns, OpenGL's glTexImage2D has consumed the buffer - and nothing reads
  // Pixels again: only the WantCreate and WantUpdates branches touch it, and
  // this API never issues an update. imgui would otherwise hold the RAM copy
  // for as long as the texture lives, which for a mod uploading a handful of
  // 2048x2048 atlas pages is hundreds of megabytes doing nothing.
  tex->DestroyPixels();

  LOG("[Texture][INFO] created %ux%u -> texid=%llu\n", width, height,
      (unsigned long long)tex->GetTexID());

  const u64 id = (u64)tex->GetTexID();
  gModTextures[id] = tex;
  *outTextureId = id;
  return HT_SUCCESS;
}

HTMLAPIATTR HTStatus HTMLAPI HTImGuiDestroyTexture(
  UINT64 textureId
) {
  if (GetCurrentThreadId() != gRenderThreadId) {
    LOG("[Texture][ERR] destroy from thread %lu, not the render thread (%lu)\n",
        GetCurrentThreadId(), gRenderThreadId);
    return HTiErrAndRet(HTError_InvalidThread, HT_FAIL);
  }

  auto it = gModTextures.find((u64)textureId);
  if (it == gModTextures.end()) {
    LOG("[Texture][ERR] no such texture: %llu\n",
        (unsigned long long)textureId);
    return HTiErrAndRet(HTError_NotFound, HT_FAIL);
  }

  retireTexture(it->second);
  gModTextures.erase(it);
  return HT_SUCCESS;
}

int HTiSetGLBackendName(
  const char *gl
) {
  strncpy(gActiveGLBackendName, gl, 31);
  gActiveGLBackendName[31] = 0;
  return 1;
}

int HTiSetGameBackendName(
  const char *game
) {
  strncpy(gActiveGameBackendName, game, 31);
  gActiveGameBackendName[31] = 0;

  return 1;
}

int HTiSetGameProcessName(
  const char *name
) {
  gGameProcessName = HTiUtf8ToWstring(name);

  return 1;
}

int HTiSetGameProcessName(
  const wchar_t *name
) {
  gGameProcessName = name;

  return 1;
}

int HTiBackendExpectProcess() {
  int success = 0;

  for (const HTiBackendRegister *p = HTiBackendRegister::list(); p; p = p->prev)
    if (p->fnExpectProcess)
      success |= p->fnExpectProcess();

  return success;
}

int HTiBackendSetupAll() {
  int success = 0;

  InitializeCriticalSection(&gGraphicInitMutex);

  for (const HTiBackendRegister *p = HTiBackendRegister::list(); p; p = p->prev)
    if (p->fnInit)
      success |= p->fnInit();

  return success;
}