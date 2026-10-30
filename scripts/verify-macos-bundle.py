#!/usr/bin/env python3
"""Reject external non-system runtime dependencies in the staged macOS bundle."""
import pathlib
import plistlib
import subprocess
import sys

bundle = pathlib.Path(sys.argv[1]).resolve()
with (bundle / "Contents/Info.plist").open("rb") as f:
    info = plistlib.load(f)
executable = bundle / "Contents/MacOS" / info["CFBundleExecutable"]
assert executable.is_file(), "Missing bundle executable"
assert (bundle / "Contents/Resources/res").is_dir(), "Missing SVG resources"
assert (bundle / "Contents/Resources/fixtures").is_dir(), "Missing example fixtures"
files = [executable] + sorted((bundle / "Contents/Frameworks").glob("*.dylib"))
for binary in files:
    assert "arm64" in subprocess.check_output(["lipo", "-archs", str(binary)], text=True), binary
    lines = subprocess.check_output(["otool", "-L", str(binary)], text=True).splitlines()[1:]
    for line in lines:
        dep = line.strip().split(" (", 1)[0]
        if dep.startswith(("/System/Library/", "/usr/lib/")):
            continue
        if dep.startswith("@executable_path/"):
            resolved = executable.parent / dep.removeprefix("@executable_path/")
        elif dep.startswith("@loader_path/"):
            resolved = binary.parent / dep.removeprefix("@loader_path/")
        else:
            raise RuntimeError(f"External or unresolved dependency in {binary.name}: {dep}")
        resolved = resolved.resolve()
        assert resolved.is_relative_to(bundle) and resolved.is_file(), (binary, dep)
print(f"PASS: ARM64 executable, resources, {len(files)-1} bundled libraries, no external non-system dependencies")
