#!/bin/sh
# Starts Liferea-Wonder inside the Flatpak. WebKit's accelerated drawing
# shows blank or broken pages with NVIDIA's driver in Flatpaks (the same
# work-around as the Flathub package of regular Liferea).

set -e

if [ -c /dev/nvidia0 ] && [ -z "${WEBKIT_DISABLE_COMPOSITING_MODE}" ]; then
    export WEBKIT_DISABLE_COMPOSITING_MODE=1
fi

exec /app/bin/liferea-wonder "$@"
