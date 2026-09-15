// ----------------------------------------------------------------------------
// The single implementation of the inspection contract. See modinspect.hpp for
// why this is shared code rather than a specification.
// ----------------------------------------------------------------------------
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "htinternal.hpp"
#include "modinspect.hpp"

namespace ModInspect {

namespace {

// RT_RCDATA is MAKEINTRESOURCE(10), which casts to LPSTR or LPWSTR depending on
// whether UNICODE is defined - and this project does not define it - so the
// wide form is spelled out to match FindResourceW's parameter type. An integer
// resource id is not text and is never decoded through a code page; the
// mismatch is purely one of pointer type.
const wchar_t *const kManifestName = L"HTMODMANIFEST";
#define HTI_MANIFEST_TYPE MAKEINTRESOURCEW(10)

// Language variants of the manifest resource, and the first one's id.
struct LanguageScan {
  unsigned seen = 0;
  WORD first = 0;
};

BOOL CALLBACK collectLanguages(
  HMODULE,
  LPCWSTR,
  LPCWSTR,
  WORD language,
  LONG_PTR param
) {
  LanguageScan *scan = reinterpret_cast<LanguageScan *>(param);
  if (scan->seen == 0)
    scan->first = language;
  scan->seen += 1;
  // Stop once ambiguity is established; a file can claim a great many.
  return scan->seen < 8 ? TRUE : FALSE;
}

// Everything after the parsed value must be whitespace. cJSON_Parse passes
// require_null_terminated = 0, so a manifest followed by arbitrary bytes has
// always parsed; this reports it rather than changing who loads.
bool hasTrailingBytes(const std::string &text, const char *parseEnd) {
  if (!parseEnd)
    return false;
  for (const char *p = parseEnd; p < text.c_str() + text.size(); ++p) {
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '\0')
      return true;
  }
  return false;
}

// cJSON keeps every duplicate in its child list and hands back the FIRST on
// lookup. A JSON.parse in the launcher would keep the last, so a manifest with
// a repeated key means the two disagree about the mod's identity.
bool hasDuplicateKey(const cJSON *object) {
  for (const cJSON *a = object ? object->child : nullptr; a; a = a->next) {
    if (!a->string)
      continue;
    for (const cJSON *b = a->next; b; b = b->next) {
      if (b->string && std::strcmp(a->string, b->string) == 0)
        return true;
    }
  }
  return false;
}

void note(Result &out, Anomaly anomaly) {
  if (std::find(out.anomalies.begin(), out.anomalies.end(), anomaly) ==
      out.anomalies.end())
    out.anomalies.push_back(anomaly);
}

// Does `candidate` resolve to something inside `folder`?
//
// Both sides go through GetFullPathNameW first, so "sub\\..\\mod.dll" is judged
// by where it lands rather than by how it is spelled. The comparison is
// case-insensitive because the filesystem is, and it requires a separator after
// the prefix so that "...\\tibik-old" is not accepted as inside "...\\tibik".
//
// This is a containment check, not a security boundary: a junction inside the
// folder can still lead elsewhere, and reparse-point policy is not settled.
bool withinFolder(const std::wstring &folder, const std::wstring &candidate) {
  wchar_t fullFolder[MAX_PATH] = {0};
  wchar_t fullCandidate[MAX_PATH] = {0};
  if (!GetFullPathNameW(folder.c_str(), MAX_PATH, fullFolder, nullptr))
    return false;
  if (!GetFullPathNameW(candidate.c_str(), MAX_PATH, fullCandidate, nullptr))
    return false;

  std::wstring base = fullFolder;
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
    base.pop_back();
  const std::wstring resolved = fullCandidate;
  if (resolved.size() <= base.size() + 1)
    return false;
  if (resolved[base.size()] != L'\\' && resolved[base.size()] != L'/')
    return false;
  return CompareStringOrdinal(
           resolved.c_str(), (int)base.size(), base.c_str(), (int)base.size(),
           TRUE) == CSTR_EQUAL;
}

bool validPackageName(const std::string &name) {
  // Same character set the loader has always allowed - which notably includes
  // '/' and '.'. That is exactly why a package name is never joined onto a path
  // anywhere in this file.
  if (name.empty() || name.size() > kMaxStringBytes)
    return false;
  return std::all_of(name.begin(), name.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' ||
           ch == '@' || ch == '/';
  });
}

