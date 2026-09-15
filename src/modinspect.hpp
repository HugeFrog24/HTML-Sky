// ----------------------------------------------------------------------------
// The mod inspection contract.
//
// ONE implementation of "what is in this folder, and is it a mod" - compiled
// into winhttp.dll and into the standalone scanner that the launcher runs.
// Anything that reads a mod folder goes through here.
//
// WHY THIS EXISTS
//
// The loader decides what a mod is. A launcher that installs, removes and
// reports on mods has to agree with that decision exactly, or it describes an
// installation the game does not have: a mod it calls broken that loads fine,
// or one it calls healthy that the loader silently skipped. A second
// implementation in another language, kept in step by a fixture corpus, can
// only DETECT that divergence after it happens. Sharing the implementation
// makes the divergence unrepresentable, which is the stronger guarantee and the
// reason this file is not a specification document.
//
// WHAT IS DELIBERATELY NOT HERE
//
// No policy is INVENTED here. Every acceptance decision below reproduces what
// loader.cpp already did, including the parts that are accidents of cJSON or of
// the Win32 API. Where an input is suspicious but was historically accepted, it
// is accepted AND reported through `anomalies` rather than rejected: this code
// cannot be run against the game here, so changing who loads would be a guess
// dressed as a fix. Tightening any of them later is a one-line change in one
// place, which is the point.
//
// THE RESULT IS SPLIT ON PURPOSE
//
// "Is there a resource", "does its identity validate", "which DLL was chosen",
// "does the folder conflict with another" and "will it actually load" are five
// different questions with five different answers. Collapsing them into one
// boolean is what produced a UI that said "we couldn't read the mod's info
// file" for a mod whose info was perfectly readable and merely duplicated.
// ----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace ModInspect {

// Bumped when any acceptance decision below changes. The scanner reports it and
// the launcher refuses a result whose version it does not know, so a launcher
// paired with an older or newer loader fails loudly instead of quietly applying
// the wrong rules.
constexpr int kPolicyVersion = 1;

// Caps. Every one of these is a refusal, never a truncation: a truncated read
// that still parses is indistinguishable from a small honest file.
constexpr unsigned long kMaxManifestBytes = 64u * 1024u;
constexpr unsigned kMaxDependencies = 256u;
constexpr unsigned kMaxStringBytes = 4096u;
constexpr unsigned kMaxDllsPerFolder = 256u;

enum class Outcome {
  Ok,
  // Nothing in the folder claims to be a mod. Not an error.
  NoManifest,
  // A manifest was found and could not be used. `detail` says why.
  Malformed,
  // More than one DLL in the folder carries a usable manifest. The folder is
  // refused entirely rather than resolved, because FindFirstFileW defines no
  // ordering and picking one would resolve differently on different machines.
  Ambiguous,
  // A cap above was exceeded.
  LimitExceeded,
  // The folder or a file in it could not be read at all.
  IoError,
};

enum class Source {
  None,
  // An RT_RCDATA resource named HTMODMANIFEST inside the mod DLL. Preferred,
  // because it can be read from a DLL that cannot load.
  Resource,
  // A manifest.json beside the DLL. Retained for mods built against the older
  // format, whose loader this binary replaces when a user installs it.
  Sidecar,
};

// Something accepted, but worth telling a human about. NEVER a rejection: each
// of these was accepted by the loader before this file existed, and silently.
enum class Anomaly {
  // `schema` absent or not the value this build knows. Accepted: rejecting an
  // unknown schema would make a future manifest unreadable by every loader
  // already installed, which is the upgrade trap this project exists to avoid.
  UnknownSchema,
  // `game_edition` was a JSON number with a fractional part. cJSON hands back a
  // double and the loader casts to int, so 2.7 has always meant 2.
  FractionalEdition,
  // Text after the end of the JSON value. cJSON_Parse does not require the
  // whole buffer to be consumed, so this has always parsed.
  TrailingBytes,
  // The same key appears twice. cJSON returns the FIRST; JSON.parse in the
  // launcher would have returned the last, so this is a real divergence
  // whenever a manifest contains one.
  DuplicateKey,
  // The DLL carries more than one language variant of the manifest resource.
  // Which one Win32 hands back depends on thread locale and MUI state, so two
  // processes on one machine can legitimately disagree. Reported so that
  // "identical bytes in every variant" can be required rather than assumed.
  MultipleResourceLanguages,
  // Another DLL in the folder had a manifest resource that did not parse. The
  // folder is not ambiguous - only one candidate succeeded - but the leftover
  // is usually a copied build directory and is worth surfacing.
  UnparseableSibling,
};

struct Dependency {
  std::string packageName;
  std::string versionRange;
};

struct Identity {
  std::string packageName;
  // Raw, not normalised: the loader rejects a version it cannot parse, and that
  // state has to be representable rather than smoothed away.
  std::string version;
  std::string modName;
  std::string description;
  std::string author;
  std::string website;
  // As the loader sees it after its own conversion. See CheckEdition().
  int gameEdition = 0;
  std::vector<Dependency> dependencies;
};

struct Result {
  Outcome outcome = Outcome::NoManifest;
  Source source = Source::None;
  // Absolute path to the DLL this folder resolves to, empty unless Ok.
  std::wstring dllPath;
  Identity identity;
  std::vector<Anomaly> anomalies;
  // One sentence for a human, in English, never shown to a player directly.
  std::string detail;
};

// Inspect one folder under the mods directory.
//
// `modsRoot` and `folderName` are kept apart because the result must never be
// built by joining attacker-controlled text onto a path: package_name is
// supplied by the mod and the loader's own validator permits '/' and '.' in it.
// Only `folderName`, which came from the filesystem, is ever joined.
//
// Never throws and never longjmps. Every failure is an Outcome.
Result InspectFolder(const std::wstring &modsRoot, const std::wstring &folderName);

// Whether `declared` is accepted against a running game edition.
//
// Reproduces HTiBackendCheckEdition + the Sky backend's editionCheck exactly,
// and it is NOT a bitwise intersection despite the values looking like flags:
//
//   0xFFFFFFFF  accepted against any game at all (the null-backend bypass)
//   0x3         accepted when the game is Chinese (0x1) OR International (0x2),
//               but NOT when the edition is still Unknown (0x0)
//   otherwise   accepted only on an exact equality with the running edition
//
// So a mod declaring 0x3 is skipped on a machine where the edition has not been
// determined yet, and a mod declaring, say, 0x7 is never accepted by anything.
bool CheckEdition(unsigned declared, unsigned running);

// Mirrors HTiSemVer::parse. Exposed so the scanner reports the same verdict the
// loader will reach, rather than the launcher approximating it in another
// language - an approximation that already disagreed on "v1.2.3", "1.2.3-" and
// "1.2.3-01".
bool ParsesAsVersion(const std::string &raw);

// Stable machine-readable names, used by the scanner's output and by tests.
const char *ToString(Outcome outcome);
const char *ToString(Source source);
const char *ToString(Anomaly anomaly);

}  // namespace ModInspect
