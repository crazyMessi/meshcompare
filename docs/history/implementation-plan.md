# Standalone Mesh Comparison Application Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone single-workspace “Mesh Compare” application that batch-imports 2–8 meshes, opens synchronized MeshLab-style viewports, supports analytical and uniform coloring, and persists UUID camera poses without linking MeshLab's legacy window or workflow UI.

**Architecture:** A Qt application layer owns `WorkspaceState`, transient panels, and orchestration. It speaks only to service interfaces and `IRendererAdapter`; the initial `MeshLabRendererAdapter` hides `MeshDocument`, `MLSceneGLSharedDataContext`, `MLRenderingData`, Trackball, and OpenGL widgets behind that boundary. The implementation is delivered as vertical slices so every task ends with a testable artifact and the existing `meshlab` target remains buildable throughout extraction.

**Tech Stack:** C++14, Qt 5.15 Widgets/OpenGL/Xml/Network/Test, CMake 3.18+, Ninja, VCGLib, legacy `QGLWidget`, `MLSceneGLSharedDataContext`, Qt Test, CTest.

## Global Constraints

- The new target is named `meshcompare`; its working display name is “Mesh Compare” and its macOS bundle identifier is `org.vcg.meshcompare`.
- One window owns one workspace; 2–8 loaded mesh layers are allowed and MDI is forbidden.
- The `meshcompare` target must not link or include `MainWindow`, `mainwindow.h`, `MultiViewer_Container`, Layer Dialog, Filter Dock, or the legacy menu/toolbar implementation.
- The application layer must not accept or return `GLArea`, `MeshDocument`, `MLRenderingData`, OpenGL contexts, or legacy UI types.
- New viewport code receives its scene, settings, and callbacks explicitly. It must not search a parent chain for `MainWindow` or inspect `QApplication::activeWindow()`.
- Mesh geometry/project export and external camera-file import/export are out of scope. Uniform and analytical colors are temporary presentation state; camera persistence means the local UUID pose library only.
- Existing `meshlab` behavior and the currently modified files are preserved. Stage exact task files only; never stage unrelated dirty-worktree changes.
- Current baseline command: `/opt/homebrew/bin/cmake --build build --target meshlab -j 4` passes with only existing deprecation warnings.
- Production code remains C++14; do not introduce C++17 types such as `std::optional` or `std::span`.
- Every fallible state transition and adapter operation reports a structured success or error result; no service opens a dialog directly.
- Analysis results are generation-scoped and committed atomically; cancellation or replacement cannot publish stale results.
- Workspace replacement destroys resources in this order: join analysis → disconnect callbacks → destroy viewports → release shared GPU resources → release the repository.

## File Structure

The implementation uses these responsibility boundaries:

```text
src/meshcompare/
  CMakeLists.txt                         build targets and test registration
  main.cpp                               process initialization and composition root
  core/
    meshcompare_types.h/.cpp             renderer-neutral IDs, states, poses, errors
    renderer_adapter.h                   application-facing renderer contract
    mesh_resource_provider.h             read-only renderer-neutral geometry contract
    workspace_state.h/.cpp               single-workspace state and generation
    reference_resolver.h/.cpp            deterministic gt selection
  services/
    mesh_import_service.h/.cpp           staged transactional batch import
    surface_comparison.h/.cpp            deterministic sampled comparison algorithm
    mesh_color_service.h/.cpp            batch analysis and uniform-color orchestration
    camera_pose_store.h/.cpp             schema-2 storage, migration, and deletion
  infrastructure/meshlab/
    meshlab_mesh_repository.h/.cpp       MeshDocument-backed repository
    meshlab_mesh_loader.h/.cpp           headless IO-plugin loader
  renderer/meshlab/
    viewport_dependencies.h              explicit scene/settings/callback bundle
    mesh_lab_viewport.h/.cpp              minimal GLArea-derived viewport core
    viewport_grid.h/.cpp                  2–8 layout, selection, linked cameras
    render_scene_context.h/.cpp           MeshDocument/shared-context lifecycle
    mesh_lab_renderer_adapter.h/.cpp      IRendererAdapter implementation
  app/
    application_startup.h/.cpp            neutral renderer startup/fatal transition
    standalone_main_window.h/.cpp         command bar, viewport host, status region
    workspace_controller.h/.cpp           import/replace/restore workflow
    coloring_panel.h/.cpp                 uniform and analytical coloring UI
    camera_panel.h/.cpp                   saved-pose UI
    diagnostics_menu.h/.cpp               minimal renderer diagnostics
  tests/
    fakes/                                 fake services, renderer, and viewport factory
    *_test.cpp                             Qt Test executables
    fixtures/                              tiny deterministic OBJ/PLY/pose fixtures
```

Library targets are `meshcompare-core`, `meshcompare-meshlab-infrastructure`,
`meshcompare-meshlab-renderer`, and `meshcompare-ui`. The executable links all four.
`meshcompare-core` never links either MeshLab-specific target or `meshcompare-ui`.

---

### Task 1: Add the Test Harness and Stable Core Contracts

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Create: `src/meshcompare/CMakeLists.txt`
- Create: `src/meshcompare/core/meshcompare_types.h`
- Create: `src/meshcompare/core/meshcompare_types.cpp`
- Create: `src/meshcompare/core/mesh_resource_provider.h`
- Create: `src/meshcompare/core/renderer_adapter.h`
- Create: `src/meshcompare/tests/fakes/fake_renderer_adapter.h`
- Create: `src/meshcompare/tests/renderer_adapter_contract_test.cpp`

**Interfaces:**
- Produces: `MeshId`, `MeshResourceId`, `WorkspacePhase`, `ColorMode`, `CameraPose`, `SceneDescriptor`, `OperationResult`, `IMeshResourceProvider`, and `IRendererAdapter`.
- Consumes: Qt Core/Gui/Widgets only. No MeshLab renderer types are permitted in these headers.

- [ ] **Step 1: Register CTest and write the failing adapter contract test**

Add `include(CTest)` after `project(MeshLab)` and find `Qt5::Test` only when `BUILD_TESTING` is enabled. Add `add_subdirectory(meshcompare)` beside `add_subdirectory(meshlab)` in `src/CMakeLists.txt`.

Create `renderer_adapter_contract_test.cpp` first:

```cpp
#include <QtTest>
#include "../core/renderer_adapter.h"
#include "fakes/fake_renderer_adapter.h"

class RendererAdapterContractTest : public QObject
{
    Q_OBJECT
private slots:
    void prepareCommitAndDiscardAreExplicit()
    {
        FakeRendererAdapter renderer;
        SceneDescriptor scene;
        scene.generation = 7;
        scene.meshes = {
            {11, 101, QStringLiteral("GT"), true},
            {12, 102, QStringLiteral("Result"), false}
        };
        scene.referenceId = 11;

        QCOMPARE(renderer.prepareScene(scene).ok, true);
        QCOMPARE(renderer.preparedGeneration(), quint64(7));
        renderer.commitPreparedScene();
        QCOMPARE(renderer.committedGeneration(), quint64(7));

        scene.generation = 8;
        QCOMPARE(renderer.prepareScene(scene).ok, true);
        renderer.discardPreparedScene();
        QCOMPARE(renderer.committedGeneration(), quint64(7));
    }
};

QTEST_GUILESS_MAIN(RendererAdapterContractTest)
#include "renderer_adapter_contract_test.moc"
```

- [ ] **Step 2: Configure and verify the test fails before the contracts exist**

Run:

```bash
/opt/homebrew/bin/cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
/opt/homebrew/bin/cmake --build build --target meshcompare-renderer-adapter-contract-test -j 4
```

Expected: build fails because `renderer_adapter.h` and `FakeRendererAdapter` do not exist.

- [ ] **Step 3: Implement the renderer-neutral contracts**

Use these exact public shapes in `meshcompare_types.h`:

```cpp
#pragma once
#include <QColor>
#include <QString>
#include <QVector>

using MeshId = quint64;
using MeshResourceId = quint64;

enum class WorkspacePhase { Empty, Loading, Ready, Analyzing, FatalError };
enum class ColorMode { Default, UniformColor, PrecisionResult, NormalAgreementResult };
enum class DiagnosticFlag { Orthographic, Wireframe, Normals };

struct OperationResult {
    bool ok = true;
    QString error;
    static OperationResult success() { return {}; }
    static OperationResult failure(const QString& value) {
        OperationResult result;
        result.ok = false;
        result.error = value;
        return result;
    }
};

struct CameraPose { QString viewStateXml; };

struct ColorPresentation {
    ColorMode mode = ColorMode::Default;
    QColor uniformColor;
    QVector<QColor> faceColors;
};

struct MeshColorPresentationUpdate {
    MeshId meshId = 0;
    ColorPresentation presentation;
};

struct RendererDiagnostics {
    QString backendName;
    QString openGlVendor;
    QString openGlRenderer;
    QString openGlVersion;
};

struct SceneMesh {
    MeshId id = 0;
    MeshResourceId resourceId = 0;
    QString label;
    bool isReference = false;
};

struct SceneDescriptor {
    quint64 generation = 0;
    QVector<SceneMesh> meshes;
    MeshId referenceId = 0;
};
```

Create `meshcompare_types.cpp` as the translation unit for the core library:

```cpp
#include "meshcompare_types.h"
```

Define `IMeshResourceProvider` as a read-only, renderer-neutral interface:

```cpp
#include <array>
#include <QVector3D>

class IMeshGeometryView {
public:
    virtual ~IMeshGeometryView() = default;
    virtual int vertexCount() const = 0;
    virtual QVector3D vertexPosition(int index) const = 0;
    virtual QVector3D vertexNormal(int index) const = 0;
    virtual int faceCount() const = 0;
    virtual std::array<int, 3> faceVertexIndices(int index) const = 0;
};

class IMeshResourceProvider {
public:
    virtual ~IMeshResourceProvider() = default;
    virtual const IMeshGeometryView* geometry(MeshResourceId id) const = 0;
};
```

Define `IRendererAdapter` with no legacy renderer types:

```cpp
#include <functional>

class QWidget;

struct RendererEvents {
    std::function<void(MeshId)> selectedMeshChanged;
    std::function<void(const CameraPose&)> cameraChanged;
    std::function<void(MeshId, int)> resourceProgress;
    std::function<void(const QString&)> rendererError;
};

class IRendererAdapter {
public:
    virtual ~IRendererAdapter() = default;
    virtual OperationResult mount(QWidget* viewportHost) = 0;
    virtual void setEvents(RendererEvents events) = 0;
    virtual OperationResult prepareScene(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources) = 0;
    virtual void commitPreparedScene() = 0;
    virtual void discardPreparedScene() = 0;
    virtual void clearScene() = 0;
    virtual void setSelectedMesh(MeshId meshId) = 0;
    virtual void setReferenceMesh(MeshId meshId) = 0;
    virtual OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>& updates) = 0;
    virtual CameraPose captureCamera() const = 0;
    virtual OperationResult restoreCamera(const CameraPose& pose) = 0;
    virtual void resetCamera() = 0;
    virtual void setDiagnostic(DiagnosticFlag flag, bool enabled) = 0;
    virtual RendererDiagnostics diagnostics() const = 0;
};
```

