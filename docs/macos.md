# macOS build (Apple Silicon)

This adds a native CMake build alongside the existing Visual Studio solution.
The macOS executable requires an OpenGL 4.1 core context (the shaders use GLSL 330).
The Windows solution remains the Windows build entry point.

## Dependencies

Install Apple Command Line Tools (`xcode-select --install`) and Homebrew.
The following binary packages were used for the first native build:

```sh
brew install cmake ninja wxwidgets opencascade glm nlohmann-json eigen manifold clipper2
```

Validated versions: AppleClang 21, wxWidgets 3.3.3, OCCT 7.9.3,
GLM 1.0.3, nlohmann-json 3.12.0, Manifold 3.5.4, Clipper2 2.0.1.

CDT and glad are supplied separately; CMake never downloads them implicitly:

```sh
git clone --branch 1.4.1 --depth 1 https://github.com/artem-ogre/CDT.git ../CDT
git clone --branch v0.1.36 --depth 1 https://github.com/Dav1dde/glad.git ../glad
(cd ../glad && python3 -m glad --profile core --api gl=4.1 --generator c \
    --extensions '' --reproducible --out-path ../glad-generated)
```

Pins: CDT `21fae3ba957551b46130349c318b030f85451be4`,
glad `1ecd45775d96f35170458e6b148eb0708967e402`.
The reproducible glad generation uses its packaged Khronos specification offline.
Alternatively, provide a glad 1.x CMake package exporting `glad::glad`.

## Build and test

From the repository root:

```sh
cmake -S . -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCDT_INCLUDE_DIR="$PWD/../CDT/CDT/include" \
  -DMOULD3R_GLAD_SOURCE_DIR="$PWD/../glad-generated" \
  -DMOULD3R_BUNDLE_SEARCH_DIRS="$(brew --prefix)/lib"
cmake --build build/macos --parallel 3
ctest --test-dir build/macos --output-on-failure
```

The integration test parses a fixture, cuts a solid with OCCT, exports and
reimports STEP through the application's importer using a UTF-8 filename,
and checks the resulting volume. It also tests the application's Manifold
boolean wrapper and closed-manifold result.

The build-tree application links to the dependency installation. To produce a
relocatable app, bundle its transitive libraries and apply an ad-hoc signature:

```sh
scripts/package-macos.sh build/macos "$PWD/build/package"
open build/package/Mould3r.app
```

Do not modify the bundle after signing. The package script rejects unresolved
non-system runtime dependencies. Ad-hoc signing supports local use; this is not
Developer ID signing or Apple notarization. Before distributing a binary,
review and include the licenses/notices for the bundled dependency versions.

## Readability

macOS uses 120% of the Windows-authored font and sidebar/control metrics.
The OpenGL drawable uses physical Retina pixels while picking stays in logical
mouse coordinates. Windows metrics remain unchanged.

## Files and resources

SVG resources and example fixtures are included under `Contents/Resources`.
On macOS, editable fixtures, materials and configuration live in
`~/Library/Application Support/Mould3r`. Missing example files are seeded from
the bundle without overwriting existing user files. The Windows portable
layout next to the executable is unchanged.

## Optional tetrahedral mesher

The existing Windows project also builds without this optional backend.
The default macOS build does not include fTetWild; its worker returns a clear
missing-backend result. Normal STEP/mesh import, mould design and booleans do
not require it. 3D tetrahedral meshing/simulation requires a separate backend build.

`-DMOULD3R_BUILD_TETMESH=ON` exposes the existing fTetWild subproject and its
upstream dependency downloads; this optional configuration has not yet been
validated on macOS. It is not part of the tested configuration above.

## Validation scope

Initial target: macOS 26.6.2, ARM64. Native build, geometry smoke test,
first launch, example-fixture rendering and relocatable bundle checks are the
baseline. Intel Macs, older macOS versions, all editing tools, and the optional
3D mesher need separate qualification. Windows regression testing remains
required before merging; the existing solution and vcpkg manifest are unchanged.
