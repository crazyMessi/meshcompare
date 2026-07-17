# Standalone Mesh Comparison Application Design

Date: 2026-07-14
Last revised: 2026-07-16
Status: Interactive design approved; adaptive viewport-layout revision approved

## 1. Background and Evidence

The project is currently a customized MeshLab fork with multi-mesh synchronized comparison, analytical coloring, and a UUID-based camera pose library. Its interface still inherits MeshLab's `MainWindow`, MDI, menus, toolbars, Filter Dock, and Layer Dialog. The product's narrow workflow no longer justifies that interface complexity.

The local usage log contains 121 events across 34 sessions:

- Mesh comparison was entered 30 times.
- Analytical coloring was requested 10 times.
- UUID camera pose export was used twice.
- Tracked traditional MeshLab operations were almost never used.

The final product direction is not a reskinned MeshLab. It is a new standalone application with its own single-workspace window, file management, and workflow. It reuses the MeshLab viewport engine only through an encapsulated Renderer Adapter.

## 2. Product Goals

The first release serves four core workflows:

1. Import 2–8 meshes in one batch and automatically create synchronized comparison viewports.
2. Apply Precision or Normal Agreement analytical coloring to every non-Reference mesh.
3. Assign a uniform color to the currently selected mesh.
4. Save, list, apply, and automatically restore camera poses by mesh UUID.

The product must preserve MeshLab's rotation, zoom, and pan behavior, as well as its existing default lighting, gradient background, and rendering character.

## 3. Non-Goals

The first release explicitly excludes:

- MeshLab's `MainWindow`, MDI, or multi-project windows.
- The old menus, toolbars, Filter Dock, and Layer Dialog.
- Access to the complete MeshLab feature set or a “traditional mode” switch.
- General-purpose filter, edit, decorator, or shader workflows.
- Mesh geometry editing, exporting colored mesh results, or saving project files.
- Opening multiple comparison sets at the same time.
- External camera file interchange. In this document, camera “import/export” means reading and writing the local UUID pose library.

Analytical and uniform coloring are temporary presentation states inside the current workspace. They do not create mesh results that must be saved or exported.

## 4. Product Behavior

### 4.1 Single Workspace

One window holds one set of meshes for comparison. The application state is one of:

- `Empty`: no mesh is loaded.
- `Loading`: a staged workspace is being built.
- `Ready`: browsing, coloring, and camera actions are available.
- `Analyzing`: a background analysis is running.
- `FatalError`: the renderer cannot initialize and the application cannot continue.

Whenever the workspace is non-empty, starting another batch import asks for confirmation. The new import will replace the current meshes and temporary coloring results. The application does not maintain an “unexported result” flag.

### 4.2 Batch Import

The empty state provides a drop target and an “Import Meshes” button. The user chooses 2–8 files at once. A successful import enters comparison mode immediately, without an intermediate single-view mode or Layer Dialog.

Import is transactional:

1. Parse files into a staged `MeshRepository`.
2. Validate the count, formats, non-empty triangle faces, and renderable attributes.
3. Assign a stable `MeshId`, display name, and UUID candidates to each mesh.
4. Resolve the Reference mesh.
5. Ask the Renderer Adapter to prepare the viewports and GPU resources.
6. Replace the active workspace only after every step succeeds.

If any file fails to parse or validate, or if GPU preparation fails, all staged resources are released and the old workspace remains unchanged.

The first release preserves the mesh file types supported by the current MeshLab import pipeline. I/O plugins may be used only as headless infrastructure. Plugins, parameter dialogs, and filter workflows are never exposed to the user.

### 4.3 Reference Selection

Reference resolution follows these rules:

1. If a filename or mesh label contains `gt`, case-insensitively, select the first match in import order.
2. If there is no match, temporarily select the first imported mesh.
3. If there are multiple matches or no match, show a non-blocking notice.
4. The user can always change the Reference in the coloring panel.

Changing the Reference clears existing analysis scores and analytical colors, but it does not clear meshes that are currently using a uniform color. The user can then run a new analysis.

### 4.4 Comparison Viewports

The viewport layout follows the customized `meshlab_lizd` comparison behavior
without depending on its `MainWindow` or `MultiViewer_Container`. It begins with
one leaf and adds each remaining viewport by selecting the largest current leaf,
then splitting that leaf 50/50 along its longest axis. Equal-area ties are
resolved by choosing the topmost, then leftmost leaf. The initial splitter
topology is calculated from the host geometry when a scene is created. This
longest-side BSP layout fills the complete host for every mesh count from 2
through 8, including odd counts; it never reserves an empty grid cell.