std::string stringField(const cJSON *json, const char *key) {
  const char *value =
    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
  if (!value)
    return std::string();
  const size_t len = std::strlen(value);
  return len > kMaxStringBytes ? std::string() : std::string(value, len);
}

// Read the manifest bytes out of one DLL without loading it.
//
// LOAD_LIBRARY_AS_DATAFILE maps the file as a plain data file: no DllMain, no
// imports resolved, no code executed. That is the whole point - a mod built
// against a newer loader cannot bind its imports, and this still answers "what
// are you", which is the difference between a named failure and a mod that
// silently is not there. It is not an image-layout mapping; the resource
// functions understand the data-file representation.
enum class Fetch { Ok, Absent, TooBig, Ambiguous, Unreadable };

Fetch fetchManifest(const std::wstring &dllPath, std::string &bytes) {
  // _EXCLUSIVE, not the plain flag. Microsoft's own security remarks recommend
  // it whenever the file does not need to stay modifiable while mapped, and
  // nothing here needs that: it stops the inspected file being swapped under
  // the mapping between the size check and the copy. The two flags are not
  // combined; this one replaces it.
  HMODULE module =
    LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
  if (!module)
    return Fetch::Unreadable;

  Fetch status = Fetch::Absent;

  // Enumerate FIRST, then fetch by the language that was enumerated.
  //
  // The previous order asked FindResourceW for "whatever suits this thread"
  // and only afterwards counted variants, so the bytes could come from a
  // different variant than the one reported - and two processes on one machine
  // could legitimately disagree. RESOURCE_ENUM_LN restricts this to the
  // language-neutral resources physically in this file; passing 0 would mean
  // LN|MUI, which is not "only this DLL".
  LanguageScan languages;
  const BOOL enumerated = EnumResourceLanguagesExW(
    module,
    HTI_MANIFEST_TYPE,
    kManifestName,
    collectLanguages,
    reinterpret_cast<LONG_PTR>(&languages),
    RESOURCE_ENUM_LN,
    0);

  if (!enumerated && languages.seen == 0) {
    // Absent is the ordinary case and has its own errors; anything else means
    // the enumeration itself failed and the answer is unknown, not "no".
    //
    // ERROR_RESOURCE_DATA_NOT_FOUND (1812) belongs on this list and was missed:
    // it is what a PE with NO resource section at all reports, which is most
    // third-party mod DLLs. Treating it as unreadable made every one of them a
    // folder-level io-error, and - worse - stopped the sidecar fallback ever
    // running, so a mod with a perfectly good manifest.json was reported as
    // unreadable. Found by pointing this at a real install rather than at a
    // fixture; every DLL in the fixtures happened to have resources.
    const DWORD err = GetLastError();
    FreeLibrary(module);
    return err == ERROR_RESOURCE_DATA_NOT_FOUND ||
           err == ERROR_RESOURCE_TYPE_NOT_FOUND ||
           err == ERROR_RESOURCE_NAME_NOT_FOUND ||
           err == ERROR_RESOURCE_LANG_NOT_FOUND
             ? Fetch::Absent
             : Fetch::Unreadable;
  }

  if (languages.seen > 1) {
    // Refused rather than reported. Which variant Win32 hands back depends on
    // thread locale and MUI state, so accepting one would make the mod's
    // identity a property of the machine that read it. Byte-identical variants
    // could be accepted later; that needs fixtures this has none of.
    FreeLibrary(module);
    return Fetch::Ambiguous;
  }

  HRSRC resource = FindResourceExW(
    module, HTI_MANIFEST_TYPE, kManifestName, languages.first);
  if (resource) {
    const DWORD size = SizeofResource(module, resource);
    if (size == 0) {
      status = Fetch::Unreadable;
    } else if (size > kMaxManifestBytes) {
      // Refused before allocating: the size is a claim made by the file.
      status = Fetch::TooBig;
    } else {
      HGLOBAL handle = LoadResource(module, resource);
      const char *data =
        handle ? reinterpret_cast<const char *>(LockResource(handle)) : nullptr;
      if (data) {
        // Copied because the resource is NOT null-terminated and the mapping
        // dies with the FreeLibrary below.
        bytes.assign(data, size);
        status = Fetch::Ok;
      } else {
        status = Fetch::Unreadable;
      }
    }
  }

  FreeLibrary(module);
  return status;
}