Implement the fake by recording the prepared and committed generation and returning `OperationResult::success()`.

Register focused libraries and one CTest executable per test file in `src/meshcompare/CMakeLists.txt`:

```cmake
add_library(meshcompare-core STATIC ${MESHCOMPARE_CORE_SOURCES})
target_link_libraries(meshcompare-core PUBLIC Qt5::Widgets)
target_include_directories(meshcompare-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

function(add_meshcompare_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE meshcompare-core Qt5::Test)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

if(BUILD_TESTING)
  add_meshcompare_test(
    meshcompare-renderer-adapter-contract-test
    tests/renderer_adapter_contract_test.cpp)
endif()
```

- [ ] **Step 4: Build and run the contract test**

Run:

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-renderer-adapter-contract-test -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-renderer-adapter-contract --output-on-failure
```

Expected: one test passes.

- [ ] **Step 5: Verify the public boundary contains no legacy types**

Run:

```bash
rg -n 'GLArea|MainWindow|MeshDocument|MLRenderingData|QGLContext|MultiViewer' \
  src/meshcompare/core
```

Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/CMakeLists.txt src/meshcompare/CMakeLists.txt \
  src/meshcompare/core src/meshcompare/tests/fakes/fake_renderer_adapter.h \
  src/meshcompare/tests/renderer_adapter_contract_test.cpp
git commit -m "test: add mesh compare core contracts"
```

---

### Task 2: Implement Workspace State and Reference Resolution

**Files:**
- Create: `src/meshcompare/core/workspace_state.h`
- Create: `src/meshcompare/core/workspace_state.cpp`
- Create: `src/meshcompare/core/reference_resolver.h`
- Create: `src/meshcompare/core/reference_resolver.cpp`
- Create: `src/meshcompare/tests/workspace_state_test.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `MeshId`, `MeshResourceId`, `WorkspacePhase`, and `ColorMode` from Task 1.
- Produces: `MeshEntry`, `MeshPresentationState`, `ReferenceResolution`, `resolveReference()`, and `WorkspaceState`.

- [ ] **Step 1: Write failing state and resolver tests**

```cpp
private slots:
    void gtWinsCaseInsensitively()
    {
        QVector<MeshEntry> meshes = {
            {1, 101, "/tmp/result.obj", "result.obj", {}, false},
            {2, 102, "/tmp/Scene_GT.ply", "Scene_GT.ply", {}, false}
        };
        const ReferenceResolution result = resolveReference(meshes);
        QCOMPARE(result.referenceId, MeshId(2));
        QCOMPARE(result.notice, QString());
    }

    void firstMeshIsFallbackWithNotice()
    {
        QVector<MeshEntry> meshes = {
            {1, 101, "/tmp/a.obj", "a.obj", {}, false},
            {2, 102, "/tmp/b.obj", "b.obj", {}, false}
        };
        const ReferenceResolution result = resolveReference(meshes);
        QCOMPARE(result.referenceId, MeshId(1));
        QVERIFY(!result.notice.isEmpty());
    }

    void replacingWorkspaceIncrementsGeneration()
    {
        WorkspaceState state;
        state.beginLoading();
        state.commitWorkspace({makeEntry(1), makeEntry(2)}, 1);
        QCOMPARE(state.phase(), WorkspacePhase::Ready);
        QCOMPARE(state.generation(), quint64(1));
        state.beginLoading();
        state.commitWorkspace({makeEntry(3), makeEntry(4)}, 3);
        QCOMPARE(state.generation(), quint64(2));
        QCOMPARE(state.selectedMeshId(), MeshId(3));
    }
```

- [ ] **Step 2: Build and verify failure**

Run:

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-workspace-state-test -j 4
```

Expected: failure because `WorkspaceState` and `resolveReference()` are undefined.

- [ ] **Step 3: Implement deterministic reference resolution**

Use a case-insensitive substring match against both `sourcePath` and `displayName`. Return a notice for zero or multiple matches:

```cpp
ReferenceResolution resolveReference(const QVector<MeshEntry>& meshes)
{
    QVector<MeshId> matches;
    for (const MeshEntry& mesh : meshes) {
        if (mesh.sourcePath.contains("gt", Qt::CaseInsensitive) ||
            mesh.displayName.contains("gt", Qt::CaseInsensitive))
            matches.push_back(mesh.id);
    }
    if (matches.size() == 1)
        return {matches.front(), {}};
    if (matches.size() > 1)
        return {matches.front(), QStringLiteral("Multiple gt meshes found; using the first import.")};
    return meshes.isEmpty()
        ? ReferenceResolution{}
        : ReferenceResolution{meshes.front().id, QStringLiteral("No gt mesh found; using the first import.")};
}
```

- [ ] **Step 4: Implement state invariants**

`WorkspaceState::commitWorkspace()` must reject fewer than 2 or more than 8 entries, mark exactly one Reference, select the first mesh, reset per-mesh scores, and increment `generation_`. `setReference()` must clear only `PrecisionResult` and `NormalAgreementResult` states; current `UniformColor` states remain.

```cpp
OperationResult WorkspaceState::setReference(MeshId id)
{
    if (!contains(id))
        return OperationResult::failure("Reference mesh does not exist in this workspace.");
    referenceId_ = id;
    for (MeshEntry& mesh : meshes_) {
        mesh.isReference = mesh.id == id;
        if (mesh.presentation.mode == ColorMode::PrecisionResult ||
            mesh.presentation.mode == ColorMode::NormalAgreementResult)
            mesh.presentation = {};
        mesh.score = 0.0;
        mesh.hasScore = false;
    }
    return OperationResult::success();
}
```

- [ ] **Step 5: Run the state tests**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-workspace-state-test -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-workspace-state --output-on-failure
```

Expected: all state and reference cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/meshcompare/core/workspace_state.* src/meshcompare/core/reference_resolver.* \
  src/meshcompare/tests/workspace_state_test.cpp src/meshcompare/CMakeLists.txt
git commit -m "feat: add mesh compare workspace state"
```

---

### Task 3: Create the Standalone Application Shell

