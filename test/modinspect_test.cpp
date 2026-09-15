// ----------------------------------------------------------------------------
// Conformance tests for ModInspect.
//
// These run against REAL PE files, not fixtures of the parser's own making,
// because the bug that prompted them lived entirely in what Win32 reports about
// a real file. The shapes that matter are:
//
//   noresource.dll     no .rsrc section at all  -> EnumResourceLanguagesExW
//                      sets ERROR_RESOURCE_DATA_NOT_FOUND (1812)
//   otherresource.dll  has resources, no HTMODMANIFEST -> 1813
//   withmanifest.dll   carries the manifest as RT_RCDATA
//
// The Makefile builds all three with the same toolchain that builds the loader,
// so the test cannot pass against a PE shape the compiler never emits.
// ----------------------------------------------------------------------------
#include "check.h"

#include "../src/modinspect.hpp"

#include <windows.h>

#include <fstream>
#include <string>

namespace {

std::wstring g_root;
std::wstring g_fixtures;

std::wstring widen(const std::string &s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      &out[0], n);
  return out;
}

std::string narrow(const std::wstring &s) {
  if (s.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      &out[0], n, nullptr, nullptr);
  return out;
}

// Every case gets its own folder under the mods root, so one case cannot see
// another's leftovers - the whole point of the ambiguity rules is that what
// else is in the folder changes the answer.
std::wstring makeFolder(const wchar_t *name) {
  const std::wstring path = g_root + L"\\" + name;
  RemoveDirectoryW(path.c_str());
  CreateDirectoryW(path.c_str(), nullptr);
  return path;
}

void copyFixture(const std::wstring &folder, const wchar_t *fixture,
                 const wchar_t *as) {
  const std::wstring from = g_fixtures + L"\\" + fixture;
  const std::wstring to = folder + L"\\" + as;
  CopyFileW(from.c_str(), to.c_str(), FALSE);
}

void writeFile(const std::wstring &folder, const wchar_t *name,
               const std::string &body) {
  std::ofstream out(narrow(folder + L"\\" + name), std::ios::binary);
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

// `main` has to name a DLL that is actually in the folder: the loader refuses a
// manifest pointing at a file that is not there, and the first draft of this
// helper hard-coded "x.dll", which made three cases fail against correct code.
std::string sidecar(const std::string &packageName, const std::string &version,
                    const std::string &main) {
  std::string escaped;
  for (char c : main) {
    if (c == '\\')
      escaped += "\\\\";
    else
      escaped += c;
  }
  return "{\"package_name\":\"" + packageName + "\",\"version\":\"" + version +
         "\",\"game_edition\":-1,\"mod_name\":\"Sidecar\",\"main\":\"" + escaped +
         "\"}";
}

// std::string, not const char*: CHECK_EQ compares with ==, and comparing two
// const char* compares addresses. The first run of this suite reported
// `got: "ok" want: "ok"` as a failure, which is the harness lying, not the code.
std::string name(ModInspect::Outcome o) { return ModInspect::ToString(o); }
std::string name(ModInspect::Source s) { return ModInspect::ToString(s); }

// Printed when a case fails: the code already wrote one sentence saying why,
// and making the harness swallow it sends you back to a debugger for something
// the result carried all along.
void explain(const ModInspect::Result &r) {
  std::printf("    outcome=%s source=%s detail=%s\n",
              ModInspect::ToString(r.outcome), ModInspect::ToString(r.source),
              r.detail.empty() ? "<none>" : r.detail.c_str());
}

bool hasAnomaly(const ModInspect::Result &r, ModInspect::Anomaly want) {
  for (ModInspect::Anomaly a : r.anomalies)
    if (a == want) return true;
  return false;
}

}

// ---------------------------------------------------------------------------
// The regression. A DLL with no resource section at all reports 1812, which is
// "there is no such resource", not "this file could not be read". Treating it
// as unreadable turned every ordinary third-party DLL into a folder-level
// io-error.
// ---------------------------------------------------------------------------
TEST_CASE(a_dll_with_no_resources_at_all_is_absent_not_unreadable) {
  const std::wstring folder = makeFolder(L"no_resources");
  copyFixture(folder, L"noresource.dll", L"noresource.dll");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"no_resources");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::NoManifest));
  CHECK_EQ(name(r.source), name(ModInspect::Source::None));
}