The splitter tree is private to the new `ViewportGrid`. Splitters are
non-collapsible and use 2-pixel handles, matching the visual separation of the
customized MeshLab comparison view. Viewports retain stable mesh identity, and
the completed tree receives meshes in left-to-right, top-to-bottom order; the
legacy split-insertion ordering is not reproduced. Handles remain draggable but
cannot collapse a pane. Later window resizing preserves the committed topology
and scales its panes; it does not rebuild viewport widgets, OpenGL contexts, or
mesh resources. Creating a new scene uses the host's current aspect ratio to
construct a fresh topology.

Each viewport displays exactly one mesh. All viewports share a camera by default. Rotating, zooming, or panning in any viewport updates every other viewport. Clicking a viewport changes only the selected mesh; it does not unlink the cameras.

The upper-left corner of each viewport shows a compact name. The Reference has an explicit badge. After analysis, each non-Reference viewport shows its metric and score. The selected viewport uses a subtle accent border.

### 4.5 Coloring

The coloring panel is a temporary panel opened from the top command bar. It contains three modes:

- Uniform Color.
- Precision.
- Normal Agreement.

Each mesh has exactly one current presentation state:

- `Default`.
- `UniformColor`.
- `PrecisionResult`.
- `NormalAgreementResult`.

Uniform color applies only to the currently selected mesh. It overrides the default material through rendering options and does not modify vertices, faces, or geometry.

Analysis uses the current Reference and processes every non-Reference mesh by default. The existing algorithm and defaults are preserved:

- Source and Reference each use 500,000 area-uniform samples by default.
- Precision uses a default distance threshold of `0.004`.
- Normal Agreement uses the absolute normal dot product by default.
- Face-aggregated results use a red–yellow–green map.
- Every live face receives an analysis color. Faces without random samples are
  evaluated deterministically at their centroid; zero-area faces receive a
  defined zero score.
- A source containing only zero-area faces returns a zero global score and
  colors every live face with the zero-score color. The Reference must still
  contain a positive-area face.

Sample count, Precision threshold, and normal-direction behavior live in a collapsed “Advanced Parameters” section of the coloring panel.

Analysis runs in a bounded, cancellable background work queue. Results for all targets are staged and committed together only after every target succeeds. Cancellation or failure never leaves a partially updated result. During analysis, each target viewport reports progress. “Clear Coloring” returns every mesh to MeshLab's default presentation.

### 4.6 Camera Poses

The camera panel lists the saved poses associated with the current Reference UUID and supports:

- Saving the complete current camera state.
- Applying a selected saved pose.
- Deleting a selected saved pose.
- Automatically applying the latest pose after mesh import.

UUID candidates from the Reference take priority. If the Reference has no UUID, the application checks candidates from the remaining meshes in import order. If no valid UUID exists, comparison remains available, but saving a pose is disabled with an explanation.

Saving a pose persists it immediately; there is no unsaved camera list. If automatic restore fails, the default camera remains active and the application shows a non-blocking notice.

The new application uses its own AppLocalData directory and camera library file. On first launch, if the new library does not exist and the customized MeshLab application's legacy `meshlab_lizd_camera_poses.json` does exist, the application copies and imports it. The legacy file is never deleted or modified. Existing schema 2 data and MeshLab view-state XML remain compatible.

### 4.7 Minimal Diagnostic Options

The upper-right overflow menu contains only:

- Reset Camera.
- Perspective/Orthographic toggle.
- Wireframe Overlay.
- Show Normals.
- Copy Diagnostics.
- Open Local Log.
- About.

The native macOS application menu contains only platform-required items such as About and Quit. It does not recreate the MeshLab menu system.

## 5. Interface Structure

`StandaloneMainWindow` has three areas:

1. A top command bar with the application name, import, coloring, camera actions, linked-camera status, mesh count, and diagnostics menu.
2. A central `ViewportHost` where the Renderer Adapter mounts the adaptive viewport grid.
3. A lightweight bottom status area for interaction hints, background progress, and non-blocking errors.

The coloring and camera panels are transient and never reserve viewport width. Every frequent task can be started from the command bar and completed in a single panel.

## 6. Architecture

### 6.1 Build Boundary

Create a new standalone executable target and application bundle. The existing MeshLab executable may remain during migration as a behavioral reference, but the new target must not link or include:

- `mainwindow.h` or `MainWindow`.
- MDI management code.
- `MultiViewer_Container`.
- Layer Dialog, Filter Dock, or old menu and toolbar code.

Reusable code should move into UI-independent library targets. The new application must never call back into the old main window to obtain shared behavior.

### 6.2 Application Layer

#### `StandaloneMainWindow`

