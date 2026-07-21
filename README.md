# Mesh Compare

Mesh Compare is the standalone, single-workspace mesh comparison application.
It keeps the MeshLab-style viewport engine behind a Renderer Adapter while
owning its window, workflow, diagnostics, application data, build, and macOS
bundle.

## Dependencies

The customized MeshLab/VCGLib viewport engine and its GLEW, EasyExif, and
Eigen sources are vendored in this repository:

```text
meshcompare/
├── src/
└── third_party/meshlab/
```

Configuration and compilation do not download source code or read a sibling
repository. The remaining build prerequisites are CMake 3.18 or newer, a C/C++
toolchain, Qt 5.15, and the platform OpenGL SDK. On macOS, packaging also uses
the `macdeployqt` installed alongside that Qt and the standard Xcode command
line tools.

## Build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/5.15
cmake --build build --target meshcompare --parallel
```

`CMAKE_PREFIX_PATH` can be omitted when Qt 5.15 is already discoverable.

The macOS application is written to:

```text
build/dist/meshcompare.app
```

The build imports only `meshlab-common`, `io_base`, VCGLib, and the viewport
sources it needs. It does not build or package MeshLab's MainWindow, filters,
edit tools, render plugins, or decorators. On macOS, Qt frameworks and the
platform plugin are deployed into the signed bundle.

Tests remain available as an opt-in build:

```bash
cmake -S . -B build-tests -DBUILD_TESTING=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests -R '^meshcompare-' --output-on-failure
```

## Launch

Open the application normally, or pass 2–8 meshes to enter comparison mode
immediately. Ordinary mesh files are displayed in one linked viewport per
mesh:

```bash
open build/dist/meshcompare.app --args reference_gt.obj candidate.obj
```

Open one MeshLab project containing 2–8 mesh layers by itself to load its
layers, names, order, and transforms into a single overlapping viewport:

```bash
open build/dist/meshcompare.app --args comparison.mlp
```

Use the batch CLI to render that same project as a linked comparison grid
without opening the application window. It writes `<project>.grid.png` into
the requested output directory:

```bash
build/dist/meshcompare.app/Contents/MacOS/meshcompare \
  --render-grid comparison.mlp --output-dir ./renders --size 3840x2160 \
  --camera 2,2,2 --look-at 0,0,0 --up 0,0,1 --fov 45
```

To reuse a pose saved from the application's Camera panel, pass its workspace
UID and saved view ID instead of explicit camera coordinates:

```bash
build/dist/meshcompare.app/Contents/MacOS/meshcompare \
  --render-grid comparison.mlp --output-dir ./renders --size 3840x2160 \
  --camera-pose workspace-uuid:view_001
```

`--render-grid` accepts exactly one `.mlp` input and exits after the PNG is
written. `--size` is optional, sets the final PNG pixel dimensions, and
defaults to `2048x1152` (maximum: 16,384 pixels per side and 64 megapixels).
`--camera` and `--look-at` optionally set a shared world-space view for every
grid cell; `--up` defaults to `0,1,0`, and `--fov` defaults to 60 degrees.
`--camera-pose` restores the complete saved MeshLab view and cannot be combined
with the explicit camera options.
Run `meshcompare --help` for the full command-line usage.

On macOS, `.mlp` is also registered as a document type, so a project can be
opened from Finder. The in-app Open dialog and drag-and-drop accept the same
project files. An MLP project cannot be mixed with ordinary mesh paths in one
open request. Every listed mesh layer participates in comparison; saved
MeshLab visibility flags are not used.

The first mesh whose name contains the approved `gt` token becomes the initial
Reference. If no mesh matches, the first imported mesh is used and the status
area explains the fallback.

## Application data

On macOS, Mesh Compare uses its own application-data directory:

```text
~/Library/Application Support/VCG/MeshCompare/
```

The camera pose library is:

```text
~/Library/Application Support/VCG/MeshCompare/meshlab_lizd_camera_poses.json
```

On first launch, when this file does not yet exist, Mesh Compare copies an
existing legacy library from the customized MeshLab data directory. It prefers
an existing `MeshLab_64bit_fp` library and otherwise uses `MeshLab_64bit_dp`.
Migration copies once; it never renames, deletes, or rewrites the legacy file.

## Diagnostics

The upper-right diagnostics menu contains only Reset Camera, Orthographic,
Wireframe Overlay, Show Normals, Copy Diagnostics, Open Local Log, and About.
Copied diagnostics include renderer and OpenGL information but never mesh
source paths.

The local JSONL log is stored at:

```text
~/Library/Application Support/VCG/MeshCompare/meshcompare-diagnostics.jsonl
```

It rotates before exceeding 2 MiB and retains one previous file named
`meshcompare-diagnostics.previous.jsonl`. Logged mesh names containing `/` or
`\` are replaced with `path_like_name_omitted`.
