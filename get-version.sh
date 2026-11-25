#!/bin/sh
# Writes generated/probe/version.h (run from the build directory).
#   PROBE_VERSION_OVERRIDE=x   use x (reproducibility checks)
#   --describe                 debugprobe-wifi builds: "v0.2.0-3-gabc1234-dirty"
#                              (git describe, marks uncommitted changes)
# Without either, upstream's behaviour: exact tag or short HEAD hash.
if [ -n "$PROBE_VERSION_OVERRIDE" ]
then
    VERSION="$PROBE_VERSION_OVERRIDE"
elif command -v git &>/dev/null
then
    if [ "$1" = "--describe" ]
    then
        VERSION=$(git -C "$(dirname "$0")" describe --tags --match 'v[0-9]*' --always --dirty 2> /dev/null || echo unknown)
    else
        VERSION=$(git describe --exact-match --tags 2> /dev/null || git rev-parse --short HEAD)   
    fi
else
    VERSION="unknown"
fi

if [ ! -e generated/probe ]
then
    mkdir -p generated/probe
fi

cat > generated/probe/version.h << EOF2
#ifndef _PROBE_VERSION_H
#define _PROBE_VERSION_H

#define PROBE_VERSION "${VERSION}"

#endif
EOF2
