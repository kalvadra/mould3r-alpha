# Mould3r
<img width="1263" height="847" alt="FullGrid" src="https://github.com/user-attachments/assets/a91d890b-dddf-451b-bea0-82734ab388cf" />

A desktop application for designing injection mould tooling.

Mould3r lets you load a fixture (a pair of blank mould halves), place the parts you want to produce inside it, lay out the desired mould features (sprue, runners, gates, vents) and generate the finished mould halves as a pair of STEP files ready for CAM or 3d printing.

A C++/OpenGL app built on wxWidgets and [OpenCascade](https://dev.opencascade.org/), with an OpenGL 3.3 viewport rendered through glad.

## Features

**Import**
- STEP (`.step`, `.stp`) — full BREP, no tessellation loss
- STL (binary and ASCII)
- OBJ (vertex positions and triangular/polygonal faces)
- Mesh imports are automatically sewn into a faceted BREP solid so they participate in boolean operations alongside STEP parts
- Adjustable mesh simplification (Off / Draft / Normal / High) with the target triangle count applied at import time. Controllable from the **Import** menu and persisted between sessions

**Design**
- Position, rotate, scale imported parts inside the fixture
- Select mould injection point
- Place and size sprue, runners, gates, and venting
- Configure feature style and specific feature dimensions (diameter, draft angle, cold‑slug depth, runner cold plug distance, gate draft, vent length/width/overrun, etc.) for full control

**Generate**
- Subtracts the placed parts, vents, sprue, and runner network from both fixture halves using OpenCascade boolean operations
- Exports both halves as STEP files ready for downstream CAM or 3d printing

**Project files**
- Save and reload the entire workspace

**Units**
- Metric (mm) or Imperial (in), switchable from the menu bar

## Building

Mould3r is a Visual Studio project that uses [vcpkg](https://vcpkg.io/) in manifest mode to pull its dependencies.

### Prerequisites

- Visual Studio 2022 (or newer) with the **Desktop development with C++** workload
- [vcpkg](https://github.com/microsoft/vcpkg) integrated with MSBuild (`vcpkg integrate install`)
- Windows 10 or newer

### Dependencies (pulled automatically by vcpkg)

- [wxWidgets](https://www.wxwidgets.org/) — windowing, menus, dialogs
- [OpenCascade](https://dev.opencascade.org/) — BREP modelling, STEP I/O, boolean operations
- [glad](https://glad.dav1d.de/) — OpenGL 3.3 core loader
- [GLM](https://github.com/g-truc/glm) — math types

### Build steps

1. Clone the repo.
2. Open `Mould3r.sln` in Visual Studio.
3. Make sure vcpkg manifest mode is enabled for the project (the repo already includes `vcpkg.json` in the project root).
4. Build in **Release | x64**. The first build will take a while — OpenCascade is sizeable.

The executable is produced in the usual MSBuild output folder. On first run Mould3r looks for a `fixtures/` folder next to the executable; drop your `.fixture` files in there and they'll show up in the fixture picker.

## Using Mould3r

On first launch the app opens to an empty workspace and prompts you to pick a fixture. Subsequent launches remember your last fixture and go straight in.

A typical session:

1. **Pick a fixture** (or make a new one from two STEP models that act as the blank mould halves).
2. **Import** your part 
3. **Position** the part in the blank mold
4. **Place mould features** sprue, runners, gates using the side panel cards. Each feature has its own dimension inputs.
5. **Place vents** wherever you need venting paths.
6. **Generate Mould** runs the boolean cuts. Both halves populate on screen.
7. **Export** writes two STEP files (one per half).

Projects are saved/loaded from the File menu and persist every placed feature along with the fixture reference.

## Project structure

```
AppConfig.*              Simple key=value config file (last fixture, mesh quality)
camera.*                 Orbit camera with pan/zoom/rotate
FileImporter.*           STEP / STL / OBJ import → mesh + BREP
FixtureFile.*            .fixture file format (model paths + injection points)
GLCanvas.*               OpenGL viewport, picking, transforms, mould generation
GridRenderer.*           Parting-plane grid
MainFrame.*              Main window, ribbon, menus, side panel, dialogs
MeshImportSettings.*     Keeps mesh resolution presets
MeshOps.* / MeshUtils.*  Normals, crease-angle splitting, geometry helpers
MeshSimplify.*           Tools for mesh simplification (keeps reasonable time frame in mould generation for STL/OBJ imported objects)
Mould3r.*                wxApp entry point
MouldFeature.*           Vent / Sprue / Runner / Gate feature types
ProjectFile.*            .mould3r project file format (full scene snapshot)
RotateDialog.* / ScaleDialog.* / TranslateDialog.*   Transform input dialogs
shaders.*                GLSL source and shader program wrappers
StartupDialog.*          Fixture picker dialog
style.h                  Centralised colour palette
VentFeature.*            Vent geometry construction
```

## Roadmap

- Undo/redo across the scene graph
- Cross‑platform support
- Expanded feature types (gate geometries, runner cross sections, etc)
- Ejector features
- Built out library of default fixtures with specific machine support

## License

GPL-3.0 license (https://www.gnu.org/licenses/gpl-3.0.en.html)
