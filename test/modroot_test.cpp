// ----------------------------------------------------------------------------
// Conformance tests for the mod root decision.
//
// This is not a path-formatting test. The failure it exists to prevent is a
// loader that reads an EMPTY mod root and so presents as a player's account
// having vanished, because identity.json lives under this path. Every case
// below is written against that failure rather than against the string.
//
// The filesystem is injected, so "does this folder exist" is a table rather
// than a disk - which is what lets the installed case (both candidates naming
// the SAME directory) be stated as a test at all.
// ----------------------------------------------------------------------------
#include "check.h"

#include "../src/modroot.hpp"

#include <set>
#include <string>

namespace {

// The set of directories that "exist" for one case. A file-scope pointer
// because ModRoot takes a plain function pointer - deliberately, so the loader
// pays nothing for the seam.
const std::set<std::wstring> *g_present = nullptr;

int fakeFolderExists(const wchar_t *path) {
  return (g_present && g_present->count(std::wstring(path))) ? 1 : 0;
}

ModRoot::Choice choose(const std::wstring &loaderDir,
                       const std::wstring &gameExeDir,
                       const std::set<std::wstring> &present) {
  g_present = &present;
  const ModRoot::Choice c =
      ModRoot::Choose(loaderDir, gameExeDir, fakeFolderExists);
  g_present = nullptr;
  return c;
}

const std::wstring kGame = L"C:\\Games\\Sky";
const std::wstring kStage = L"C:\\Users\\a\\AppData\\Local\\tibik-wrapper\\loader";

}  // namespace

// The case that covers every existing install. Both candidates resolve to the
// same directory, so whichever branch is taken the answer is identical - which
// is what makes this change safe to ship to people who never asked for it.
TEST_CASE(installed_layout_is_unaffected_whether_or_not_the_folder_exists) {
  const std::wstring expected = kGame + L"\\htmodloader";

  const ModRoot::Choice absent = choose(kGame, kGame, {});
  CHECK_EQ(absent.root, expected);

  const ModRoot::Choice present = choose(kGame, kGame, {expected});
  CHECK_EQ(present.root, expected);
}

// A loader staged outside the game folder, with nobody having opted in. It must
// keep reading the game folder: that is where the player's data already is, and
// silently switching to an empty root here is the account-loss failure.
TEST_CASE(staged_loader_without_the_folder_keeps_the_game_root) {
  const ModRoot::Choice c = choose(kStage, kGame, {});
  CHECK_EQ(c.root, kGame + L"\\htmodloader");
  CHECK(c.source == ModRoot::kFromGameExe);
}

// The opt-in: the folder beside the loader exists, so it wins.
TEST_CASE(staged_loader_with_the_folder_uses_it) {
  const std::wstring beside = kStage + L"\\htmodloader";
  const ModRoot::Choice c = choose(kStage, kGame, {beside});
  CHECK_EQ(c.root, beside);
  CHECK(c.source == ModRoot::kFromLoader);
}

// Existence of the GAME folder's root must not make the loader's one win. The
// two are separate questions, and conflating them would opt people in on the
// strength of a folder that has always been there.
TEST_CASE(only_the_loaders_own_folder_opts_in) {
  const ModRoot::Choice c = choose(kStage, kGame, {kGame + L"\\htmodloader"});
  CHECK_EQ(c.root, kGame + L"\\htmodloader");
  CHECK(c.source == ModRoot::kFromGameExe);
}

// The source flag is what the log line reports, and the log is how a support
// conversation distinguishes "your account is gone" from "the loader read the
// other folder". A correct path with a mislabelled source is still a defect.
TEST_CASE(source_names_the_folder_actually_chosen) {
  const std::wstring beside = kStage + L"\\htmodloader";

  const ModRoot::Choice loader = choose(kStage, kGame, {beside});
  CHECK(loader.source == ModRoot::kFromLoader);
  CHECK_EQ(loader.root, beside);

  const ModRoot::Choice game = choose(kStage, kGame, {});
  CHECK(game.source == ModRoot::kFromGameExe);
  CHECK_EQ(game.root, kGame + L"\\htmodloader");
}

// A null probe must not be treated as "it exists". Choose() is reachable before
// the loader has a filesystem helper wired, and defaulting to the opt-in on a
// missing probe would be the empty-root failure again, arrived at sideways.
TEST_CASE(a_missing_filesystem_probe_falls_back_rather_than_opting_in) {
  const ModRoot::Choice c = ModRoot::Choose(kStage, kGame, nullptr);
  CHECK_EQ(c.root, kGame + L"\\htmodloader");
  CHECK(c.source == ModRoot::kFromGameExe);
}