**Files:**
- Create: `src/meshcompare/main.cpp`
- Create: `src/meshcompare/app/standalone_main_window.h`
- Create: `src/meshcompare/app/standalone_main_window.cpp`
- Create: `src/meshcompare/tests/standalone_main_window_test.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `WorkspaceState` from Task 2.
- Produces: `StandaloneMainWindow`, object names `commandBar`, `importMeshesButton`, `coloringButton`, `cameraButton`, `viewportHost`, `statusLabel`, and an executable `meshcompare`.

- [ ] **Step 1: Write the failing UI structure test**

```cpp
void hasOnlyTheMinimalShell()
{
    WorkspaceState state;
    StandaloneMainWindow window(state);
    QVERIFY(window.findChild<QWidget*>("commandBar"));
    QVERIFY(window.findChild<QPushButton*>("importMeshesButton"));
    QVERIFY(window.findChild<QWidget*>("viewportHost"));
    QVERIFY(window.findChild<QLabel*>("statusLabel"));
    QVERIFY(window.findChild<QMdiArea*>() == nullptr);
    QCOMPARE(window.menuBar()->actions().size(), 0);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-standalone-main-window-test -j 4
```

Expected: failure because `StandaloneMainWindow` is undefined.

- [ ] **Step 3: Implement the minimal window layout**

Use a `QMainWindow` with one central widget and no dock widgets or application menus:

```cpp
StandaloneMainWindow::StandaloneMainWindow(WorkspaceState& state, QWidget* parent)
    : QMainWindow(parent), state_(state)
{
    setWindowTitle(QStringLiteral("Mesh Compare"));
    setAcceptDrops(true);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(8, 8, 8, 6);
    layout->setSpacing(6);

    commandBar_ = buildCommandBar(root);
    commandBar_->setObjectName("commandBar");
    viewportHost_ = new QWidget(root);
    viewportHost_->setObjectName("viewportHost");
    viewportHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    statusLabel_ = new QLabel(tr("Import 2–8 meshes to begin."), root);
    statusLabel_->setObjectName("statusLabel");

    layout->addWidget(commandBar_);
    layout->addWidget(viewportHost_, 1);
    layout->addWidget(statusLabel_);
    setCentralWidget(root);
}
```

The command bar contains text buttons for Import Meshes, Coloring, Camera, a linked-camera status label, mesh count, and an overflow tool button. Disable Coloring and Camera while `WorkspacePhase::Empty`.

- [ ] **Step 4: Add the independent process entry point**

Declare the executable as an application bundle from the first shell slice so every
later manual launch command is valid:

```cmake
add_library(meshcompare-ui STATIC
  app/standalone_main_window.cpp
  app/standalone_main_window.h)
target_link_libraries(meshcompare-ui PUBLIC meshcompare-core Qt5::Widgets)

add_executable(meshcompare MACOSX_BUNDLE main.cpp)
target_link_libraries(meshcompare PRIVATE
  meshcompare-ui meshcompare-core meshlab-common)
```

```cpp
int main(int argc, char** argv)
{
    MeshLabApplication app(argc, argv);
    QCoreApplication::setOrganizationName("VCG");
    QCoreApplication::setApplicationName("MeshCompare");
    QCoreApplication::setApplicationDisplayName("Mesh Compare");
    std::setlocale(LC_ALL, "C");
    QLocale::setDefault(QLocale::C);

    WorkspaceState state;
    StandaloneMainWindow window(state);
    window.resize(1440, 900);
    window.show();
    return app.exec();
}
```

- [ ] **Step 5: Run UI and legacy-boundary checks**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare meshcompare-standalone-main-window-test -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-standalone-main-window --output-on-failure
rg -n '\bMainWindow\b|QMdiArea|MultiViewer_Container|LayerDialog|FilterDock' src/meshcompare
```

Expected: test passes; `rg` reports only the deliberate `QMdiArea` assertion in the test.

- [ ] **Step 6: Launch the shell manually**

```bash
open build/src/distrib/meshcompare.app
```

Expected: a single “Mesh Compare” window opens with the command bar, empty viewport host, and no MeshLab menus, docks, or toolbars.

- [ ] **Step 7: Commit**

```bash
git add src/meshcompare/main.cpp src/meshcompare/app/standalone_main_window.* \
  src/meshcompare/tests/standalone_main_window_test.cpp src/meshcompare/CMakeLists.txt
git commit -m "feat: add standalone mesh compare shell"
```

---

### Task 4: Add Transactional Headless Mesh Import

**Files:**
- Create: `src/meshcompare/services/mesh_import_service.h`
- Create: `src/meshcompare/services/mesh_import_service.cpp`
- Create: `src/meshcompare/infrastructure/meshlab/meshlab_mesh_repository.h`
- Create: `src/meshcompare/infrastructure/meshlab/meshlab_mesh_repository.cpp`
- Create: `src/meshcompare/infrastructure/meshlab/meshlab_mesh_loader.h`
- Create: `src/meshcompare/infrastructure/meshlab/meshlab_mesh_loader.cpp`
- Create: `src/meshcompare/tests/fakes/fake_mesh_loader.h`
- Create: `src/meshcompare/tests/mesh_import_service_test.cpp`
- Create: `src/meshcompare/tests/fixtures/triangle_a.obj`
- Create: `src/meshcompare/tests/fixtures/triangle_gt.obj`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `MeshEntry`, `MeshResourceId`, `IMeshResourceProvider`, and MeshLab's lower-level headless `meshlab::loadMesh()` path.
- Produces: `IMeshLoader::loadFile()`, `IMeshImportService::stage()`, `MeshImportService`, `StagedWorkspace`, and `MeshLabMeshRepository`.

- [ ] **Step 1: Write failing transactional import tests**

```cpp
void oneFailureRejectsTheEntireBatch()
{
    FakeMeshLoader loader;
    loader.succeed("a.obj", fakeLoadedMesh(1, "a.obj"));
    loader.fail("bad.obj", "bad.obj is corrupt");
    MeshImportService service(loader);

    StagedWorkspace staged = service.stage({"a.obj", "bad.obj"});
    QVERIFY(!staged.result.ok);
    QVERIFY(staged.repository == nullptr);
    QCOMPARE(staged.fileErrors.size(), 1);
}

void validatesTotalLoadedMeshCountNotFileCount()
{
    FakeMeshLoader loader;
    loader.succeed("multi.obj", {fakeLoadedMesh(1, "a"), fakeLoadedMesh(2, "b")});
    MeshImportService service(loader);
    StagedWorkspace staged = service.stage({"multi.obj"});
    QVERIFY(staged.result.ok);
    QCOMPARE(staged.entries.size(), 2);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-import-service-test -j 4
```

Expected: failure because the import service and loader interfaces are undefined.

- [ ] **Step 3: Implement the loader and staged result contracts**

```cpp
struct LoadedMesh {
    MeshResourceId resourceId = 0;
    QString sourcePath;
    QString displayName;
    QStringList uuidCandidates;
};

class IMeshLoader {
public:
    virtual ~IMeshLoader() = default;
    virtual OperationResult loadFile(
        const QString& path,
        MeshLabMeshRepository& destination,
        QVector<LoadedMesh>* loaded) = 0;
};

struct StagedWorkspace {
    OperationResult result;
    std::unique_ptr<MeshLabMeshRepository> repository;
    QVector<MeshEntry> entries;
    QVector<QPair<QString, QString>> fileErrors;
};

class IMeshImportService {
public:
    virtual ~IMeshImportService() = default;
    virtual StagedWorkspace stage(const QStringList& paths) = 0;
};

class MeshImportService final : public IMeshImportService {
public:
    explicit MeshImportService(IMeshLoader& loader);
    StagedWorkspace stage(const QStringList& paths) override;
private:
    IMeshLoader& loader_;
};
```

`MeshLabMeshRepository` implements `IMeshResourceProvider`; neither
`IMeshImportService` nor `StagedWorkspace` exposes its internal `MeshDocument`.

- [ ] **Step 4: Implement atomic staging**

`MeshImportService::stage()` creates a new repository, loads every selected file into it, collects errors, validates the final mesh-layer count, and returns the repository only on success:

```cpp
if (!errors.isEmpty())
    return {OperationResult::failure("One or more meshes could not be loaded."), nullptr, {}, errors};
if (entries.size() < 2 || entries.size() > 8)
    return {OperationResult::failure("Import must produce between 2 and 8 mesh layers."), nullptr, {}, {}};
for (const MeshEntry& entry : entries) {
    if (!repository->hasPositiveAreaFaces(entry.resourceId))
        return {OperationResult::failure(entry.displayName + " has no positive-area triangles."), nullptr, {}, {}};
}
return {OperationResult::success(), std::move(repository), entries, {}};
```

- [ ] **Step 5: Implement the MeshLab-backed loader without dialogs**

`MeshLabMeshLoader::loadFile()` must:

1. Verify that the path exists and is readable.
2. Resolve `IOPlugin*` by suffix from `meshlab::pluginManagerInstance()`.
3. Use the plugin's default `initPreOpenParameter()` values joined with `meshlab::defaultGlobalParameterList()`.
4. Allocate all contained meshes in the destination repository's internal `MeshDocument`.
5. Call `meshlab::loadMesh()`.
6. Convert loaded models to `LoadedMesh` metadata and extract normalized UUID candidates from path and label.
7. Delete every newly added model and return a failure if `MLException` is thrown.

Do not use `RichParameterListDialog`, `QMessageBox`, recent-file settings, or `MainWindow::computeRenderingDataOnLoading()`.

Keep the concrete loader/repository dependency out of `meshcompare-core`:

```cmake
add_library(meshcompare-meshlab-infrastructure STATIC
  services/mesh_import_service.cpp
  infrastructure/meshlab/meshlab_mesh_repository.cpp
  infrastructure/meshlab/meshlab_mesh_loader.cpp)
target_link_libraries(meshcompare-meshlab-infrastructure
  PUBLIC meshcompare-core
  PRIVATE meshlab-common meshlab-common-gui)
target_link_libraries(meshcompare PRIVATE meshcompare-meshlab-infrastructure)
```

- [ ] **Step 6: Run unit and real-fixture tests**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-import-service-test -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-mesh-import-service --output-on-failure
```

Expected: fake rollback cases and real OBJ fixture import pass.

- [ ] **Step 7: Rebuild the legacy application**

```bash
/opt/homebrew/bin/cmake --build build --target meshlab -j 4
```

Expected: successful link with only existing warnings.

- [ ] **Step 8: Commit**

```bash
git add src/meshcompare/services/mesh_import_service.* \
  src/meshcompare/infrastructure/meshlab/meshlab_mesh_repository.* \
  src/meshcompare/infrastructure/meshlab/meshlab_mesh_loader.* \
  src/meshcompare/tests/fakes/fake_mesh_loader.h \
  src/meshcompare/tests/mesh_import_service_test.cpp \
  src/meshcompare/tests/fixtures/triangle_a.obj \
  src/meshcompare/tests/fixtures/triangle_gt.obj src/meshcompare/CMakeLists.txt
git commit -m "feat: add transactional mesh import"
```

---

### Task 5: Extract a MainWindow-Free MeshLab Viewport

**Files:**
- Create: `src/meshcompare/renderer/meshlab/viewport_dependencies.h`
- Create: `src/meshcompare/renderer/meshlab/mesh_lab_viewport.h`
- Create: `src/meshcompare/renderer/meshlab/mesh_lab_viewport.cpp`
- Create: `src/meshcompare/tests/fakes/fake_viewport_callbacks.h`
- Create: `src/meshcompare/tests/mesh_lab_viewport_dependencies_test.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`
- Reference only: `src/meshlab/glarea.h`
- Reference only: `src/meshlab/glarea.cpp`

**Interfaces:**
- Consumes: `CameraPose`; MeshLab `MeshDocument`, `MLSceneGLSharedDataContext`, `GLAreaSetting`, `RichParameterList`, and Trackball internals behind the renderer library.
- Produces: `IViewportCallbacks`, `ViewportDependencies`, and `MeshLabViewport` with no parent-window lookup.

- [ ] **Step 1: Write the failing dependency-injection test**

The test constructs a viewport only from an explicit dependency bundle and asserts camera events go to the callback object:

```cpp
void cameraChangesUseInjectedCallbacks()
{
    TestRenderScene scene;
    FakeViewportCallbacks callbacks;
    ViewportDependencies deps{
        scene.document(), scene.sharedContext(), scene.settings(), callbacks,
        1, 1, 1, QStringLiteral("GT"), true
    };
    MeshLabViewport viewport(nullptr, deps);
    viewport.notifyCameraChangedForTest();
    QCOMPARE(callbacks.cameraChangeCount(), 1);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-lab-viewport-dependencies-test -j 4
```

Expected: failure because the dependency types and viewport do not exist.

- [ ] **Step 3: Define explicit viewport dependencies**

```cpp
class IViewportCallbacks {
public:
    virtual ~IViewportCallbacks() = default;
    virtual void viewportActivated(int viewportId) = 0;
    virtual void cameraChanged(int viewportId, const CameraPose& pose) = 0;
    virtual void rendererError(int viewportId, const QString& message) = 0;
};

struct ViewportDependencies {
    MeshDocument& document;
    MLSceneGLSharedDataContext& sharedContext;
    RichParameterList& settings;
    IViewportCallbacks& callbacks;
    int viewportId;
    int meshModelId;
    int viewportIndex;
    int viewportCount;
    QString label;
    bool selected;
};
```

- [ ] **Step 4: Implement only the retained GLArea behavior**

`MeshLabViewport` inherits `QGLWidget` and takes `ViewportDependencies` in its constructor. Copy and adapt only these `GLArea` behaviors:

- `initializeGL()`, `drawGradient()`, `drawLight()`, `setView()`, `setLightingColors()`;
- Trackball mouse press/move/release, wheel zoom, double-click recenter, and resize behavior;
- `shotFromTrackball()`, `loadShot()`, `viewToText()`, and view-state XML loading;
- drawing the assigned mesh through `MLSceneGLSharedDataContext`;
- the compact viewport label, selected border, and analysis-score badge.

The retained paint path is structurally limited to:

```cpp
void MeshLabViewport::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.beginNativePainting();
    makeCurrent();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    setView();
    drawGradient();
    drawLight();
    glPushMatrix();
    trackball_.GetView();
    trackball_.Apply();
    drawAssignedMesh();
    glPopMatrix();
    painter.endNativePainting();
    drawViewportOverlay(painter);
}
```

Do not copy raster rendering, snapshots, selection, editor, renderer plugins, decorators, help overlays, real-time logs, filter state, Layer Dialog signals, or menu-update signals.

- [ ] **Step 5: Replace implicit synchronization with callbacks**

At the end of an input event that changes Trackball state:

```cpp
callbacks_.cameraChanged(viewportId_, captureCamera());
update();
```

On mouse activation:

```cpp
callbacks_.viewportActivated(viewportId_);
```

There must be no `mw()`, `QApplication::activeWindow()`, `parentmultiview`, or `MultiViewer_Container` member.

- [ ] **Step 6: Run tests and static boundary checks**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-lab-viewport-dependencies-test -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-mesh-lab-viewport-dependencies --output-on-failure
rg -n 'mw\(|MainWindow|activeWindow|MultiViewer_Container|parentmultiview' \
  src/meshcompare/renderer/meshlab/mesh_lab_viewport.*
```

Expected: test passes and `rg` produces no output.

- [ ] **Step 7: Rebuild MeshLab to prove extraction did not regress it**

```bash
/opt/homebrew/bin/cmake --build build --target meshlab -j 4
```

Expected: successful link.

- [ ] **Step 8: Commit**

```bash
git add src/meshcompare/renderer/meshlab/viewport_dependencies.h \
  src/meshcompare/renderer/meshlab/mesh_lab_viewport.* \
  src/meshcompare/tests/fakes/fake_viewport_callbacks.h \
  src/meshcompare/tests/mesh_lab_viewport_dependencies_test.cpp \
  src/meshcompare/CMakeLists.txt
git commit -m "refactor: extract standalone MeshLab viewport"
```

---

### Task 6: Add RenderSceneContext and the Single-Viewport Adapter Slice

**Files:**
- Create: `src/meshcompare/renderer/meshlab/render_scene_context.h`
- Create: `src/meshcompare/renderer/meshlab/render_scene_context.cpp`
- Create: `src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.h`
- Create: `src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.cpp`
- Create: `src/meshcompare/renderer/meshlab/viewport_factory.h`
- Create: `src/meshcompare/tests/fakes/fake_viewport_factory.h`
- Create: `src/meshcompare/tests/mesh_lab_renderer_adapter_test.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `IRendererAdapter`, `IMeshResourceProvider`, `ViewportDependencies`, and `MeshLabViewport`.
- Produces: `IViewportFactory`, `RenderSceneContext`, and `MeshLabRendererAdapter` prepare/commit/discard behavior.

- [ ] **Step 1: Write the failing adapter lifecycle tests**

```cpp
void failedPreparationKeepsCommittedScene()
{
    FakeViewportFactory factory;
    MeshLabRendererAdapter adapter(factory);
    FakeResourceProvider resources;
    QVERIFY(adapter.prepareScene(scene(1, {1, 2}), resources).ok);
    adapter.commitPreparedScene();
    QCOMPARE(adapter.committedGeneration(), quint64(1));

    factory.failNextCreation("OpenGL context unavailable");
    QVERIFY(!adapter.prepareScene(scene(2, {3, 4}), resources).ok);
    QCOMPARE(adapter.committedGeneration(), quint64(1));
}

void clearDestroysViewportsBeforeSharedContext()
{
    FakeViewportFactory factory;
    MeshLabRendererAdapter adapter(factory);
    adapter.prepareScene(scene(1, {1, 2}), resources_);
    adapter.commitPreparedScene();
    adapter.clearScene();
    QCOMPARE(factory.lifecycle(), QStringList({"viewports:destroy", "context:destroy"}));
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-lab-renderer-adapter-test -j 4
```

Expected: failure because the adapter and scene context do not exist.

- [ ] **Step 3: Implement `RenderSceneContext` ownership**

The context owns objects in declaration order and destroys them explicitly in reverse rendering order:

```cpp
class RenderSceneContext {
public:
    explicit RenderSceneContext(const RenderSceneSettings& settings);
    ~RenderSceneContext();
    MeshDocument& document();
    MLSceneGLSharedDataContext& sharedContext();
    OperationResult addResources(
        const SceneDescriptor& scene,
        const IMeshResourceProvider& resources);
private:
    std::unique_ptr<vcg::QtThreadSafeMemoryInfo> memoryInfo_;
    std::unique_ptr<MeshDocument> document_;
    QPointer<MLSceneGLSharedDataContext> sharedContext_;
};
```

`addResources()` creates backend `MeshModel` objects, copies geometry through `IMeshGeometryView`, computes `MLPoliciesStandAloneFunctions::suggestedDefaultPerViewRenderingData()`, registers data for every prepared viewport context, and calls `manageBuffers()` once per mesh.

Build the backend separately from application state and UI:

```cmake
add_library(meshcompare-meshlab-renderer STATIC
  renderer/meshlab/mesh_lab_viewport.cpp
  renderer/meshlab/render_scene_context.cpp
  renderer/meshlab/mesh_lab_renderer_adapter.cpp)
target_link_libraries(meshcompare-meshlab-renderer
  PUBLIC meshcompare-core
  PRIVATE meshlab-common meshlab-common-gui Qt5::OpenGL OpenGL::GL)
target_link_libraries(meshcompare PRIVATE meshcompare-meshlab-renderer)
```

- [ ] **Step 4: Implement transactional adapter state**

Keep `prepared_` and `committed_` scene bundles separate:

```cpp
OperationResult MeshLabRendererAdapter::prepareScene(
    const SceneDescriptor& scene,
    const IMeshResourceProvider& resources)
{
    discardPreparedScene();
    std::unique_ptr<SceneBundle> candidate(new SceneBundle(settings_));
    OperationResult result = candidate->context.addResources(scene, resources);
    if (!result.ok)
        return result;
    result = candidate->createViewports(scene, viewportFactory_, *this);
    if (!result.ok)
        return result;
    prepared_ = std::move(candidate);
    return OperationResult::success();
}

void MeshLabRendererAdapter::commitPreparedScene()
{
    if (!prepared_)
        return;
    destroyBundle(committed_);
    committed_ = std::move(prepared_);
    committed_->mount(viewportHost_);
}
```

- [ ] **Step 5: Run lifecycle tests**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-lab-renderer-adapter-test -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-mesh-lab-renderer-adapter --output-on-failure
```

Expected: prepare/commit/discard and destruction-order cases pass using fake viewports.

- [ ] **Step 6: Commit**

```bash
git add src/meshcompare/renderer/meshlab/render_scene_context.* \
  src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.* \
  src/meshcompare/renderer/meshlab/viewport_factory.h \
  src/meshcompare/tests/fakes/fake_viewport_factory.h \
  src/meshcompare/tests/mesh_lab_renderer_adapter_test.cpp \
  src/meshcompare/CMakeLists.txt
git commit -m "feat: add MeshLab renderer adapter lifecycle"
```

---

### Task 7: Implement the Multi-Viewport Grid and Linked Cameras

**Files:**
- Create: `src/meshcompare/renderer/meshlab/viewport_grid.h`
- Create: `src/meshcompare/renderer/meshlab/viewport_grid.cpp`
- Create: `src/meshcompare/tests/viewport_grid_test.cpp`
- Create: `src/meshcompare/tests/renderer_smoke_test.cpp`
- Modify: `src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `MeshLabViewport`, `IViewportCallbacks`, `SceneDescriptor`, and `CameraPose`.
- Produces: `ViewportLayout viewportLayoutForCount(int)`, `ViewportGrid`, linked-camera propagation, and selection callbacks.

- [ ] **Step 1: Write failing grid and camera tests**

```cpp
void layoutMatchesTheProductRules()
{
    QCOMPARE(viewportLayoutForCount(2), ViewportLayout({1, 2}));
    QCOMPARE(viewportLayoutForCount(3), ViewportLayout({2, 2}));
    QCOMPARE(viewportLayoutForCount(4), ViewportLayout({2, 2}));
    QCOMPARE(viewportLayoutForCount(6), ViewportLayout({3, 2}));
    QCOMPARE(viewportLayoutForCount(8), ViewportLayout({4, 2}));
}

void cameraChangePropagatesWithoutFeedbackLoop()
{
    FakeViewportFactory factory;
    ViewportGrid grid(factory);
    grid.create(scene(1, {1, 2, 3}));
    factory.viewport(0).emitCameraChanged(pose("camera-a"));
    QCOMPARE(factory.viewport(1).restoredPose(), pose("camera-a"));
    QCOMPARE(factory.viewport(2).restoredPose(), pose("camera-a"));
    QCOMPARE(factory.totalCameraChangedEmissions(), 1);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-viewport-grid-test -j 4
```

Expected: failure because the grid and layout function do not exist.

- [ ] **Step 3: Implement deterministic grid placement**

```cpp
ViewportLayout viewportLayoutForCount(int count)
{
    if (count == 2) return {1, 2};
    if (count <= 4) return {2, 2};
    if (count <= 6) return {3, 2};
    return {4, 2};
}
```

`ViewportGrid` owns a `QGridLayout`, one viewport per mesh, and maps `viewportId ↔ MeshId`. Empty grid cells for 3, 5, and 7 meshes remain blank rather than stretching one mesh differently.

- [ ] **Step 4: Implement linked camera propagation with a guard**

```cpp
void ViewportGrid::cameraChanged(int sourceId, const CameraPose& pose)
{
    if (propagatingCamera_)
        return;
    QScopedValueRollback<bool> guard(propagatingCamera_, true);
    for (MeshLabViewport* viewport : viewports_)
        if (viewport->viewportId() != sourceId)
            viewport->restoreCameraSilently(pose);
}
```

`viewportActivated()` updates selected borders and emits the selected `MeshId` to the adapter callback.

- [ ] **Step 5: Add a real OpenGL smoke test**

The smoke test loads the two triangle fixtures, mounts the real adapter in a 1000×600 widget, waits for both viewports to expose valid contexts, performs one camera rotation on the first viewport, and asserts the second viewport's serialized `CameraPose` matches.

```cpp
QTRY_VERIFY_WITH_TIMEOUT(adapter.validViewportCount() == 2, 5000);
adapter.viewportForTest(0)->trackballStep(QStringLiteral("Horizontal +"));
QTRY_COMPARE_WITH_TIMEOUT(
    adapter.viewportForTest(1)->captureCamera().viewStateXml,
    adapter.viewportForTest(0)->captureCamera().viewStateXml,
    2000);
```

- [ ] **Step 6: Run grid, renderer smoke, and legacy build checks**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-viewport-grid-test meshcompare-renderer-smoke-test meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build \
  -R 'meshcompare-(viewport-grid|renderer-smoke)' --output-on-failure
```

Expected: grid and real renderer smoke tests pass; MeshLab links.

- [ ] **Step 7: Commit**

```bash
git add src/meshcompare/renderer/meshlab/viewport_grid.* \
  src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.cpp \
  src/meshcompare/tests/viewport_grid_test.cpp \
  src/meshcompare/tests/renderer_smoke_test.cpp src/meshcompare/CMakeLists.txt
git commit -m "feat: add synchronized mesh viewport grid"
```

---

### Task 8: Orchestrate Batch Import into the Standalone UI

**Files:**
- Create: `src/meshcompare/app/application_startup.h`
- Create: `src/meshcompare/app/application_startup.cpp`
- Create: `src/meshcompare/app/workspace_controller.h`
- Create: `src/meshcompare/app/workspace_controller.cpp`
- Create: `src/meshcompare/tests/fakes/fake_mesh_import_service.h`
- Create: `src/meshcompare/tests/workspace_controller_test.cpp`
- Modify: `src/meshcompare/app/standalone_main_window.h`
- Modify: `src/meshcompare/app/standalone_main_window.cpp`
- Modify: `src/meshcompare/main.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `WorkspaceState`, `IMeshImportService`, `IRendererAdapter`, and `resolveReference()`.
- Produces: `WorkspaceController::importMeshes()`, `WorkspaceController::selectMesh()`, replace-confirmation UI, drag/drop, and automatic comparison entry.

- [ ] **Step 1: Write failing controller tests**

```cpp
void failedRendererPreparationPreservesTheOldWorkspace()
{
    WorkspaceState state = readyWorkspace({1, 2}, 1);
    FakeMeshImportService importer(stagedWorkspace({3, 4}));
    FakeRendererAdapter renderer;
    renderer.failNextPrepare("GPU upload failed");
    WorkspaceController controller(state, importer, renderer);

    OperationResult result = controller.importMeshes({"new-a.obj", "new-b.obj"});
    QVERIFY(!result.ok);
    QCOMPARE(state.meshes().front().id, MeshId(1));
    QCOMPARE(state.generation(), quint64(1));
}

void successfulImportCommitsRendererBeforeState()
{
    // Fake renderer records prepare → commit; state observer records state commit.
    // The expected sequence prevents UI from pointing at an uncommitted renderer scene.
    QCOMPARE(runSuccessfulImportSequence(), QStringList({"prepare", "renderer-commit", "state-commit"}));
}

void fatalRendererInitializationEntersFatalStateOnce()
{
    FakeRendererAdapter renderer;
    renderer.failNextMount("OpenGL 2.1 is unavailable");
    QWidget host;
    const OperationResult result = initializeRenderer(state_, renderer, &host);
    QVERIFY(!result.ok);
    QCOMPARE(state_.phase(), WorkspacePhase::FatalError);
    QCOMPARE(result.error, QString("OpenGL 2.1 is unavailable"));
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-workspace-controller-test -j 4
```

Expected: failure because `WorkspaceController` is undefined.

- [ ] **Step 3: Implement transactional orchestration**

```cpp
OperationResult WorkspaceController::importMeshes(const QStringList& paths)
{
    state_.beginLoading();
    StagedWorkspace staged = importer_.stage(paths);
    if (!staged.result.ok) {
        state_.cancelLoading();
        return staged.result;
    }

    const ReferenceResolution reference = resolveReference(staged.entries);
    const SceneDescriptor scene = makeSceneDescriptor(
        state_.generation() + 1, staged.entries, reference.referenceId);
    OperationResult prepared = renderer_.prepareScene(scene, *staged.repository);
    if (!prepared.ok) {
        renderer_.discardPreparedScene();
        state_.cancelLoading();
        return prepared;
    }

    renderer_.commitPreparedScene();
    repository_ = std::move(staged.repository);
    state_.commitWorkspace(staged.entries, reference.referenceId);
    renderer_.setSelectedMesh(state_.selectedMeshId());
    return OperationResult::success();
}
```

- [ ] **Step 4: Connect import button, drag/drop, and replacement confirmation**

`StandaloneMainWindow` emits `importRequested(QStringList)` after `QFileDialog::getOpenFileNames()`. If the workspace is non-empty, ask exactly once:

```cpp
QMessageBox box(
    QMessageBox::Question,
    tr("Replace Current Workspace?"),
    tr("This will replace the current meshes and temporary coloring results."),
    QMessageBox::Cancel,
    this);
QAbstractButton* replaceButton =
    box.addButton(tr("Replace"), QMessageBox::AcceptRole);
box.exec();
if (box.clickedButton() != replaceButton)
    return;
```

Only call `controller.importMeshes()` after confirmation. Accept drops only when every URL is a local file. Display grouped file errors in one dialog; all other errors go to `statusLabel`.

- [ ] **Step 5: Compose production services in `main.cpp`**

Load plugins once, construct `MeshLabMeshLoader`, `MeshImportService`, `MeshLabRendererAdapter`, `WorkspaceController`, and `StandaloneMainWindow`, then mount the adapter into `window.viewportHost()` through `initializeRenderer()`.

```cpp
meshlab::pluginManagerInstance().loadPlugins();
MeshLabMeshLoader loader;
MeshImportService importer(loader);
MeshLabRendererAdapter renderer;
WorkspaceController controller(state, importer, renderer);
StandaloneMainWindow window(state, controller);
OperationResult mounted = initializeRenderer(state, renderer, window.viewportHost());
if (!mounted.ok)
    return presentFatalStartupError(mounted.error);
```

After `window.show()`, defer command-line import until the event loop can create the
OpenGL widgets:

```cpp
QStringList startupMeshes;
for (int i = 1; i < argc; ++i)
    startupMeshes.push_back(QString::fromLocal8Bit(argv[i]));
if (!startupMeshes.isEmpty())
    QTimer::singleShot(0, [&controller, startupMeshes] {
        controller.importMeshes(startupMeshes);
    });
```

`initializeRenderer()` returns the neutral error from `IRendererAdapter::mount()` and
sets `WorkspacePhase::FatalError` on failure. Convert plugin-loading and
adapter-construction `MLException` values into the same `OperationResult` at the
composition root. Present one copyable error dialog and exit with `EXIT_FAILURE`;
Task 14 wires the same result into the diagnostics log. Renderer callbacks report
later errors through the status area and must not create repeated startup dialogs.

- [ ] **Step 6: Run controller, UI, and application smoke tests**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-workspace-controller-test meshcompare meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build \
  -R 'meshcompare-(workspace-controller|standalone-main-window|renderer-smoke)' \
  --output-on-failure
```

Expected: all selected tests pass and both executables link.

- [ ] **Step 7: Manually verify the first vertical slice**

```bash
open build/src/distrib/meshcompare.app --args \
  src/meshcompare/tests/fixtures/triangle_gt.obj \
  src/meshcompare/tests/fixtures/triangle_a.obj
```

Expected: one independent window, two side-by-side MeshLab-style viewports, linked camera interaction, compact labels, no MeshLab menus/docks, and a selected Reference badge.

- [ ] **Step 8: Commit**

```bash
git add src/meshcompare/app/workspace_controller.* \
  src/meshcompare/app/application_startup.* \
  src/meshcompare/app/standalone_main_window.* src/meshcompare/main.cpp \
  src/meshcompare/tests/fakes/fake_mesh_import_service.h \
  src/meshcompare/tests/workspace_controller_test.cpp src/meshcompare/CMakeLists.txt
git commit -m "feat: connect batch import comparison workflow"
```

---

### Task 9: Extract and Test the Surface Comparison Engine

**Files:**
- Create: `src/meshcompare/services/surface_comparison.h`
- Create: `src/meshcompare/services/surface_comparison.cpp`
- Create: `src/meshcompare/tests/surface_comparison_test.cpp`
- Modify: `src/meshlabplugins/filter_colorproc/filter_colorproc.cpp`
- Modify: `src/meshlabplugins/filter_colorproc/CMakeLists.txt`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: renderer-neutral immutable snapshots and existing sampled-comparison code currently local to `filter_colorproc.cpp`; `CMeshO` remains an implementation/conversion detail only.
- Produces: `SurfaceMeshSnapshot`, `SurfaceComparisonMetric`, `SurfaceComparisonOptions`, `SurfaceComparisonResult`, `SurfaceComparisonOutcome`, `ISurfaceComparer`, `compareSampledSurfaces()`, and `surfaceScoreColors()`.

- [ ] **Step 1: Write deterministic failing algorithm tests**

```cpp
void identicalTrianglesHavePerfectScores()
{
    SurfaceMeshSnapshot source = triangleAtZ(0.0f, false);
    SurfaceMeshSnapshot reference = triangleAtZ(0.0f, false);
    SurfaceComparisonOptions options;
    options.sampleCount = 2000;
    options.distanceThreshold = 0.001f;
    options.randomSeed = 17;

    const SurfaceComparisonOutcome precision = compareSampledSurfaces(
        source, reference, SurfaceComparisonMetric::PrecisionAtThreshold, options);
    const SurfaceComparisonOutcome normals = compareSampledSurfaces(
        source, reference, SurfaceComparisonMetric::NormalAgreement, options);
    QVERIFY(precision.result.ok);
    QVERIFY(normals.result.ok);
    QCOMPARE(precision.comparison.globalScore, 1.0);
    QCOMPARE(normals.comparison.globalScore, 1.0);
}

void offsetTrianglesFailPrecisionButKeepNormalAgreement()
{
    SurfaceMeshSnapshot source = triangleAtZ(0.0f, false);
    SurfaceMeshSnapshot reference = triangleAtZ(1.0f, false);
    SurfaceComparisonOptions options{2000, 0.01f, true, 17};
    const SurfaceComparisonOutcome precision = compareSampledSurfaces(
        source, reference, SurfaceComparisonMetric::PrecisionAtThreshold, options);
    const SurfaceComparisonOutcome normals = compareSampledSurfaces(
        source, reference, SurfaceComparisonMetric::NormalAgreement, options);
    QVERIFY(precision.result.ok);
    QVERIFY(normals.result.ok);
    QCOMPARE(precision.comparison.globalScore, 0.0);
    QCOMPARE(normals.comparison.globalScore, 1.0);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-surface-comparison-test -j 4
```

Expected: failure because the extracted engine does not exist.

- [ ] **Step 3: Move the algorithm behind a reusable API**

```cpp
enum class SurfaceComparisonMetric { PrecisionAtThreshold, NormalAgreement };

struct SurfaceMeshSnapshot {
    QVector<QVector3D> vertices;
    QVector<QVector3D> vertexNormals;
    QVector<std::array<int, 3>> faces;
};

struct SurfaceComparisonOptions {
    int sampleCount = 500000;
    float distanceThreshold = 0.004f;
    bool useAbsoluteNormalDot = true;
    std::uint32_t randomSeed = 0x4d595df4u;
};

struct SurfaceComparisonResult {
    int sampleCount = 0;
    int coloredFaceCount = 0;
    double globalScore = 0.0;
    QVector<float> faceScores;
};

struct SurfaceComparisonOutcome {
    OperationResult result;
    SurfaceComparisonResult comparison;
};

using AnalysisProgress = std::function<bool(int percent, const QString& message)>;

class ISurfaceComparer {
public:
    virtual ~ISurfaceComparer() = default;
    virtual SurfaceComparisonOutcome compare(
        const SurfaceMeshSnapshot& source,
        const SurfaceMeshSnapshot& reference,
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options,
        AnalysisProgress progress = {}) const = 0;
};

SurfaceComparisonOutcome compareSampledSurfaces(
    const SurfaceMeshSnapshot& source,
    const SurfaceMeshSnapshot& reference,
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options,
    AnalysisProgress progress = {});
```

Move area-weighted sampling, KD-tree lookup, scoring, and red–yellow–green mapping
from the anonymous namespace in `filter_colorproc.cpp`. Convert snapshots to any
VCG/KD-tree working types privately inside the `.cpp`; no public header exposes
`CMeshO`. Every live face must receive an analysis score; faces with no random
sample are evaluated deterministically at their centroid. Add a
cancellation/progress callback. An all-zero-area source returns zero scores for
all live faces, while the Reference still requires positive-area geometry:

Return `SurfaceComparisonOutcome{OperationResult::failure("Analysis cancelled."), {}}`
when the callback returns `false`. `SurfaceComparer` is the production
`ISurfaceComparer` implementation and delegates to `compareSampledSurfaces()`.

- [ ] **Step 4: Make the legacy filters delegate to the extracted engine**

Remove the duplicated helper definitions from `filter_colorproc.cpp`. Convert the current `CMeshO` to a `SurfaceMeshSnapshot`, construct `SurfaceComparisonOptions` from RichParameters, call `compareSampledSurfaces()`, write the returned colors to the current mesh, and keep the existing status values.

Build the extracted files as `meshcompare-analysis` and link both consumers explicitly:

```cmake
add_library(meshcompare-analysis STATIC
  services/surface_comparison.cpp
  services/surface_comparison.h)
target_include_directories(meshcompare-analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(meshcompare-analysis PUBLIC Qt5::Gui PRIVATE meshlab-common)
target_link_libraries(meshcompare-core PUBLIC meshcompare-analysis)
target_link_libraries(filter_colorproc PRIVATE meshcompare-analysis)
```

- [ ] **Step 5: Run algorithm, plugin, and legacy build tests**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-surface-comparison-test filter_colorproc meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-surface-comparison --output-on-failure
```

Expected: deterministic algorithm tests pass; plugin and MeshLab link without duplicate symbols.

- [ ] **Step 6: Commit**

```bash
git add src/meshcompare/services/surface_comparison.* \
  src/meshcompare/tests/surface_comparison_test.cpp \
  src/meshlabplugins/filter_colorproc/filter_colorproc.cpp \
  src/meshlabplugins/filter_colorproc/CMakeLists.txt src/meshcompare/CMakeLists.txt
git commit -m "refactor: extract sampled surface comparison"
```

---

### Task 10: Implement Atomic Batch Analysis and Renderer Color Presentations

**Files:**
- Create: `src/meshcompare/services/mesh_color_service.h`
- Create: `src/meshcompare/services/mesh_color_service.cpp`
- Create: `src/meshcompare/tests/fakes/fake_surface_comparer.h`
- Create: `src/meshcompare/tests/mesh_color_service_test.cpp`
- Modify: `src/meshcompare/core/renderer_adapter.h`
- Modify: `src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.cpp`
- Modify: `src/meshcompare/app/workspace_controller.h`
- Modify: `src/meshcompare/app/workspace_controller.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `ISurfaceComparer`, `SurfaceComparisonOptions`, `IMeshResourceProvider`, `WorkspaceState`, and `IRendererAdapter::setColorPresentations()`.
- Produces: `AnalysisRequest`, `AnalysisBatchResult`, `IMeshColorService`, `MeshColorService::startAnalysis()`, `cancelAnalysis()`, `setUniformColor()`, and `clearColoring()`.

- [ ] **Step 1: Write failing atomicity and generation tests**

```cpp
void oneTargetFailureCommitsNothing()
{
    FakeSurfaceComparer comparer;
    comparer.succeed(2, score(0.9));
    comparer.fail(3, "mesh 3 has no valid faces");
    MeshColorService service(resources_, comparer, state_, renderer_);

    QSignalSpy finished(&service, &MeshColorService::analysisFinished);
    service.startAnalysis(request(7, 1, {2, 3}));
    QVERIFY(finished.wait());
    QCOMPARE(renderer_.presentationUpdateCount(), 0);
    QCOMPARE(state_.mesh(2).presentation.mode, ColorMode::Default);
    QCOMPARE(state_.mesh(3).presentation.mode, ColorMode::Default);
}

void staleGenerationNeverCommits()
{
    FakeSurfaceComparer comparer;
    comparer.pauseAfterCompute();
    MeshColorService service(resources_, comparer, state_, renderer_);
    service.startAnalysis(request(7, 1, {2}));
    service.setCurrentGeneration(8);
    comparer.resume();
    QTRY_COMPARE(renderer_.presentationUpdateCount(), 0);
}

void rendererBatchFailureLeavesStateAndPresentationsUnchanged()
{
    comparer_.succeed(2, score(0.9));
    comparer_.succeed(3, score(0.8));
    renderer_.failNextPresentationBatch("GPU buffer allocation failed");
    service_.startAnalysis(request(7, 1, {2, 3}));
    QVERIFY(finished_.wait());
    QCOMPARE(renderer_.presentationUpdateCount(), 0);
    QCOMPARE(state_.mesh(2).presentation.mode, ColorMode::Default);
    QCOMPARE(state_.mesh(3).presentation.mode, ColorMode::Default);
}

void uniformColorDoesNotMutateMeshFaces()
{
    const SurfaceMeshSnapshot original = snapshot(resources_.geometry(2));
    service_.setUniformColor(2, QColor("#5aaa75"));
    const SurfaceMeshSnapshot after = snapshot(resources_.geometry(2));
    QCOMPARE(after.vertices, original.vertices);
    QCOMPARE(after.vertexNormals, original.vertexNormals);
    QCOMPARE(after.faces, original.faces);
    QCOMPARE(renderer_.presentation(2).mode, ColorMode::UniformColor);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-mesh-color-service-test -j 4
```

Expected: failure because `MeshColorService` is undefined.

- [ ] **Step 3: Implement the request and staged-result contracts**

```cpp
struct AnalysisRequest {
    quint64 generation = 0;
    MeshId referenceId = 0;
    QVector<MeshId> targetIds;
    SurfaceComparisonMetric metric = SurfaceComparisonMetric::PrecisionAtThreshold;
    SurfaceComparisonOptions options;
};

struct MeshAnalysisResult {
    OperationResult result;
    MeshId meshId = 0;
    double globalScore = 0.0;
    QVector<QColor> faceColors;
};

struct AnalysisBatchResult {
    OperationResult result;
    quint64 generation = 0;
    QVector<MeshAnalysisResult> meshes;
};

class IMeshColorService {
public:
    virtual ~IMeshColorService() = default;
    virtual OperationResult startAnalysis(const AnalysisRequest& request) = 0;
    virtual void cancelAnalysis() = 0;
    virtual OperationResult setUniformColor(MeshId meshId, const QColor& color) = 0;
    virtual void clearColoring() = 0;
};
```

- [ ] **Step 4: Implement a bounded cancellable worker**

Run one batch at a time on a dedicated `QThread` worker. Before dispatch,
`MeshColorService` copies the Reference and every target from
`IMeshResourceProvider` into owning `SurfaceMeshSnapshot` values. The worker owns no
widgets and never reads the live repository. Stage results in a local vector and emit
one completion event:

```cpp
for (MeshId target : request.targetIds) {
    if (cancelled_.load())
        return failedBatch(request.generation, "Analysis cancelled.");
    MeshAnalysisResult result = compareOne(request, target);
    if (!result.result.ok)
        return failedBatch(request.generation, result.result.error);
    staged.push_back(std::move(result));
}
return {OperationResult::success(), request.generation, staged};
```

The GUI-thread completion slot checks `result.generation == state_.generation()`,
builds one vector of `MeshColorPresentationUpdate`, and calls the adapter once. The
adapter stages all color buffers and either commits all updates or returns a failure
without changing the committed presentations. Only after renderer success does the
service update all state scores/presentation modes. Geometry in
`IMeshResourceProvider` remains immutable; analytical face colors live in
`ColorPresentation` and are temporary.

Inject `ISurfaceComparer&` into `MeshColorService`; `FakeSurfaceComparer` implements
that contract so worker scheduling and comparison behavior are independently testable.

- [ ] **Step 5: Implement MeshLab renderer presentation mapping**

For `UniformColor`, set `MLPerViewGLOptions::_persolid_fixed_color_enabled = true` and the fixed solid color while leaving mesh attributes unchanged. For analysis results, enable `ATT_FACECOLOR`, disable vertex/texture colors for `PR_SOLID`, call `meshAttributesUpdated(meshId, false, faceColorAtts)`, and `manageBuffers(meshId)`. `Default` recomputes `suggestedDefaultPerViewRenderingData()`. `setColorPresentations()` validates and stages every update before swapping the committed per-view rendering data; one staging failure discards the entire batch.

```cpp
MLRenderingData::RendAtts faceColorAtts;
faceColorAtts[MLRenderingData::ATT_NAMES::ATT_VERTPOSITION] = true;
faceColorAtts[MLRenderingData::ATT_NAMES::ATT_FACECOLOR] = true;
renderingData.set(MLRenderingData::PR_SOLID, faceColorAtts);
sharedContext.setRenderingDataPerMeshView(modelId, viewportContext, renderingData);
```

- [ ] **Step 6: Expose controller commands**

Add:

```cpp
OperationResult WorkspaceController::setUniformColor(MeshId meshId, const QColor& color);
OperationResult WorkspaceController::startAnalysis(
    SurfaceComparisonMetric metric,
    const SurfaceComparisonOptions& options);
void WorkspaceController::cancelAnalysis();
void WorkspaceController::clearColoring();
```

`startAnalysis()` derives targets from all non-Reference entries; UI callers cannot provide a partial target set in the default path.

- [ ] **Step 7: Run service, adapter, and lifecycle tests**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-mesh-color-service-test meshcompare-mesh-lab-renderer-adapter-test meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build \
  -R 'meshcompare-(mesh-color-service|mesh-lab-renderer-adapter)' --output-on-failure
```

Expected: atomicity, cancellation, generation, and presentation cases pass.

- [ ] **Step 8: Commit**

```bash
git add src/meshcompare/services/mesh_color_service.* \
  src/meshcompare/tests/fakes/fake_surface_comparer.h \
  src/meshcompare/tests/mesh_color_service_test.cpp \
  src/meshcompare/core/renderer_adapter.h \
  src/meshcompare/renderer/meshlab/mesh_lab_renderer_adapter.cpp \
  src/meshcompare/app/workspace_controller.* src/meshcompare/CMakeLists.txt
git commit -m "feat: add atomic mesh coloring service"
```

---

### Task 11: Add the Transient Coloring Panel and Progress UI

**Files:**
- Create: `src/meshcompare/app/coloring_panel.h`
- Create: `src/meshcompare/app/coloring_panel.cpp`
- Create: `src/meshcompare/tests/coloring_panel_test.cpp`
- Modify: `src/meshcompare/app/standalone_main_window.h`
- Modify: `src/meshcompare/app/standalone_main_window.cpp`
- Modify: `src/meshcompare/app/workspace_controller.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: Workspace selection/reference state and Task 10 controller commands.
- Produces: `IColoringCommands`, tab modes `uniform`, `precision`, `normal`; manual Reference selection; `analysisProgress`; advanced parameter controls; Clear Coloring; score badges.

- [ ] **Step 1: Write failing panel behavior tests**

```cpp
void uniformModeTargetsOnlyTheSelectedMesh()
{
    WorkspaceState state = readyWorkspace({1, 2, 3}, 1);
    state.setSelectedMesh(2);
    FakeWorkspaceController controller;
    ColoringPanel panel(state, controller);
    panel.selectUniformModeForTest();
    panel.setColorForTest(QColor("#5aaa75"));
    panel.applyForTest();
    QCOMPARE(controller.uniformCalls(), QVector<MeshId>({2}));
}

void analysisModeUsesReferenceAndEveryOtherMesh()
{
    WorkspaceState state = readyWorkspace({1, 2, 3}, 1);
    FakeWorkspaceController controller;
    ColoringPanel panel(state, controller);
    panel.selectPrecisionModeForTest();
    panel.applyForTest();
    QCOMPARE(controller.lastAnalysisMetric(), SurfaceComparisonMetric::PrecisionAtThreshold);
    QCOMPARE(controller.lastAnalysisOptions().sampleCount, 500000);
    QCOMPARE(controller.lastAnalysisOptions().distanceThreshold, 0.004f);
}

void changingReferenceUsesAWorkspaceCommand()
{
    WorkspaceState state = readyWorkspace({1, 2, 3}, 1);
    FakeColoringCommands commands;
    ColoringPanel panel(state, commands);
    panel.setReferenceForTest(3);
    QCOMPARE(commands.referenceCalls(), QVector<MeshId>({3}));
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-coloring-panel-test -j 4
```

Expected: failure because `ColoringPanel` does not exist.

- [ ] **Step 3: Implement the transient panel**

Define a narrow panel-facing command contract and implement it in
`WorkspaceController`:

```cpp
class IColoringCommands {
public:
    virtual ~IColoringCommands() = default;
    virtual OperationResult setReference(MeshId meshId) = 0;
    virtual OperationResult setUniformColor(MeshId meshId, const QColor& color) = 0;
    virtual OperationResult startAnalysis(
        SurfaceComparisonMetric metric,
        const SurfaceComparisonOptions& options) = 0;
    virtual void cancelAnalysis() = 0;
    virtual void clearColoring() = 0;
};
```

Use a modeless `QFrame` anchored below `coloringButton`. Provide a three-tab
segmented control, a Reference selector, a selected-mesh label and color palette in
Uniform mode, and Reference/target summaries plus an Advanced Parameters expander in
analysis modes. `WorkspaceController::setReference()` calls
validates the target, identifies the currently analytical meshes, and first sends one
batched Default-presentation update for those meshes. If the renderer accepts it, the
controller calls `WorkspaceState::setReference()` and
`IRendererAdapter::setReferenceMesh()` so badges update without rebuilding the scene.
Uniform-color presentations remain unchanged; renderer failure leaves the old
Reference and state intact.

Defaults are exact:

```cpp
sampleCountSpin_->setRange(1, 5000000);
sampleCountSpin_->setValue(500000);
distanceThresholdSpin_->setDecimals(6);
distanceThresholdSpin_->setRange(0.0, 1000000.0);
distanceThresholdSpin_->setValue(0.004);
absoluteNormalDotCheck_->setChecked(true);
```

Disable the Apply button while `WorkspacePhase::Analyzing`; show Cancel instead.

- [ ] **Step 4: Connect progress and result badges**

Map `MeshId → progress percent` into each viewport overlay during analysis. On success replace progress with `P 0.942` or `N 0.917`. On failure clear progress and put the error in `statusLabel`; do not open one dialog per target.

- [ ] **Step 5: Run UI and analysis tests**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-coloring-panel-test meshcompare-mesh-color-service-test meshcompare -j 4
/opt/homebrew/bin/ctest --test-dir build \
  -R 'meshcompare-(coloring-panel|mesh-color-service)' --output-on-failure
```

Expected: panel routing, defaults, progress, and cancellation cases pass.

- [ ] **Step 6: Manually verify coloring**

Launch two triangle fixtures. Apply a uniform green to one mesh, clear it, run Precision, then run Normal Agreement. Expected: uniform color affects one viewport only; analysis affects every non-Reference viewport; score badges appear; Clear Coloring restores defaults.

- [ ] **Step 7: Commit**

```bash
git add src/meshcompare/app/coloring_panel.* \
  src/meshcompare/tests/coloring_panel_test.cpp \
  src/meshcompare/app/standalone_main_window.* \
  src/meshcompare/app/workspace_controller.cpp src/meshcompare/CMakeLists.txt
git commit -m "feat: add mesh coloring workflow UI"
```

---

### Task 12: Extract the Camera Pose Store with Migration and Deletion

**Files:**
- Create: `src/meshcompare/services/camera_pose_store.h`
- Create: `src/meshcompare/services/camera_pose_store.cpp`
- Create: `src/meshcompare/tests/camera_pose_store_test.cpp`
- Modify: `src/meshlab/camera_pose_library.h`
- Modify: `src/meshlab/camera_pose_library.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: existing schema-2 JSON and MeshLab view-state XML.
- Produces: `SavedCameraPose`, `ICameraPoseStore`, `CameraPoseStore::migrateLegacyIfNeeded()`, `save()`, `list()`, `load()`, and `remove()`; legacy `CameraPoseLibrary` delegates to the shared implementation.

- [ ] **Step 1: Write failing temporary-directory tests**

```cpp
void migratesLegacyLibraryOnlyWhenNewLibraryIsAbsent()
{
    QTemporaryDir dir;
    const QString legacy = dir.filePath("legacy.json");
    const QString current = dir.filePath("current/poses.json");
    writeSchema2Library(legacy, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "view_001");
    CameraPoseStore store(current, legacy);
    QVERIFY(store.migrateLegacyIfNeeded().ok);
    QVERIFY(QFileInfo::exists(current));
    QVERIFY(QFileInfo::exists(legacy));
    QCOMPARE(store.list(uuidA()).size(), 1);
}

void deletionIsAtomicAndScopedToUuid()
{
    CameraPoseStore store(currentPath_, QString());
    store.save(uuidA(), pose("a"));
    store.save(uuidB(), pose("b"));
    QVERIFY(store.remove(uuidA(), "view_001").ok);
    QCOMPARE(store.list(uuidA()).size(), 0);
    QCOMPARE(store.list(uuidB()).size(), 1);
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-camera-pose-store-test -j 4
```

Expected: failure because `CameraPoseStore` is undefined.

- [ ] **Step 3: Implement explicit-path storage**

```cpp
struct SavedCameraPose {
    QString uuid;
    QString viewId;
    QString savedAtUtc;
    CameraPose pose;
};

class ICameraPoseStore {
public:
    virtual ~ICameraPoseStore() = default;
    virtual OperationResult migrateLegacyIfNeeded() = 0;
    virtual OperationResult save(
        const QString& uuid, const CameraPose& pose, QString* viewId = nullptr) = 0;
    virtual QVector<SavedCameraPose> list(
        const QString& uuid, OperationResult* result = nullptr) const = 0;
    virtual OperationResult load(
        const QString& uuid, const QString& viewId, CameraPose* pose) const = 0;
    virtual OperationResult remove(const QString& uuid, const QString& viewId) = 0;
};

class CameraPoseStore final : public ICameraPoseStore {
public:
    CameraPoseStore(QString storagePath, QString legacyPath);
    OperationResult migrateLegacyIfNeeded() override;
    OperationResult save(
        const QString& uuid, const CameraPose& pose, QString* viewId = nullptr) override;
    QVector<SavedCameraPose> list(
        const QString& uuid, OperationResult* result = nullptr) const override;
    OperationResult load(
        const QString& uuid, const QString& viewId, CameraPose* pose) const override;
    OperationResult remove(const QString& uuid, const QString& viewId) override;
    static QString normalizeUuid(const QString& value);
};
```

All writes use `QSaveFile`. `remove()` deletes the UUID property when its final pose is removed. A corrupt JSON document returns a failure and is never overwritten.

- [ ] **Step 4: Implement first-launch migration**

If the new path does not exist and the legacy path does, create the destination directory and use `QFile::copy()`. Never rename, delete, or rewrite the legacy file.

Production paths are composed in `main.cpp` after setting the new application name. The legacy path is the existing MeshLab AppLocalData directory plus `meshlab_lizd_camera_poses.json`.

- [ ] **Step 5: Keep the existing MeshLab target compatible**

Change `CameraPoseLibrary` into a small adapter that preserves its current static signatures while delegating JSON parsing and persistence to `CameraPoseStore`. Add a static path factory for the legacy application so existing behavior remains unchanged.

- [ ] **Step 6: Run store and legacy build tests**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-camera-pose-store-test meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-camera-pose-store --output-on-failure
```

Expected: migration, schema validation, save/list/load/delete, and legacy compatibility pass.

- [ ] **Step 7: Commit**

```bash
git add src/meshcompare/services/camera_pose_store.* \
  src/meshcompare/tests/camera_pose_store_test.cpp \
  src/meshlab/camera_pose_library.* src/meshcompare/CMakeLists.txt
git commit -m "feat: add standalone camera pose store"
```

---

### Task 13: Add Camera Auto-Restore and the Camera Panel

**Files:**
- Create: `src/meshcompare/app/camera_panel.h`
- Create: `src/meshcompare/app/camera_panel.cpp`
- Create: `src/meshcompare/tests/camera_workflow_test.cpp`
- Modify: `src/meshcompare/app/workspace_controller.h`
- Modify: `src/meshcompare/app/workspace_controller.cpp`
- Modify: `src/meshcompare/app/standalone_main_window.h`
- Modify: `src/meshcompare/app/standalone_main_window.cpp`
- Modify: `src/meshcompare/main.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`

**Interfaces:**
- Consumes: `ICameraPoseStore`, renderer camera capture/restore, and per-mesh UUID candidates.
- Produces: deterministic `resolveWorkspaceUuid()`, import-time latest-pose restore, and camera panel save/apply/delete actions.

- [ ] **Step 1: Write failing UUID-priority and restore tests**

```cpp
void referenceUuidWinsOverEarlierNonReferenceUuid()
{
    QVector<MeshEntry> meshes = {
        meshWithUuid(1, uuidA(), false),
        meshWithUuid(2, uuidB(), true)
    };
    QCOMPARE(resolveWorkspaceUuid(meshes, 2), uuidB());
}

void successfulImportRestoresLatestPoseAfterRendererCommit()
{
    FakeCameraPoseStore store;
    store.add(uuidA(), "view_001", pose("old"), "2026-07-14T01:00:00Z");
    store.add(uuidA(), "view_002", pose("latest"), "2026-07-14T02:00:00Z");
    FakeRendererAdapter renderer;
    WorkspaceController controller(state_, importer_, renderer, colorService_, store);
    QVERIFY(controller.importMeshes(paths_).ok);
    QCOMPARE(renderer.restoredPose(), pose("latest"));
    QCOMPARE(renderer.events(), QStringList({"prepare", "commit", "restore:latest"}));
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target meshcompare-camera-workflow-test -j 4
```

Expected: failure because the UUID resolver and camera panel workflow do not exist.

- [ ] **Step 3: Implement deterministic UUID and latest-pose selection**

Check the Reference entry first, then remaining entries in import order. Within each entry, use the first normalized UUID candidate. Sort saved poses by `savedAtUtc`, breaking ties by `viewId`, and restore the last item.

If no UUID exists, return a successful import with camera saving disabled and status text “No UUID was found in this workspace.” Restore errors are non-fatal and go to the status area.

- [ ] **Step 4: Implement the camera panel**

The transient panel contains a Save Current Pose button, a list scoped to the resolved workspace UUID, Apply, and Delete. Emit only semantic controller calls:

```cpp
controller_.saveCurrentCameraPose();
controller_.applyCameraPose(selectedViewId());
controller_.deleteCameraPose(selectedViewId());
```

Refresh the list after save or delete. Disable all write actions when no UUID is available.

- [ ] **Step 5: Run camera workflow and renderer tests**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-camera-workflow-test meshcompare-renderer-smoke-test meshcompare -j 4
/opt/homebrew/bin/ctest --test-dir build \
  -R 'meshcompare-(camera-workflow|camera-pose-store|renderer-smoke)' \
  --output-on-failure
```

Expected: UUID priority, latest restore, save/apply/delete, invalid XML, and non-fatal restore cases pass.

- [ ] **Step 6: Manually verify compatibility with the existing pose list**

Launch the old app once to confirm its legacy library exists, then launch Mesh Compare. Expected: the new library is copied once, the current UUID's saved views appear, applying a view updates every viewport, and the legacy file timestamp does not change.

- [ ] **Step 7: Commit**

```bash
git add src/meshcompare/app/camera_panel.* \
  src/meshcompare/tests/camera_workflow_test.cpp \
  src/meshcompare/app/workspace_controller.* \
  src/meshcompare/app/standalone_main_window.* \
  src/meshcompare/main.cpp src/meshcompare/CMakeLists.txt
git commit -m "feat: add UUID camera pose workflow"
```

---

### Task 14: Add Diagnostics, Packaging, and Final Acceptance Gates

**Files:**
- Create: `src/meshcompare/app/diagnostics_menu.h`
- Create: `src/meshcompare/app/diagnostics_menu.cpp`
- Create: `src/meshcompare/services/diagnostics_log.h`
- Create: `src/meshcompare/services/diagnostics_log.cpp`
- Create: `src/meshcompare/tests/diagnostics_menu_test.cpp`
- Create: `src/meshcompare/tests/workspace_lifecycle_test.cpp`
- Create: `src/meshcompare/tests/render_regression_test.cpp`
- Create: `src/meshcompare/tests/fixtures/render_reference.png`
- Create: `src/meshcompare/README.md`
- Modify: `src/meshcompare/main.cpp`
- Modify: `src/meshcompare/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: `IRendererAdapter::resetCamera()`, `setDiagnostic()`, and `diagnostics()` plus application status.
- Produces: the exact seven-item diagnostics menu, local JSONL diagnostics, macOS bundle metadata, lifecycle stress test, and render regression gate.

- [ ] **Step 1: Write failing diagnostics-menu and lifecycle tests**

```cpp
void menuContainsOnlyApprovedActions()
{
    FakeRendererAdapter renderer;
    DiagnosticsMenu menu(renderer, diagnostics_);
    QCOMPARE(menu.actionTexts(), QStringList({
        "Reset Camera", "Orthographic", "Wireframe Overlay", "Show Normals",
        "Copy Diagnostics", "Open Local Log", "About"
    }));
}

void repeatedReplaceAndShutdownUsesSafeOrder()
{
    LifecycleRecorder recorder;
    for (int i = 0; i < 25; ++i)
        runImportReplaceCycle(recorder, i);
    shutdown(recorder);
    QVERIFY(recorder.everyCycleMatches({
        "analysis:join", "callbacks:disconnect", "viewports:destroy",
        "gpu:release", "repository:release"
    }));
}
```

- [ ] **Step 2: Build and verify failure**

```bash
/opt/homebrew/bin/cmake --build build --target \
  meshcompare-diagnostics-menu-test meshcompare-workspace-lifecycle-test -j 4
```

Expected: failure because diagnostics and the lifecycle harness do not exist.

- [ ] **Step 3: Implement the exact diagnostics menu**

Wire actions as follows:

```cpp
connect(resetCamera_, &QAction::triggered, [&] { renderer_.resetCamera(); });
connect(orthographic_, &QAction::toggled, [&](bool on) {
    renderer_.setDiagnostic(DiagnosticFlag::Orthographic, on);
});
connect(wireframe_, &QAction::toggled, [&](bool on) {
    renderer_.setDiagnostic(DiagnosticFlag::Wireframe, on);
});
connect(normals_, &QAction::toggled, [&](bool on) {
    renderer_.setDiagnostic(DiagnosticFlag::Normals, on);
});
```

Copy Diagnostics includes app version, Qt version, OS, renderer backend name, OpenGL vendor/renderer/version, workspace mesh counts, and the most recent error. It must not include source paths; replace them with display names.

- [ ] **Step 4: Implement local diagnostics logging**

Write JSONL under the new AppLocalData directory, rotate at 2 MiB, and record session start/end, batch import outcome, analysis metric/outcome, camera save/apply, and fatal renderer errors. Keep the existing path-redaction rule: names containing `/` or `\\` become `path_like_name_omitted`.

- [ ] **Step 5: Configure the independent macOS bundle**

Set exact target properties:

```cmake
set_target_properties(meshcompare PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_BUNDLE_NAME "Mesh Compare"
  MACOSX_BUNDLE_GUI_IDENTIFIER "org.vcg.meshcompare"
  MACOSX_BUNDLE_BUNDLE_VERSION "${MESHLAB_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${MESHLAB_VERSION}"
  MACOSX_BUNDLE_INFO_STRING "Mesh Compare ${MESHLAB_VERSION}")
```

Install/copy only IO plugins required by the current import pipeline into `meshcompare.app/Contents/PlugIns`. Do not copy filter, edit, render, or decorator plugins. Document the build, test, launch, application-data, camera migration, and log paths in `src/meshcompare/README.md`.

- [ ] **Step 6: Add the render regression test**

Render the fixed triangle fixture at a fixed 1000×600 viewport and fixed camera. Compare against `render_reference.png` after masking the viewport label and allowing per-channel difference ≤ 3 for at least 99.5% of pixels:

```cpp
const ImageDiff diff = compareImages(actual, reference, 3);
QVERIFY2(diff.equalPixelRatio >= 0.995, qPrintable(diff.summary()));
```

Generate the initial golden only through an explicit update mode, inspect it against
the approved MeshLab lighting/background reference, then commit it. Normal CI runs
must never update the file:

```bash
MESHCOMPARE_UPDATE_GOLDEN=1 build/src/meshcompare/meshcompare-render-regression-test
open src/meshcompare/tests/fixtures/render_reference.png
/opt/homebrew/bin/ctest --test-dir build -R meshcompare-render-regression --output-on-failure
```

The test writes the fixture only when `MESHCOMPARE_UPDATE_GOLDEN=1`; otherwise a
missing reference is a hard failure.

- [ ] **Step 7: Run the complete automated suite**

```bash
/opt/homebrew/bin/cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
/opt/homebrew/bin/cmake --build build --target meshcompare meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build -R '^meshcompare-' --output-on-failure
```

Expected: all Mesh Compare tests pass and the existing MeshLab executable links.

- [ ] **Step 8: Run static architecture gates**

```bash
! rg -n 'mainwindow\.h|\bMainWindow\b|MultiViewer_Container|LayerDialog|FilterDock' \
  src/meshcompare --glob '!tests/**'
! rg -n 'mw\(|QApplication::activeWindow\(' \
  src/meshcompare/renderer/meshlab
```

Expected: both commands exit successfully with no matches.

- [ ] **Step 9: Run manual acceptance against the approved spec**

Verify in the built app:

1. Import 2, 3, 4, 5, 6, 7, and 8 meshes and confirm the specified grids.
2. Confirm `gt` selection, fallback notice, multiple-`gt` notice, and manual Reference change.
3. Rotate, zoom, and pan each viewport and compare direction/speed with the current MeshLab app.
4. Apply/clear a uniform color; run/cancel Precision and Normal Agreement.
5. Save, list, apply, delete, and auto-restore a UUID camera pose.
6. Replace a non-empty workspace and cancel once, then confirm once.
7. Toggle all three diagnostics and copy/open diagnostics.
8. Repeat replacement ten times and quit during an idle workspace; no crash or stale-context warning is allowed.

- [ ] **Step 10: Commit**

```bash
git add src/meshcompare/app/diagnostics_menu.* \
  src/meshcompare/services/diagnostics_log.* \
  src/meshcompare/tests/diagnostics_menu_test.cpp \
  src/meshcompare/tests/workspace_lifecycle_test.cpp \
  src/meshcompare/tests/render_regression_test.cpp \
  src/meshcompare/tests/fixtures/render_reference.png \
  src/meshcompare/README.md src/meshcompare/main.cpp \
  src/meshcompare/CMakeLists.txt src/CMakeLists.txt
git commit -m "feat: finish standalone mesh compare application"
```

---

## Final Verification

After Task 14, run the full final gate from a clean incremental build:

```bash
/opt/homebrew/bin/cmake --build build --target clean
/opt/homebrew/bin/cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
/opt/homebrew/bin/cmake --build build --target meshcompare meshlab -j 4
/opt/homebrew/bin/ctest --test-dir build -R '^meshcompare-' --output-on-failure
git status --short
```

Expected:

- `meshcompare.app` and `meshlab.app` both build.
- Every `meshcompare-*` test passes.
- Static architecture checks report no forbidden legacy dependency.
- `git status --short` contains only pre-existing user changes and no generated build artifacts.
- The implementation commit series contains one focused commit per task.