Owns only the window layout, transient panels, and user-input forwarding. It owns no mesh workflow rules and never touches MeshLab renderer types.

#### `WorkspaceController`

Owns the single-workspace state machine and command orchestration: import, replacement confirmation, Reference selection, coloring, camera restore, task cancellation, and shutdown ordering.

#### `WorkspaceState`

Stores application-level state:

- `MeshEntry`: `MeshId`, source path, display name, UUID candidates, and Reference flag.
- The currently selected mesh.
- Each mesh's coloring mode, analysis score, and task status.
- The current Reference.
- The workspace lifecycle state.

`WorkspaceState` never stores `GLArea`, OpenGL contexts, `MLRenderingData`, or QWidget pointers.

### 6.3 Mesh Services

#### `MeshRepository`

Owns mesh geometry for the current and staged workspaces. It exposes stable `MeshResourceId` values and renderer-neutral read-only geometry and rendering-source interfaces. It does not expose `CMeshO`. The initial implementation may wrap VCG/MeshLab data structures internally.

#### `MeshImportService`

Uses headless MeshLab I/O infrastructure to parse files, extract UUID candidates, and build the staged repository.

#### `MeshColorService`

Contains the analytical algorithms extracted from `FilterColorProc` and the uniform-color presentation logic. Analysis is no longer a QAction or filter plugin and can run directly in tests without a window.

#### `CameraPoseStore`

Owns schema 2 JSON reading, atomic writing, legacy migration, listing, saving, loading, and deletion. It works with an independent `CameraPose` type and does not depend on `GLArea`.

### 6.4 Renderer Adapter Boundary

The application layer depends only on `IRendererAdapter`. The interface uses product-level types: `MeshResourceId`, `SceneDescriptor`, `ColorPresentation`, `FaceColorBuffer`, `CameraPose`, and `DiagnosticFlag`.

Its responsibilities are:

- Mounting into an application-provided `ViewportHost`.
- Preparing, committing, and clearing a workspace scene.
- Building the mesh-to-viewport layout.
- Setting the selected viewport and linked-camera behavior.
- Applying uniform colors or analytical face colors.
- Capturing, restoring, and resetting the camera.
- Toggling orthographic mode, wireframe, and normal diagnostics.
- Reporting resource progress, active-viewport changes, camera changes, and renderer errors.

The interface neither accepts nor returns `GLArea`, `MeshDocument`, OpenGL contexts, or old UI types. The adapter owns and mounts its child viewports, while the application sees only `ViewportHost` and the Renderer Adapter contract.

### 6.5 MeshLab Renderer Backend

#### `MeshLabRendererAdapter`

Translates product-level Renderer Adapter commands into MeshLab viewport engine operations and normalizes lifecycle behavior and errors.

#### `ViewportGrid`

Replaces `MultiViewer_Container`. It owns a renderer-private, longest-side BSP
splitter tree for 2–8 viewports, the active viewport, and camera synchronization.
It owns no menu or project state and does not link or call the legacy container.

#### `RenderSceneContext`

Internally owns `MeshDocument`, `MLSceneGLSharedDataContext`, per-viewport `MLRenderingData`, and shared GPU resources. These types never cross the Adapter boundary.

#### `MeshLabViewport`

Extracts and preserves the following capabilities from `GLArea`:

- The QGL/OpenGL drawing core.
- Trackball and camera behavior.
- MeshLab's default lighting and gradient background.
- `MLRenderingData` rendering.
- Shared-resource drawing and necessary viewport labels.

Its constructor explicitly receives the Scene, Render Settings, and Viewport Callbacks. The new backend must not search the parent object chain for `MainWindow`, and it must not inspect `QApplication::activeWindow()` to determine camera synchronization.

The current `GLArea` contains 17 `mw()` call sites and directly depends on `MultiViewer_Container`. This work is therefore not a wrapper-only change. Raster, editor, decorator, filter, Layer Dialog, and menu synchronization paths must be removed from the new viewport. Any retained behavior must arrive through explicit injected interfaces.

### 6.6 Dependency Direction

Dependencies flow in one direction only:

`UI → WorkspaceController → Services / IRendererAdapter → MeshLabRendererAdapter → MeshLab viewport core`

The MeshLab backend never calls `StandaloneMainWindow`. It reports state only through Adapter events or callbacks.

## 7. Key Data Flows

### 7.1 Import

`Import command → staged MeshRepository → validation → Reference resolution → renderer.prepareScene → renderer.commitScene → WorkspaceState commit → camera auto-restore`

The old workspace remains usable until commit. If preparing new GPU resources exceeds available memory, the operation fails and preserves the old workspace.