// Was a JSON number safe to narrow to int?
//
// Casting a double that cannot be represented is undefined behaviour, and both
// `schema` and `game_edition` arrive as doubles straight from cJSON - so
// "schema": 1e300 reached a cast. Checked here instead.
bool narrowsToInt(double value, int &out) {
  if (!std::isfinite(value))
    return false;
  if (value < (double)INT32_MIN || value > (double)INT32_MAX)
    return false;
  out = (int)value;
  return true;
}

enum class Read { Ok, Malformed, LimitExceeded };

// Turn manifest text into an identity, or say why it is not one.
Read readIdentity(const std::string &text, Result &out) {
  const char *parseEnd = nullptr;
  cJSON *json = cJSON_ParseWithOpts(text.c_str(), &parseEnd, 0);
  if (!json) {
    out.detail = "the manifest is not valid JSON";
    return Read::Malformed;
  }
  // From here on every exit goes through `finish`, so the cJSON tree is freed
  // on each of them. It was freed at each return before, which is the shape
  // that eventually leaks one.
  Read verdict = Read::Ok;
  const auto finish = [&](Read r, const char *why) {
    if (why)
      out.detail = why;
    cJSON_Delete(json);
    return r;
  };

  if (!cJSON_IsObject(json))
    return finish(Read::Malformed, "the manifest is not a JSON object");

  if (hasTrailingBytes(text, parseEnd))
    note(out, Anomaly::TrailingBytes);
  if (hasDuplicateKey(json))
    note(out, Anomaly::DuplicateKey);

  // Checked, not cast: "schema": 1e300 used to reach a double-to-int cast,
  // which is undefined for a value that cannot be represented. A fractional
  // 1.5 also used to truncate to 1 and read as the known schema.
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
  int schemaValue = 0;
  if (!cJSON_IsNumber(schema) ||
      !narrowsToInt(cJSON_GetNumberValue(schema), schemaValue) ||
      (double)schemaValue != cJSON_GetNumberValue(schema) || schemaValue != 1)
    note(out, Anomaly::UnknownSchema);

  out.identity.packageName = stringField(json, "package_name");
  if (!validPackageName(out.identity.packageName))
    return finish(Read::Malformed, "the manifest has no usable package name");

  out.identity.version = stringField(json, "version");
  if (!ParsesAsVersion(out.identity.version))
    return finish(Read::Malformed, "the manifest's version cannot be read");

  const cJSON *edition = cJSON_GetObjectItemCaseSensitive(json, "game_edition");
  const double rawEdition = cJSON_GetNumberValue(edition);
  if (std::isnan(rawEdition))
    return finish(Read::Malformed, "the manifest has no game edition");
  if (rawEdition != std::floor(rawEdition))
    note(out, Anomaly::FractionalEdition);
  // Truncation toward zero is preserved - it is what the loader has always
  // done - but only inside the representable range. Outside it, the old cast
  // was undefined, so there is no historical behaviour to preserve.
  if (!narrowsToInt(std::trunc(rawEdition), out.identity.gameEdition))
    return finish(Read::Malformed, "the manifest's game edition is out of range");

  out.identity.modName = stringField(json, "mod_name");
  out.identity.description = stringField(json, "description");
  out.identity.author = stringField(json, "author");
  out.identity.website = stringField(json, "website");

  const cJSON *deps = cJSON_GetObjectItemCaseSensitive(json, "dependencies");
  if (cJSON_IsObject(deps)) {
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, deps) {
      // REFUSE, never drop. Silently keeping the first 256 would hand the
      // loader a mod whose 257th requirement is unchecked - a dependency that
      // is missing or incompatible, enforced by nobody. The cap is a limit on
      // what can be inspected, not a licence to inspect part of it.
      if (out.identity.dependencies.size() >= kMaxDependencies)
        return finish(Read::LimitExceeded, "the mod lists too many dependencies");
      if (!cJSON_IsString(item) || !item->string)
        continue;
      const char *range = cJSON_GetStringValue(item);
      // Dependency names and ranges are bounded too. Optional display strings
      // were already capped by stringField; these bypassed it entirely.
      if (std::strlen(item->string) > kMaxStringBytes ||
          (range && std::strlen(range) > kMaxStringBytes))
        return finish(Read::LimitExceeded, "a dependency entry is too long");
      Dependency dep;
      dep.packageName = item->string;
      dep.versionRange = range ? range : "";
      out.identity.dependencies.push_back(dep);
    }
  }

  return finish(verdict, nullptr);
}

}  // namespace

