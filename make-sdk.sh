#!/bin/sh
# Assemble dist/sdk/, the directory a mod builds against.
#
# Upstream ships this inside its release zip but has no target that produces it,
# so a fork building from source has the loader and no way to compile anything
# against it. The layout below mirrors the release zip exactly, so a mod's build
# does not care whether it was given an official SDK or one from here.
#
#   ./make-sdk.sh    (after `mingw32-make`)
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK="$ROOT/dist/sdk"
IMGUI="imgui-1.92.2b"

test -f "$ROOT/dist/htmodloader.lib" || {
  echo "dist/htmodloader.lib missing - run mingw32-make first" >&2
  exit 1
}

rm -rf "$SDK"
mkdir -p "$SDK/includes/htmodloader/includes/backends" \
         "$SDK/includes/$IMGUI/backends" \
         "$SDK/includes/cJSON" \
         "$SDK/lib"

cp "$ROOT/src/includes/"*.h            "$SDK/includes/htmodloader/includes/"
cp "$ROOT/src/includes/backends/"*.h   "$SDK/includes/htmodloader/includes/backends/"
cp "$ROOT/libraries/$IMGUI/"*.h        "$SDK/includes/$IMGUI/"
cp "$ROOT/libraries/$IMGUI/backends/"*.h "$SDK/includes/$IMGUI/backends/"
cp "$ROOT/libraries/cJSON/cJSON.h"     "$SDK/includes/cJSON/"
cp "$ROOT/dist/htmodloader.lib"        "$SDK/lib/"

echo "sdk assembled at dist/sdk ($(find "$SDK" -type f | wc -l) files)"
