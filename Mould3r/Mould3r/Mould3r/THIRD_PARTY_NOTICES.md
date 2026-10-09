# Third-Party Notices

Mould3r is licensed under the GNU General Public License v3.0. It is built with, and its Windows installer redistributes, the third-party components below. Each is used under its own license, and every one of those licenses is compatible with GPL-3.0.

The full license text of each component ships with this installation in the `licenses/` folder. In the source tree, vcpkg places them at `vcpkg_installed/x64-windows/x64-windows/share/<port>/copyright` after a build, and the installer copies them from there.

## Direct dependencies

| Component | Used for | License |
|---|---|---|
| [wxWidgets](https://www.wxwidgets.org/) | UI framework, update-check networking | wxWindows Library Licence 3.1 (LGPL-2.0-or-later with exception) |
| [Open CASCADE Technology](https://dev.opencascade.org/) | BREP modelling, STEP I/O, booleans | LGPL-2.1 (with the Open CASCADE exception) |
| [Manifold](https://github.com/elalish/manifold) | Mesh booleans | Apache-2.0 |
| [Eigen](https://eigen.tuxfamily.org/) | Sparse linear solvers | MPL-2.0 |
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | Polygon clipping (also used by Manifold) | BSL-1.0 |
| [CDT](https://github.com/artem-ogre/CDT) | Constrained Delaunay triangulation | MPL-2.0; bundled predicates BSD-3-Clause / Apache-2.0 WITH LLVM-exception |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing | MIT |
| [glad](https://github.com/Dav1dde/glad) | OpenGL loader | Generator MIT; generated code Public Domain / CC0; Khronos specifications Apache-2.0 |
| [GLM](https://github.com/g-truc/glm) | Math library | MIT |
| [fTetWild](https://github.com/wildmeshing/fTetWild) | 3D tetrahedral meshing (built into `m3tetmesh.dll`, used by the mesh worker) | MPL-2.0 |
| [GMP](https://gmplib.org/) — mini-gmp subset | Exact rational arithmetic for fTetWild (compiled into `m3tetmesh.dll`) | LGPL-3.0-or-later OR GPL-2.0-or-later |
| [AMGCL](https://github.com/ddemidov/amgcl) 1.4.4 | Algebraic multigrid solver for the 3D fill (header-only, vendored unmodified in `third_party/amgcl`) | MIT |

## Pulled in by the dependencies above

| Component | Pulled in by | License |
|---|---|---|
| FreeType | Open CASCADE (default `freetype` feature) | FTL **or** GPL-2.0-or-later; used under GPL-2.0-or-later |
| libpng | wxWidgets, FreeType | libpng License v2 |
| zlib | wxWidgets, libpng, TIFF, FreeType, Geogram | zlib License |
| bzip2 | FreeType | bzip2 License (BSD-style) |
| Brotli | FreeType | MIT |
| libjpeg-turbo | wxWidgets, TIFF | BSD-3-Clause and IJG License |
| LibTIFF | wxWidgets | libtiff License (BSD-style) |
| XZ Utils (liblzma) | LibTIFF | 0BSD |
| libwebp | wxWidgets | BSD-3-Clause (plus patent grant) |
| NanoSVG | wxWidgets (SVG icons) | zlib License |
| PCRE2 | wxWidgets (regular expressions) | BSD-3-Clause (with PCRE2 exception) |
| Expat | wxWidgets (XML) | MIT |
| Khronos EGL / OpenGL registry headers | glad | Apache-2.0 / MIT (Khronos) |
| [libigl](https://libigl.github.io/) (core + predicates modules only) | fTetWild | MPL-2.0 |
| [Geogram](https://github.com/BrunoLevy/geogram) | fTetWild | BSD-3-Clause |
| [oneTBB](https://github.com/uxlfoundation/oneTBB) | fTetWild (multi-threading) | Apache-2.0 |
| [spdlog](https://github.com/gabime/spdlog) | fTetWild (logging) | MIT |
| [{fmt}](https://github.com/fmtlib/fmt) | spdlog | MIT |
| [json](https://github.com/jdumas/json) (fork of nlohmann/json) | fTetWild | MIT |
| AMGCL | Geogram | MIT |
| stb | Geogram | Public Domain / MIT |
| Geometric predicates (J. R. Shewchuk) | fTetWild, libigl | Public Domain |
| MSH mesh I/O (from PyMesh) | fTetWild | MPL-2.0 |

## Required acknowledgements

- This software is based in part on the work of the Independent JPEG Group.
- Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- Memory-usage routines (`getRSS.c`, bundled with fTetWild) by David Robert Nadeau, http://NadeauSoftware.com/, licensed under the Creative Commons Attribution 3.0 Unported License (https://creativecommons.org/licenses/by/3.0/).

## Other bundled material

- **Icons** in `res/icons/`: from [Tabler Icons](https://tabler.io/icons) (MIT).
- **Microsoft Visual C++ Redistributable** (`vc_redist.x64.exe`): redistributed under Microsoft's Visual Studio license terms. It is a compiler runtime, so it falls under GPL-3.0's System Library provision.

The 3D mesher (`m3tetmesh.dll`) is built by `third_party/m3tetmesh/build_windows.bat` from fTetWild at a pinned commit, with GMP's rationals supplied by mini-gmp from the official GMP 6.3.0 release (checksum-verified). fTetWild's other dependencies come in through its own build at the versions it pins, and the script collects all their license files into `third_party/m3tetmesh/out/licenses/` for the installer. TetGen (AGPL) and Triangle are **not** built or linked: the libigl TetGen module and Geogram's Triangle support are switched off.

The dependency list reflects Mould3r's `vcpkg.json` and each port's default features. If you enable extra features (for example OCCT `tbb`, `freeimage` or `vtk`), add their licenses here.