// The consequence, and the reason the bug was expensive: reporting io-error
// stopped the sidecar fallback ever running, so a mod whose manifest.json was
// perfectly readable was reported as unreadable.
TEST_CASE(a_resourceless_dll_still_falls_back_to_its_sidecar) {
  const std::wstring folder = makeFolder(L"sidecar_fallback");
  copyFixture(folder, L"noresource.dll", L"noresource.dll");
  writeFile(folder, L"manifest.json", sidecar("fixture.sidecar", "1.0.0", "noresource.dll"));

  const ModInspect::Result r =
    ModInspect::InspectFolder(g_root, L"sidecar_fallback");

  if (r.outcome != ModInspect::Outcome::Ok) explain(r);
  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::Ok));
  CHECK_EQ(name(r.source), name(ModInspect::Source::Sidecar));
  CHECK_EQ(r.identity.packageName, std::string("fixture.sidecar"));
  CHECK_EQ(r.identity.version, std::string("1.0.0"));
}

// The other absent code seen in the wild: a PE that HAS resources but none of
// our type reports 1813 rather than 1812. Both mean absent.
TEST_CASE(a_dll_with_other_resources_but_no_manifest_is_also_absent) {
  const std::wstring folder = makeFolder(L"other_resources");
  copyFixture(folder, L"otherresource.dll", L"otherresource.dll");
  writeFile(folder, L"manifest.json", sidecar("fixture.other", "1.2.3", "otherresource.dll"));

  const ModInspect::Result r =
    ModInspect::InspectFolder(g_root, L"other_resources");

  if (r.outcome != ModInspect::Outcome::Ok) explain(r);
  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::Ok));
  CHECK_EQ(name(r.source), name(ModInspect::Source::Sidecar));
}

TEST_CASE(a_manifest_resource_is_read_and_preferred) {
  const std::wstring folder = makeFolder(L"resource_ok");
  copyFixture(folder, L"withmanifest.dll", L"withmanifest.dll");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"resource_ok");

  if (r.outcome != ModInspect::Outcome::Ok) explain(r);
  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::Ok));
  CHECK_EQ(name(r.source), name(ModInspect::Source::Resource));
  CHECK_EQ(r.identity.packageName, std::string("fixture.resource"));
  CHECK_EQ(r.identity.version, std::string("2.3.4"));
  CHECK_EQ(r.identity.modName, std::string("Resource Fixture"));
  CHECK(!r.dllPath.empty());
}

// A resource beats a sidecar in the same folder: it can be read from a DLL that
// cannot load, so it is the more trustworthy of the two.
TEST_CASE(a_resource_wins_over_a_sidecar_in_the_same_folder) {
  const std::wstring folder = makeFolder(L"both_sources");
  copyFixture(folder, L"withmanifest.dll", L"withmanifest.dll");
  writeFile(folder, L"manifest.json", sidecar("fixture.sidecar", "9.9.9", "withmanifest.dll"));

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"both_sources");

  CHECK_EQ(name(r.source), name(ModInspect::Source::Resource));
  CHECK_EQ(r.identity.packageName, std::string("fixture.resource"));
}

// FindFirstFileW defines no ordering, so resolving this would pick differently
// on different machines. The folder is refused entirely.
TEST_CASE(two_dlls_claiming_an_identity_refuse_the_whole_folder) {
  const std::wstring folder = makeFolder(L"ambiguous");
  copyFixture(folder, L"withmanifest.dll", L"withmanifest.dll");
  copyFixture(folder, L"secondmanifest.dll", L"secondmanifest.dll");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"ambiguous");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::Ambiguous));
  CHECK(r.dllPath.empty());
}

TEST_CASE(an_empty_folder_is_not_a_mod_and_not_an_error) {
  makeFolder(L"empty");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"empty");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::NoManifest));
}

TEST_CASE(a_folder_that_is_not_there_is_an_io_error) {
  const ModInspect::Result r =
    ModInspect::InspectFolder(g_root, L"never_created");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::IoError));
}