### 7.2 Analytical Coloring

`Analyze command → immutable source/reference snapshots → background sampling → staged scores and face colors → validation → repository result commit → renderer color-buffer refresh`

Each background task carries a workspace generation ID. After workspace replacement or cancellation, an old task can never commit results into the new workspace.

### 7.3 Camera

`viewport interaction → renderer camera event → current CameraPose snapshot`

Save flow:

`captureCamera → CameraPoseStore atomic write → refresh list`

Automatic restore flow:

`resolve UUID → load latest pose → renderer.restoreCamera → linked viewports update`

## 8. Error Handling

- Import errors are grouped by file and never create a partial workspace.
- Counts outside 2–8, meshes without valid triangle faces, and unsupported formats produce specific explanations.
- GPU preparation failure releases staged resources and preserves the old workspace.
- Invalid analysis parameters are rejected before a task starts.
- If any target analysis fails, the batch's new results are not committed.
- A damaged camera library, missing pose, or restore failure preserves the current camera and produces a non-blocking notice.
- Failure to initialize the shared OpenGL context is a startup-level error. The application shows copyable diagnostics and exits safely.

Workspace replacement and application shutdown follow a fixed order: stop and join background analysis → disconnect viewport callbacks → destroy child viewports → release shared GPU resources → release the mesh repository. This avoids stale viewer and context access previously seen in the legacy code.

## 9. Testing Strategy

Per the implementation directive for the 2026-07-16 adaptive-layout revision,
no new automated tests are required and the existing test suite is not run for
this revision. Verification is limited to successfully building the application.

### 9.1 Unit Tests

- Automatic and manual Reference selection.
- Transactional workspace import and generation IDs.
- Coloring presentation states and clear behavior.
- Precision, Normal Agreement, and color mapping.
- Camera library schema, atomic writes, legacy migration, and UUID resolution.
- Error normalization and state-machine transitions.

### 9.2 Adapter Contract Tests

Use a Fake Renderer to prove that the application layer depends only on `IRendererAdapter`. Cover scene prepare/commit, color refresh, camera events, cancellation, and fatal errors.

The same contract suite must also run against `MeshLabRendererAdapter`.

### 9.3 Integration Tests

- Creating, rearranging, and destroying 2–8 viewports.
- Uploading shared OpenGL resources once and referencing them correctly from every viewport.
- Camera synchronization, capture, and restore.
- Switching among default, uniform-color, and analytical face-color presentations.
- Repeated workspace replacement and application shutdown without crashes, use-after-free, or stale contexts.

### 9.4 UI and Render Regression Tests

- Confirmation before replacing a non-empty workspace.
- Transient coloring and camera panels.
- Analysis progress, cancellation, and score badges.
- Screenshot regression with fixed meshes, camera, and render settings, allowing a small cross-GPU pixel tolerance.
- Manual comparison with the existing MeshLab `GLArea` to verify matching rotation, zoom, and pan direction and speed.

## 10. Completion Criteria

The implementation is complete only when all of the following are true:

1. The new application target does not link the old `MainWindow`, MDI, `MultiViewer_Container`, Filter Dock, or Layer Dialog.
2. A batch of 2–8 meshes imports directly into synchronized comparison viewports.
3. Automatic `gt` Reference selection and manual Reference changes both work.
4. Precision and Normal Agreement process every non-Reference mesh in one operation.
5. The selected mesh can use a uniform color, and coloring can be cleared.
6. UUID poses can be saved, listed, applied, deleted, and automatically restored, and the legacy library can be migrated.
7. Viewport interaction, default lighting, and background match the current customized MeshLab application.
8. The Renderer Adapter boundary leaks no legacy viewport or OpenGL implementation types.
9. Repeated import, analysis cancellation, and application exit pass lifecycle integration tests.
10. The UI contains none of MeshLab's old menus, toolbars, Layer Dialog, or Filter Dock.

## 11. Confirmed Design Decisions

- Build a new standalone application instead of hiding features in MeshLab's `MainWindow`.
- Use one window and one workspace; do not support MDI.
- Use a single top command bar; do not use a persistent sidebar.
- Enter comparison immediately after batch import.
- Automatically select `gt` as the Reference.
- Analyze every non-Reference mesh by default.
- Preserve automatic UUID camera restore.
- Do not export colored mesh results.
- Always confirm before replacing a non-empty workspace.
- Keep only a minimal set of rendering diagnostics.
- Lay out comparison viewports by repeatedly splitting the largest pane along its longest axis; never leave a blank pane for odd mesh counts.
- Require explicit dependency injection at the Renderer Adapter boundary; the new viewport backend must never search for the old `MainWindow`.
