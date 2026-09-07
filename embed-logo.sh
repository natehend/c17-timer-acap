#!/bin/bash
# Regenerates app/axis_logo.h from axis-logo.png in this folder, so the
# display preview shows the real AXIS Communications artwork instead of
# the SVG reconstruction webui.c falls back to.
#
# Run automatically by build.command; there's no need to call it by hand.
# If axis-logo.png isn't here, this does nothing and leaves whatever
# header is already in place alone.
set -e
cd "$(dirname "$0")"

SRC="axis-logo.png"
OUT="app/axis_logo.h"

if [ ! -f "$SRC" ]; then
  exit 0
fi

B64=$(base64 < "$SRC" | tr -d '\n')

{
  echo "/**"
  echo " * axis_logo - the AXIS Communications lockup shown at the bottom of the"
  echo " * display preview, as a base64 PNG."
  echo " *"
  echo " * GENERATED FILE - do not edit. Produced from axis-logo.png in the"
  echo " * project root by embed-logo.sh, which build.command runs on every"
  echo " * build. To change the artwork, replace that PNG and rebuild."
  echo " *"
  echo " * Embedded rather than linked because a strict Content-Security-Policy"
  echo " * on the proxied path blocks external assets - the same reason the"
  echo " * C8310 photo lives in panel_image.h."
  echo " *"
  echo " * While AXIS_LOGO_B64 is empty, webui.c falls back to a hand-drawn SVG"
  echo " * reconstruction of the lockup."
  echo " */"
  echo "#ifndef AXIS_LOGO_H"
  echo "#define AXIS_LOGO_H"
  echo ""
  echo "#define AXIS_LOGO_B64 \\"
  # Wrapped into adjacent string literals: one 4KB+ line would be past
  # what C guarantees a compiler must accept for a single logical line.
  echo "$B64" | fold -w 100 | sed 's/^/    "/; s/$/" \\/'
  echo "    \"\""
  echo ""
  echo "#endif // AXIS_LOGO_H"
} > "$OUT"

echo "Embedded $SRC into $OUT ($(wc -c < "$SRC" | tr -d ' ') bytes -> ${#B64} base64 chars)"