TEST_CASE(a_sidecar_that_is_not_json_is_malformed_not_absent) {
  const std::wstring folder = makeFolder(L"bad_json");
  copyFixture(folder, L"noresource.dll", L"noresource.dll");
  writeFile(folder, L"manifest.json", "{ this is not json");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"bad_json");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::Malformed));
  CHECK(!r.detail.empty());
}

// A cap is a refusal, never a truncation: a truncated read that still parses is
// indistinguishable from a small honest file.
TEST_CASE(a_sidecar_over_the_size_cap_is_refused_not_truncated) {
  const std::wstring folder = makeFolder(L"huge_manifest");
  copyFixture(folder, L"noresource.dll", L"noresource.dll");

  std::string padding(ModInspect::kMaxManifestBytes + 1024, 'x');
  writeFile(folder, L"manifest.json",
            "{\"package_name\":\"big\",\"version\":\"1.0.0\",\"description\":\"" +
              padding + "\"}");

  const ModInspect::Result r =
    ModInspect::InspectFolder(g_root, L"huge_manifest");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::LimitExceeded));
}

TEST_CASE(more_dependencies_than_the_cap_are_refused_not_dropped) {
  const std::wstring folder = makeFolder(L"many_deps");
  copyFixture(folder, L"noresource.dll", L"noresource.dll");

  std::string deps;
  for (unsigned i = 0; i <= ModInspect::kMaxDependencies; ++i) {
    if (i) deps += ",";
    deps += "\"dep" + std::to_string(i) + "\":\">=1.0.0\"";
  }
  writeFile(folder, L"manifest.json",
            "{\"package_name\":\"deps\",\"version\":\"1.0.0\","
            "\"game_edition\":-1,\"dependencies\":{" + deps + "}}");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"many_deps");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::LimitExceeded));
  CHECK(r.identity.dependencies.size() <= ModInspect::kMaxDependencies);
}

// The text a mod controls is `main`, and this is the guard that keeps it from
// naming a file outside its own folder. folderName is deliberately NOT tested
// for escapes: the contract says it comes from the filesystem enumeration, and
// an earlier version of this case fed it ".." and passed - not because
// containment held, but because the parent directory happened to hold two
// manifest-bearing fixtures and came back Ambiguous. A test that passes for a
// reason unrelated to its name is worse than no test.
TEST_CASE(a_manifest_naming_a_file_outside_its_folder_is_refused) {
  for (const char *escape : {"..\\noresource.dll",
                             "..\\..\\..\\windows\\system32\\kernel32.dll",
                             "C:\\Windows\\System32\\kernel32.dll"}) {
    const std::wstring folder = makeFolder(L"escaping_main");
    copyFixture(folder, L"noresource.dll", L"noresource.dll");
    writeFile(folder, L"manifest.json",
              sidecar("fixture.escape", "1.0.0", escape));

    const ModInspect::Result r =
      ModInspect::InspectFolder(g_root, L"escaping_main");

    if (r.outcome == ModInspect::Outcome::Ok || !r.dllPath.empty()) {
      explain(r);
      std::printf("    main=%s dllPath=%s\n", escape, narrow(r.dllPath).c_str());
    }
    CHECK(r.outcome != ModInspect::Outcome::Ok);
    CHECK(r.dllPath.empty());
  }
}

// The header's promise, pinned across every outcome this suite can produce.
// The refused-but-still-populated case was found through one manifest; a caller
// that reads dllPath without checking outcome is the bug this prevents, and it
// existed - the launcher hashed and listed the folder of whatever it was given.
TEST_CASE(a_refused_folder_never_hands_back_a_path) {
  struct Shape {
    const wchar_t *folder;
    const wchar_t *dll;
    const char *manifest;
  };

  const Shape shapes[] = {
    {L"inv_escape", L"noresource.dll", "..\\..\\evil.dll"},
    {L"inv_missing_main", L"noresource.dll", "nothere.dll"},
    {L"inv_absolute", L"noresource.dll", "C:\\Windows\\System32\\kernel32.dll"},
  };

  for (const Shape &s : shapes) {
    const std::wstring folder = makeFolder(s.folder);
    copyFixture(folder, L"noresource.dll", s.dll);
    writeFile(folder, L"manifest.json",
              sidecar("fixture.invariant", "1.0.0", s.manifest));

    const ModInspect::Result r = ModInspect::InspectFolder(g_root, s.folder);
    if (r.outcome != ModInspect::Outcome::Ok && !r.dllPath.empty()) explain(r);
    CHECK(r.outcome != ModInspect::Outcome::Ok);
    CHECK(r.dllPath.empty());
  }

  // And the same promise for the outcomes reached without a manifest at all.
  makeFolder(L"inv_empty");
  CHECK(ModInspect::InspectFolder(g_root, L"inv_empty").dllPath.empty());
  CHECK(ModInspect::InspectFolder(g_root, L"inv_gone").dllPath.empty());
}