bool ParsesAsVersion(const std::string &raw) {
  HTiSemVer version;
  // HTiSemVer::parse reads each component with std::stoi, which THROWS
  // std::out_of_range past INT_MAX rather than returning false - measured:
  // "2147483648.0.0" throws. That exception would otherwise escape
  // InspectFolder, whose contract says it never throws, and escape it inside
  // winhttp.dll as well, where nothing is catching.
  //
  // Caught rather than pre-validated because the throw is a property of the
  // shared parser, and a separate range check here would be a second rule that
  // can drift from it. A version this parser cannot read is one the loader
  // cannot read either, which is exactly what this function reports.
  try {
    return version.read(raw);
  } catch (...) {
    return false;
  }
}

bool CheckEdition(unsigned declared, unsigned running) {
  if (declared == 0xFFFFFFFFu)
    return true;
  if (declared == 0x3u && (running == 0x1u || running == 0x2u))
    return true;
  return declared == running;
}

namespace {

Result inspectFolderImpl(const std::wstring &modsRoot,
                         const std::wstring &folderName) {
  Result out;

  // Only folderName is ever joined, and it came from the filesystem. Nothing a
  // mod supplies reaches a path here.
  const std::wstring folder = HTiPathJoin({modsRoot, folderName});
  const std::wstring glob = HTiPathJoin({folder, L"\\*.dll"});

  WIN32_FIND_DATAW found;
  SetLastError(0);
  HANDLE search = FindFirstFileW(glob.c_str(), &found);
  unsigned examined = 0;
  bool parsedOne = false;
  Result winner;

  // The same absent-vs-unreadable split fetchManifest makes, and it was missing
  // here: the failure was skipped over with no else, so a folder that is GONE
  // and a folder that cannot be OPENED both reported "nothing here claims to be
  // a mod". ERROR_FILE_NOT_FOUND is the honest empty case - the folder is there
  // and holds no DLL. ERROR_PATH_NOT_FOUND means the folder itself is not
  // there, and anything else (ERROR_ACCESS_DENIED above all) means we could not
  // look, which is not the same as having looked and found nothing.
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND) {
      out.outcome = Outcome::IoError;
      out.detail = err == ERROR_PATH_NOT_FOUND
                     ? "this folder is not there any more"
                     : "this folder could not be read";
      return out;
    }
  }

  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        continue;
      if (++examined > kMaxDllsPerFolder) {
        FindClose(search);
        out.outcome = Outcome::LimitExceeded;
        out.detail = "the folder holds more files than this will examine";
        return out;
      }

      const std::wstring dllPath =
        HTiPathJoin({folder, std::wstring(L"\\") + found.cFileName});

      std::string bytes;
      const Fetch fetched = fetchManifest(dllPath, bytes);
      if (fetched == Fetch::Absent)
        continue;

      // A carrier this cannot examine must not be silently dropped from the
      // candidate set. Dropping it changed which executable the folder
      // resolves to: one oversized valid carrier beside one small valid
      // carrier became an ACCEPTED folder instead of an ambiguous one, and a
      // single oversized carrier fell through to the sidecar and elected a
      // different identity entirely. An unexamined candidate is unknown, not
      // absent.
      if (fetched == Fetch::TooBig || fetched == Fetch::Ambiguous ||
          fetched == Fetch::Unreadable) {
        FindClose(search);
        out.source = Source::Resource;
        out.dllPath = dllPath;
        if (fetched == Fetch::TooBig) {
          out.outcome = Outcome::LimitExceeded;
          out.detail = "a file here carries mod details too large to read";
        } else if (fetched == Fetch::Ambiguous) {
          out.outcome = Outcome::Malformed;
          out.detail = "a file here carries its details in more than one language";
          note(out, Anomaly::MultipleResourceLanguages);
        } else {
          out.outcome = Outcome::IoError;
          out.detail = "a file here could not be read";
        }
        return out;
      }

      Result candidate;
      candidate.source = Source::Resource;
      candidate.dllPath = dllPath;

      const Read read = readIdentity(bytes, candidate);
      if (read == Read::LimitExceeded) {
        FindClose(search);
        out = candidate;
        out.outcome = Outcome::LimitExceeded;
        return out;
      }
      if (read != Read::Ok) {
        // A carrier whose JSON does not parse does NOT make the folder
        // ambiguous - only successful candidates compete - but it is reported,
        // because the usual cause is a build directory copied wholesale.
        note(out, Anomaly::UnparseableSibling);
        continue;
      }

      if (parsedOne) {
        FindClose(search);
        out.outcome = Outcome::Ambiguous;
        out.detail = "more than one file here claims to be the mod";
        return out;
      }
      winner = candidate;
      parsedOne = true;
    } while (FindNextFileW(search, &found));
    // An enumeration that stopped for any reason other than running out of
    // files leaves the candidate set incomplete, and an incomplete set can
    // elect the wrong winner or report no mod at all.
    const DWORD enumError = GetLastError();
    FindClose(search);
    if (enumError != ERROR_NO_MORE_FILES) {
      out.outcome = Outcome::IoError;
      out.detail = "this folder could not be read all the way through";
      return out;
    }
  } else if (GetLastError() != ERROR_FILE_NOT_FOUND &&
             GetLastError() != ERROR_PATH_NOT_FOUND) {
    out.outcome = Outcome::IoError;
    out.detail = "this folder could not be opened";
    return out;
  }

  if (parsedOne) {
    for (const Anomaly a : out.anomalies)
      note(winner, a);
    winner.outcome = Outcome::Ok;
    return winner;
  }

  // Sidecar fallback. Retained for mods built against the older format, whose
  // loader this binary replaces when a user installs it; deleting it would
  // silently break working third-party installs.
  const std::wstring sidecar = HTiPathJoin({folder, L"\\manifest.json"});
  if (!HTiFileExists(sidecar.c_str())) {
    out.outcome = Outcome::NoManifest;
    return out;
  }

  std::string text;
  {
    HANDLE file = CreateFileW(
      sidecar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      out.outcome = Outcome::IoError;
      out.detail = "the mod's details could not be read";
      return out;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (LONGLONG)kMaxManifestBytes) {
      CloseHandle(file);
      out.outcome = size.QuadPart > (LONGLONG)kMaxManifestBytes
                      ? Outcome::LimitExceeded
                      : Outcome::IoError;
      out.detail = "the mod's details are too large to read";
      return out;
    }
    text.resize((size_t)size.QuadPart);
    DWORD read = 0;
    const BOOL ok =
      text.empty() || ReadFile(file, &text[0], (DWORD)text.size(), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != text.size()) {
      out.outcome = Outcome::IoError;
      out.detail = "the mod's details could not be read";
      return out;
    }
  }

  out.source = Source::Sidecar;
  const Read sidecarRead = readIdentity(text, out);
  if (sidecarRead == Read::LimitExceeded) {
    out.outcome = Outcome::LimitExceeded;
    return out;
  }
  if (sidecarRead != Read::Ok) {
    out.outcome = Outcome::Malformed;
    return out;
  }

  // Only the sidecar path needs `main`: a manifest read out of a module names
  // the file it was read from, so a filename stored inside it could only ever
  // disagree with reality.
  cJSON *json = cJSON_Parse(text.c_str());
  const char *main = json
    ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "main"))
    : nullptr;
  if (!main) {
    if (json)
      cJSON_Delete(json);
    out.outcome = Outcome::Malformed;
    out.detail = "the mod's details do not say which file to load";
    return out;
  }
  const std::wstring relative = HTiUtf8ToWstring(main);
  cJSON_Delete(json);

  // Containment is checked on the RESOLVED path, not by banning two dots.
  //
  // A `relative.find(L"..")` test rejected names the loader has always
  // accepted - "foo..dll" contains the substring and escapes nothing - and it
  // is not a containment check either, since it says nothing about where the
  // path actually lands.
  //
  // The boundary is deliberately the MOD'S OWN FOLDER, which is narrower than
  // the old rule: the loader used HTiPathRelative against the whole mods root,
  // so a manifest could name a DLL inside a sibling mod's folder. Nothing
  // legitimate does that, and a mod reaching into another mod's folder is a
  // mod that cannot be uninstalled coherently. Recorded here because it is a
  // deliberate narrowing, not an accident, and kPolicyVersion covers it.
  if (relative.empty() || HTiPathIsAbsolute(relative)) {
    out.outcome = Outcome::Malformed;
    out.detail = "the mod's details point outside its own folder";
    return out;
  }

  out.dllPath = HTiPathJoin({folder, std::wstring(L"\\") + relative});
  if (!withinFolder(folder, out.dllPath)) {
    out.outcome = Outcome::Malformed;
    out.detail = "the mod's details point outside its own folder";
    return out;
  }
  if (!HTiFileExists(out.dllPath.c_str())) {
    out.outcome = Outcome::Malformed;
    out.detail = "the file the mod names is not there";
    return out;
  }

  out.outcome = Outcome::Ok;
  return out;
}

}

