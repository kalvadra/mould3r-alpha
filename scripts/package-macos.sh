#!/bin/sh
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <cmake-build-directory> <absolute-package-directory>" >&2
    exit 2
fi
case "$2" in /*) ;; *) echo "Package directory must be absolute" >&2; exit 2 ;; esac
cmake --install "$1" --prefix "$2"
python3 "$(dirname "$0")/verify-macos-bundle.py" "$2/Mould3r.app"
# Sign nested Mach-O libraries before the enclosing bundle.
find "$2/Mould3r.app/Contents/Frameworks" -type f -name '*.dylib' -exec codesign --force --sign - {} \;
codesign --force --sign - "$2/Mould3r.app"
codesign --verify --deep --strict --verbose=2 "$2/Mould3r.app"
