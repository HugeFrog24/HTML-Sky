// ----------------------------------------------------------------------------
// htmodscan - read-only inspection of a mods folder, for the launcher.
//
// Built from the SAME ModInspect core that winhttp.dll uses, which is the whole
// point: a launcher that installs and removes mods has to agree with the loader
// about what a mod is, and a second implementation in another language can only
// discover a disagreement after shipping one.
//
//   htmodscan.exe "<game>\htmodloader\mods"
//
// Writes one JSON object to stdout and exits 0 when it produced a result, even
// a result full of failures - a mod folder that cannot be read is an answer,
// not a crash. A non-zero exit means the scan itself could not run, and stdout
// is then not JSON.
//
// What it deliberately does NOT do: load any inspected DLL, write anything,
// touch the network, or read outside the folder it is given. It is the
// untrusted-input surface of the launcher, so it is a separate process that can
// die without taking the app with it. Process separation here is containment,
// not a security sandbox.
// ----------------------------------------------------------------------------
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "htinternal.hpp"
#include "modinspect.hpp"

namespace {

// Protocol version of this program's OUTPUT, separate from the policy version
// it enforces. The shape can change while the rules do not, and the launcher
// needs to refuse each independently.
constexpr int kProtocolVersion = 1;

void addIdentity(cJSON *parent, const ModInspect::Identity &identity) {
  cJSON *out = cJSON_AddObjectToObject(parent, "identity");
  if (!out)
    return;
  cJSON_AddStringToObject(out, "packageName", identity.packageName.c_str());
  cJSON_AddStringToObject(out, "version", identity.version.c_str());
  cJSON_AddStringToObject(out, "modName", identity.modName.c_str());
  cJSON_AddStringToObject(out, "description", identity.description.c_str());
  cJSON_AddStringToObject(out, "author", identity.author.c_str());
  cJSON_AddStringToObject(out, "website", identity.website.c_str());
  cJSON_AddNumberToObject(out, "gameEdition", identity.gameEdition);

  // An ARRAY, not an object keyed by package name.
  //
  // cJSON keeps both children for a repeated key and the loader evaluates every
  // one of them, so a manifest may legitimately carry two requirements on one
  // name. Encoding that as object properties emits duplicate JSON keys -
  // measured: {"helper":">=1.0.0","helper":">=2.0.0"} - and JSON.parse in the
  // launcher keeps only the last. Sharing the parser buys nothing if the
  // serialisation throws half the answer away.
  cJSON *deps = cJSON_AddArrayToObject(out, "dependencies");
  if (!deps)
    return;
  for (const ModInspect::Dependency &dep : identity.dependencies) {
    cJSON *entry = cJSON_CreateObject();
    if (!entry)
      return;
    cJSON_AddStringToObject(entry, "packageName", dep.packageName.c_str());
    cJSON_AddStringToObject(entry, "versionRange", dep.versionRange.c_str());
    cJSON_AddItemToArray(deps, entry);
  }
}

void addFolder(
  cJSON *array,
  const std::wstring &folderName,
  const ModInspect::Result &result
) {
  cJSON *out = cJSON_CreateObject();
  if (!out)
    return;

  cJSON_AddStringToObject(out, "folderName", HTiWstringToUtf8(folderName.c_str()).c_str());
  cJSON_AddStringToObject(out, "outcome", ModInspect::ToString(result.outcome));
  cJSON_AddStringToObject(out, "source", ModInspect::ToString(result.source));
  cJSON_AddStringToObject(out, "dllPath", HTiWstringToUtf8(result.dllPath.c_str()).c_str());
  cJSON_AddStringToObject(out, "detail", result.detail.c_str());

  cJSON *anomalies = cJSON_AddArrayToObject(out, "anomalies");
  if (anomalies) {
    for (const ModInspect::Anomaly anomaly : result.anomalies) {
      cJSON *item = cJSON_CreateString(ModInspect::ToString(anomaly));
      if (item)
        cJSON_AddItemToArray(anomalies, item);
    }
  }

  addIdentity(out, result.identity);
  cJSON_AddItemToArray(array, out);
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    std::fwprintf(stderr, L"usage: htmodscan <mods folder>\n");
    return 2;
  }

  const std::wstring modsRoot = argv[1];
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return 3;

  cJSON_AddNumberToObject(root, "protocol", kProtocolVersion);
  cJSON_AddNumberToObject(root, "policyVersion", ModInspect::kPolicyVersion);
  cJSON_AddStringToObject(root, "modsRoot", HTiWstringToUtf8(modsRoot.c_str()).c_str());

  cJSON *folders = cJSON_AddArrayToObject(root, "folders");

  const std::wstring glob = HTiPathJoin({modsRoot, L"\\*"});
  WIN32_FIND_DATAW found;
  DWORD enumError = ERROR_NO_MORE_FILES;
  HANDLE search = FindFirstFileW(glob.c_str(), &found);
  // INVALID_HANDLE_VALUE, not NULL: FindFirstFileW never returns NULL, and a
  // missing mods folder is an ordinary answer - the game simply has none yet.
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        continue;
      if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L".."))
        continue;
      if (folders) {
        addFolder(
          folders, found.cFileName,
          ModInspect::InspectFolder(modsRoot, found.cFileName));
      }
    } while (FindNextFileW(search, &found));
    enumError = GetLastError();
    FindClose(search);
  } else {
    // "Missing" is one of several reasons this can fail, and reporting them all
    // as missing turns an inventory failure into the confident claim that the
    // player has no mods. Measured: a regular file passed as the root reported
    // modsFolderMissing with exit 0.
    const DWORD err = GetLastError();
    const DWORD attrs = GetFileAttributesW(modsRoot.c_str());
    const char *reason;
    if (attrs == INVALID_FILE_ATTRIBUTES &&
        (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND))
      reason = "missing";
    else if (attrs != INVALID_FILE_ATTRIBUTES &&
             !(attrs & FILE_ATTRIBUTE_DIRECTORY))
      reason = "not-a-folder";
    else if (err == ERROR_ACCESS_DENIED)
      reason = "access-denied";
    else
      reason = "unreadable";
    cJSON_AddStringToObject(root, "rootProblem", reason);
    cJSON_AddNumberToObject(root, "rootError", (double)err);
  }

  // An enumeration that ended for any reason other than running out of entries
  // produced an INCOMPLETE inventory, which must not read as a complete one.
  if (search != INVALID_HANDLE_VALUE && enumError != ERROR_NO_MORE_FILES) {
    cJSON_AddStringToObject(root, "rootProblem", "incomplete");
    cJSON_AddNumberToObject(root, "rootError", (double)enumError);
  }

  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text)
    return 3;

  // Bytes, not text: stdout must carry UTF-8 through unchanged, and the default
  // text mode would translate line endings inside strings.
  _setmode(_fileno(stdout), _O_BINARY);
  std::fwrite(text, 1, std::strlen(text), stdout);
  cJSON_free(text);
  return 0;
}