// The header promises dllPath is empty unless Ok, and one path broke that
// promise: a manifest naming "..\\..\\..\\windows\\system32\\kernel32.dll" was
// correctly REFUSED, and the refused result still carried the escaping path.
// The scanner serialises dllPath for every outcome, and the launcher read,
// hashed and listed the folder of whatever it was handed - so mod-controlled
// text decided which file got read, on a result that said "no".
//
// Enforced here rather than at each of the fifteen returns above, because a
// rule applied at one exit cannot be forgotten by a branch added later.
Result InspectFolder(const std::wstring &modsRoot, const std::wstring &folderName) {
  Result out = inspectFolderImpl(modsRoot, folderName);
  if (out.outcome != Outcome::Ok)
    out.dllPath.clear();
  return out;
}

const char *ToString(Outcome outcome) {
  switch (outcome) {
  case Outcome::Ok: return "ok";
  case Outcome::NoManifest: return "no-manifest";
  case Outcome::Malformed: return "malformed";
  case Outcome::Ambiguous: return "ambiguous";
  case Outcome::LimitExceeded: return "limit-exceeded";
  case Outcome::IoError: return "io-error";
  }
  return "unknown";
}

const char *ToString(Source source) {
  switch (source) {
  case Source::None: return "none";
  case Source::Resource: return "resource";
  case Source::Sidecar: return "sidecar";
  }
  return "unknown";
}

const char *ToString(Anomaly anomaly) {
  switch (anomaly) {
  case Anomaly::UnknownSchema: return "unknown-schema";
  case Anomaly::FractionalEdition: return "fractional-edition";
  case Anomaly::TrailingBytes: return "trailing-bytes";
  case Anomaly::DuplicateKey: return "duplicate-key";
  case Anomaly::MultipleResourceLanguages: return "multiple-resource-languages";
  case Anomaly::UnparseableSibling: return "unparseable-sibling";
  }
  return "unknown";
}

}  // namespace ModInspect
