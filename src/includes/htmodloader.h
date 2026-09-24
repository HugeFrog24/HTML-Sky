// ----------------------------------------------------------------------------
// API exports of HTModLoader.
// <https://www.github.com/HTMonkeyG/HTML-Sky>
//
// MIT License
//
// Copyright (c) 2025 HTMonkeyG
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// ----------------------------------------------------------------------------

// #pragma once
#ifndef __HTMODLOADER_H__
#define __HTMODLOADER_H__

// Throws an error when compiled on other architectures.
#if !(defined(_M_X64) || defined(_WIN64) || defined(__x86_64__) || defined(__amd64__))
#error HT's Mod Loader and it's related mods is only avaliable on x86-64!
#endif

// Mod loader version.
// Version number is used for pre-processing statements handling version
// compatibility.
#define HTML_VERSION 11002
#define HTML_VERSION_NAME "1.10.2"

#define HTMLAPI __stdcall
#ifndef HTMLAPIATTR
#define HTMLAPIATTR
#endif

// Includes.
#include <windows.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// This fork serves one mod, Tibik, and exports only what it imports. The rest
// of upstream's mod-facing API - version and game queries, cross-mod handles
// and manifests, messaging, the datastore, custom options, the path helpers,
// memory, signature scans, console output, asserts and the last-error slot -
// served mods written by other people, and went with them.

// ----------------------------------------------------------------------------
// [SECTION] HTML/basic
// ----------------------------------------------------------------------------

// Whether the execution was successful or not.
typedef enum {
  HT_FAIL = 0,
  HT_SUCCESS = 1
} HTStatus;

// Game editions. Defined by backends.
typedef int HTGameEdition;

// Uninitialized state.
#define HT_ImplNull_EditionUnknown 0
// Only for debug use, don't check edition compatibility.
#define HT_ImplNull_EditionAll 0xFFFFFFFF

// Game status.
typedef struct {
  // Base address of game executable file.
  LPVOID baseAddr;
  // The edition of the game.
  HTGameEdition edition;
  // The window handle of the game.
  HWND window;
  // The process id of the game.
  DWORD pid;
} HTGameStatus;

// Function prototype.
typedef LPVOID (HTMLAPI *PFN_HTVoidFunction)(
  VOID);

// Handle.
typedef LPVOID HTHandle;

#define HT_INVALID_HANDLE NULL

/* Mod exported function prototypes. */

// Gui renderer.
typedef VOID (HTMLAPI *PFN_HTModRenderGui)(
  FLOAT, LPVOID);
// Initialize event
typedef HTStatus (HTMLAPI *PFN_HTModOnInit)(
  LPVOID);
// Mod enable event
typedef HTStatus (HTMLAPI *PFN_HTModOnEnable)(
  LPVOID);

/**
 * Get the folder where the mods are located, in the active code page.
 */
HTMLAPIATTR VOID HTMLAPI HTGetModFolder(
  LPSTR result,
  UINT64 maxLen);

/**
 * Get the package name of the one mod this loader runs.
 *
 * The loader looks only in the folder of that name under the mods folder, and
 * never opens any other. Exported so that can be told from outside: a launcher
 * reading the export table finds this name without loading the DLL, and knows
 * every other mod folder is ignored.
 *
 * Returns a static string, never NULL.
 */
HTMLAPIATTR LPCSTR HTMLAPI HTGetTenantPackageName(
  VOID);

/**
 * Create a texture from 32-bit RGBA pixels using whichever renderer backend is
 * active, and return an ImTextureID usable with ImGui::Image() and
 * ImDrawList::AddImage().
 *
 * WHY THIS EXISTS
 *
 * A mod cannot create textures on its own. The loader owns the renderer, and
 * under the Vulkan layer there is no GL context at all - so a mod that uploads
 * through OpenGL silently gets nothing back and its icons simply never appear,
 * with no error anywhere.
 *
 * Handing out the VkDevice instead would work, but it couples every mod to
 * Vulkan and to the lifetime of objects the loader recreates on swapchain
 * rebuild. Doing it here costs the mod nothing and keeps it renderer-agnostic:
 * both ImGui_ImplVulkan_UpdateTexture() and ImGui_ImplOpenGL3_UpdateTexture()
 * accept an ImTextureData, so this dispatches to whichever backend is live and
 * the mod never names a graphics API.
 *
 * MUST BE CALLED FROM HTModRenderGui(), on the render thread, like every other
 * ImGui call. A call from any other thread is rejected rather than being
 * allowed to race the renderer.
 *
 * `pixels` is copied and need not outlive the call.
 *
 * Returns HT_FAIL when the renderer has not started yet (retry on a later
 * frame), for a null argument, a zero dimension or a size that is too large,
 * off the render thread, or when the renderer refused to allocate the texture.
 * The cases are not told apart: the one mod this loader serves treats them
 * all the same way.
 */
HTMLAPIATTR HTStatus HTMLAPI HTImGuiCreateTextureRGBA32(
  const void *pixels,
  UINT32 width,
  UINT32 height,
  UINT64 *outTextureId);

/**
 * Release a texture created by HTImGuiCreateTextureRGBA32. Same thread rule.
 *
 * The id is invalid the moment this returns, but the GPU resources behind it
 * are freed a few frames later, once no frame still in flight can reference
 * them. Fails for an id that was never created or has already been destroyed.
 */
HTMLAPIATTR HTStatus HTMLAPI HTImGuiDestroyTexture(
  UINT64 textureId);

// ----------------------------------------------------------------------------
// [SECTION] HTML/assembly
// ----------------------------------------------------------------------------

// Enable or disable all hooks or patches created by the specified mod.
#define HT_ALL_HOOKS NULL

/**
 * Create hook with MinHook. This function won't record the function name.
 */
HTMLAPIATTR HTStatus HTMLAPI HTAsmHookCreateRaw(
  HMODULE hModuleOwner,
  LPVOID fn,
  LPVOID detour,
  LPVOID *origin);

/**
 * Enable hook on specified function.
 */
HTMLAPIATTR HTStatus HTMLAPI HTAsmHookEnable(
  HMODULE hModuleOwner,
  LPVOID fn);

#ifdef __cplusplus
}
#endif

#endif
