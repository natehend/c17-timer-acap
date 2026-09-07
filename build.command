#!/bin/bash
# C17 Timer - Build Script
#
# Double-click this file in Finder to build the .eap install package. You
# can also run it from Terminal with `./build.command`.
#
# The AXIS C1710 and C1720 (C17 Series Network Display Speaker) run an NXP
# i.MX 8M Mini, so **aarch64** is the only architecture this app is built
# for - the whole C17 series is aarch64, per Axis's own device list at
# https://help.axis.com/en-us/axis-audio-manager-pro (Prepare your devices,
# item 4). An armv7hf build used to be produced as well, purely so the
# button/event/API side could be exercised on an older speaker such as a
# C1210 without a display; that was never signed and never installed on a
# real C17, so it is no longer built.
#
# Requires Docker Desktop, installed and running, with network access
# (it pulls the Axis ACAP Native SDK image the first time).
#
# What this does, in order:
#   1. Reads the current version number from app/manifest.json.
#   2. Moves any .eap files already sitting in this folder from a
#      previous build into older-versions/, so this folder only ever
#      shows the newest build at a glance, but nothing is ever lost -
#      the last known-working build is always still there if a new one
#      turns out to be broken.
#   3. Builds a Docker image for aarch64 and extracts the resulting .eap
#      into this folder.
#   4. Cleans up the temporary Docker container used for extraction.
#   5. Bumps the patch version number (the last digit) in
#      app/manifest.json by one, so the NEXT time this script runs it
#      automatically builds the next version - no need to edit the
#      version by hand before each build.
#
# It also regenerates app/axis_logo.h from axis-logo.png (if that file is
# present here) before building, so replacing the artwork is just a
# matter of dropping in a new PNG.

set -e

cd "$(dirname "$0")"

pause_and_exit() {
  echo
  read -n 1 -s -r -p "Press any key to close this window..."
  echo
  # The keypress above only unblocks the script - it doesn't actually
  # close the Terminal window; whether Terminal does that on its own
  # depends on a Terminal.app preference (Settings > Profiles > Shell >
  # "When the shell exits") that's often left on "Don't close the
  # window". Closing it explicitly here works regardless of that
  # setting. Matched by tty (this window's specific pseudo-terminal
  # device, e.g. /dev/ttys003) rather than by window title, so this can
  # only ever close the one window actually running this script - never
  # some other Terminal window that happens to have a similar title.
  # The close command runs in a detached, briefly-delayed background
  # subshell rather than inline: Terminal won't close a window it still
  # considers "running a process," so this gives the script's own shell
  # a moment to fully exit first.
  local this_tty
  this_tty="$(tty 2>/dev/null || true)"
  if [ -n "$this_tty" ]; then
    ( sleep 0.3
      osascript -e "tell application \"Terminal\" to close (every window whose tty is \"$this_tty\")" >/dev/null 2>&1
    ) &
    disown
  fi
}
trap pause_and_exit EXIT

echo "=========================================="
echo " C17 Timer - Build"
echo "=========================================="
echo

# --- Check Docker is installed and running ---
if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: Docker was not found."
  echo "Install Docker Desktop from https://www.docker.com/products/docker-desktop/ and try again."
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "ERROR: Docker doesn't seem to be running."
  echo "Open Docker Desktop and wait for it to finish starting, then try again."
  exit 1
fi

# --- Read the current version straight out of manifest.json, without ---
# --- reformatting the rest of the file (a full JSON parse+rewrite     ---
# --- would collapse the blank-line groupings in paramConfig below).   ---
MANIFEST="app/manifest.json"
VERSION=$(grep -Eo '"version": *"[0-9]+\.[0-9]+\.[0-9]+"' "$MANIFEST" | head -1 | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+')

if [ -z "$VERSION" ]; then
  echo "ERROR: Couldn't find a version number in $MANIFEST."
  exit 1
fi

echo "Building version $VERSION..."
echo

VERSION_UNDERSCORE=$(echo "$VERSION" | tr '.' '_')

# --- Embed the AXIS logo -------------------------------------------
# Regenerates app/axis_logo.h from axis-logo.png if that file is here, so
# the display preview shows the real artwork. A no-op when it isn't.
./embed-logo.sh

# --- Move any .eap files already in this folder from a previous    ---
# --- build into older-versions/, so this folder only ever shows the ---
# --- newest build at a glance without losing older ones - the last  ---
# --- known-working build is always still there to fall back to.     ---
OLDER_VERSIONS_DIR="older-versions"
shopt -s nullglob
old_eaps=(*.eap)
if [ ${#old_eaps[@]} -gt 0 ]; then
  mkdir -p "$OLDER_VERSIONS_DIR"
  echo "Moving ${#old_eaps[@]} old .eap file(s) into $OLDER_VERSIONS_DIR/:"
  for f in "${old_eaps[@]}"; do
    echo "  $f"
    mv -f "$f" "$OLDER_VERSIONS_DIR/"
  done
  echo
fi
shopt -u nullglob

build_arch() {
  local arch="$1"
  echo "--- Building for $arch ---"
  docker build --platform=linux/amd64 --build-arg ARCH="$arch" --tag c17-timer-acap-build .

  local container_name="c17-extract-$arch-$$"
  docker create --name "$container_name" --platform=linux/amd64 c17-timer-acap-build >/dev/null

  local eap_name="C17_Timer_${VERSION_UNDERSCORE}_${arch}.eap"
  docker cp "$container_name:/opt/app/$eap_name" "./$eap_name"
  docker rm "$container_name" >/dev/null

  echo "Built: $eap_name"
  echo
}

build_arch aarch64

# --- Bump the patch (last) version number for next time, in place, ---
# --- touching only that one line.                                  ---
MAJOR_MINOR="${VERSION%.*}"
PATCH="${VERSION##*.}"
NEXT_PATCH=$((PATCH + 1))
NEXT_VERSION="${MAJOR_MINOR}.${NEXT_PATCH}"
sed -i '' "s/\"version\": *\"${VERSION}\"/\"version\": \"${NEXT_VERSION}\"/" "$MANIFEST"

echo "=========================================="
echo " Build complete!"
echo "=========================================="
echo "File created in this folder:"
echo "  C17_Timer_${VERSION_UNDERSCORE}_aarch64.eap   <- install this on a C17"
echo
echo "Next build will automatically use version ${NEXT_VERSION}."