TEST_CASE(an_unparseable_sibling_is_reported_but_does_not_refuse_the_folder) {
  const std::wstring folder = makeFolder(L"bad_sibling");
  copyFixture(folder, L"withmanifest.dll", L"withmanifest.dll");
  copyFixture(folder, L"broken.dll", L"broken.dll");

  const ModInspect::Result r = ModInspect::InspectFolder(g_root, L"bad_sibling");

  CHECK_EQ(name(r.outcome), name(ModInspect::Outcome::Ok));
  CHECK(hasAnomaly(r, ModInspect::Anomaly::UnparseableSibling));
}

// ---------------------------------------------------------------------------
// The pure helpers the launcher mirrors.
// ---------------------------------------------------------------------------
TEST_CASE(edition_matching_reproduces_the_loaders_own_rule) {
  CHECK(ModInspect::CheckEdition(0xFFFFFFFFu, 1u));
  CHECK(ModInspect::CheckEdition(0xFFFFFFFFu, 2u));

  CHECK(ModInspect::CheckEdition(0x3u, 0x1u));
  CHECK(ModInspect::CheckEdition(0x3u, 0x2u));

  CHECK(ModInspect::CheckEdition(1u, 1u));
  CHECK(!ModInspect::CheckEdition(1u, 2u));
  CHECK(!ModInspect::CheckEdition(0x3u, 0x4u));
}

// These pin what the loader's parser DOES, not what semver says it should. The
// leniency below is load-bearing: every one of these forms is a version some
// installed mod may already declare, and tightening the parser would stop a mod
// loading that loads today. If that tightening is ever wanted, this case is
// where the decision gets made, visibly.
TEST_CASE(version_parsing_reproduces_the_loaders_own_leniency) {
  CHECK(ModInspect::ParsesAsVersion("1.2.3"));
  CHECK(ModInspect::ParsesAsVersion("1.2.3-rc1"));
  CHECK(ModInspect::ParsesAsVersion("1.2.3+build"));

  // A leading v or =, and surrounding space, are stripped before parsing.
  CHECK(ModInspect::ParsesAsVersion("v1.2.3"));
  CHECK(ModInspect::ParsesAsVersion("  1.2.3  "));

  // Trailing text after patch is IGNORED rather than refused, so both of these
  // are accepted as 1.2.3. Surprising, and true.
  CHECK(ModInspect::ParsesAsVersion("1.2.3.4.5.6.7"));
  CHECK(ModInspect::ParsesAsVersion("1.2.3abc"));

  // Leading zeros are accepted, which strict semver forbids.
  CHECK(ModInspect::ParsesAsVersion("01.02.03"));

  // Three parts are required, and the first must be a digit.
  CHECK(!ModInspect::ParsesAsVersion(""));
  CHECK(!ModInspect::ParsesAsVersion("not a version"));
  CHECK(!ModInspect::ParsesAsVersion("1.2"));
  CHECK(!ModInspect::ParsesAsVersion("1"));
  CHECK(!ModInspect::ParsesAsVersion("-1.2.3"));
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 3) {
    std::printf("usage: modinspect_test <fixtures dir> <scratch dir>\n");
    return 2;
  }
  g_fixtures = argv[1];
  g_root = argv[2];
  CreateDirectoryW(g_root.c_str(), nullptr);

  return Check::run("modinspect conformance");
}
