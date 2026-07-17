#include "app/application_startup.h"
#include "app/camera_pose_paths.h"
#include "app/diagnostics_menu.h"
#include "app/standalone_main_window.h"
#include "app/workspace_controller.h"
#include "core/workspace_state.h"
#include "infrastructure/meshlab/meshlab_mesh_loader.h"
#include "renderer/meshlab/mesh_lab_renderer_adapter.h"
#include "services/camera_pose_store.h"
#include "services/diagnostics_log.h"
#include "services/mesh_import_service.h"
#include "services/surface_comparison.h"

#include <common/globals.h>
#include <common/mlexception.h>
#include <common/mlapplication.h>
#include <common/plugins/plugin_manager.h>

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QFileInfo>
#include <QLocale>
#include <QObject>
#include <QStandardPaths>

#include <clocale>

namespace
{
void appendNotice(QString& destination, const QString& notice)
{
    if (notice.isEmpty())
        return;
    if (!destination.isEmpty())
        destination.append(QLatin1Char('\n'));
    destination.append(notice);
}

QString analysisMetricName(SurfaceComparisonMetric metric)
{
    switch (metric) {
    case SurfaceComparisonMetric::PrecisionAtThreshold:
        return QStringLiteral("precision");
    case SurfaceComparisonMetric::NormalAgreement:
        return QStringLiteral("normal_agreement");
    case SurfaceComparisonMetric::DistanceToReference:
        return QStringLiteral("distance");
    case SurfaceComparisonMetric::DoubleLayer:
        return QStringLiteral("double_layer");
    }
    return QStringLiteral("unknown");
}
} // namespace

int main(int argc, char** argv)
{
    MeshLabApplication app(argc, argv);
    FileOpenEventBridge fileOpenBridge(app);
    QCoreApplication::setOrganizationName("VCG");
    QCoreApplication::setApplicationName("MeshCompare");
    QCoreApplication::setApplicationVersion(
        QStringLiteral(MESHCOMPARE_VERSION));
    QGuiApplication::setApplicationDisplayName("Mesh Compare");
    std::setlocale(LC_ALL, "C");
    QLocale::setDefault(QLocale::C);

    const QString applicationDataRoot = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    DiagnosticsLog diagnosticsLog(applicationDataRoot);
    const OperationResult sessionLogStarted =
        diagnosticsLog.recordSessionStart();

    try {
        meshlab::pluginManagerInstance().loadPlugins();

        WorkspaceState state;
        MeshLabMeshLoader loader;
        MeshImportService importer(loader);
        MeshLabRendererAdapter renderer;
        SurfaceComparer comparer;
        const CameraPosePaths cameraPaths = resolveCameraPosePaths(
            applicationDataRoot,
            QStandardPaths::writableLocation(
                QStandardPaths::GenericDataLocation));
        CameraPoseStore cameraStore(
            cameraPaths.storagePath, cameraPaths.legacyPath);
        const OperationResult cameraMigration =
            cameraStore.migrateLegacyIfNeeded();
        QString pendingStartupNotice;
        if (!sessionLogStarted.ok) {
            appendNotice(
                pendingStartupNotice,
                QStringLiteral("Diagnostics logging is unavailable: %1")
                    .arg(sessionLogStarted.error));
        }
        if (!cameraMigration.ok) {
            appendNotice(
                pendingStartupNotice,
                QStringLiteral("Camera pose migration was skipped: %1")
                    .arg(cameraMigration.error));
        }
        QStringList startupMeshes;
        for (int index = 1; index < argc; ++index)
            startupMeshes.append(QString::fromLocal8Bit(argv[index]));
        // Construct the viewport owner first so the controller can join analysis,
        // disconnect callbacks, and clear the renderer while the host still lives.
        StandaloneMainWindow window(state);
        WorkspaceController controller(
            state, importer, renderer, comparer, cameraStore);
        DiagnosticsMenu diagnosticsMenu(
            renderer, state, diagnosticsLog, &window);
        window.setDiagnosticsMenu(&diagnosticsMenu);
        if (!sessionLogStarted.ok) {
            diagnosticsMenu.setLoggingError(sessionLogStarted.error);
            qWarning().noquote()
                << "Diagnostics logging failure:" << sessionLogStarted.error;
        }
        const auto reportLoggingFailure =
            [&window, &diagnosticsMenu](const OperationResult& result) {
                if (result.ok)
                    return;
                const QString message =
                    QStringLiteral("Diagnostics logging is unavailable: %1")
                        .arg(result.error);
                diagnosticsMenu.setLoggingError(result.error);
                qWarning().noquote()
                    << "Diagnostics logging failure:" << result.error;
                window.showStatusMessage(message);
            };
        if (!pendingStartupNotice.isEmpty())
            diagnosticsMenu.setRecentError(pendingStartupNotice);
        const auto importAndPresent =
            [&controller,
             &window,
             &state,
             &diagnosticsMenu,
             &diagnosticsLog,
             &reportLoggingFailure,
             &pendingStartupNotice](
                const QStringList& paths) {
                WorkspaceImportOutcome outcome = controller.importMeshes(paths);
                mergeStartupNotice(outcome, pendingStartupNotice);
                pendingStartupNotice.clear();
                QStringList displayNames;
                if (outcome.result.ok) {
                    displayNames.reserve(state.meshes().size());
                    for (const MeshEntry& mesh : state.meshes())
                        displayNames.append(mesh.displayName);
                }
                else {
                    displayNames.reserve(paths.size());
                    for (const QString& path : paths)
                        displayNames.append(QFileInfo(path).fileName());
                    diagnosticsMenu.setRecentError(outcome.result.error);
                }
                reportLoggingFailure(
                    diagnosticsLog.recordImportOutcome(
                        outcome.result.ok,
                        outcome.result.ok ? state.meshes().size() : 0,
                        displayNames,
                        outcome.result.error));
                window.presentImportOutcome(outcome);
            };
        QObject::connect(
            &window,
            &StandaloneMainWindow::importRequested,
            &window,
            importAndPresent);
        QObject::connect(
            &controller,
            &WorkspaceController::statusMessage,
            &window,
            [&window, &diagnosticsMenu](const QString& message) {
                diagnosticsMenu.setRecentError(message);
                window.showStatusMessage(message);
            });
        QObject::connect(
            &window,
            &StandaloneMainWindow::operationFailed,
            &window,
            [&diagnosticsMenu](const QString& message) {
                diagnosticsMenu.setRecentError(message);
            });
        QObject::connect(
            &controller,
            &WorkspaceController::workspaceChanged,
            &window,
            &StandaloneMainWindow::refreshWorkspace);
        QObject::connect(
            &controller,
            &WorkspaceController::analysisProgress,
            &window,
            &StandaloneMainWindow::presentAnalysisProgress);
        QObject::connect(
            &controller,
            &WorkspaceController::analysisFinished,
            &window,
            &StandaloneMainWindow::presentAnalysisFinished);
        QObject::connect(
            &controller,
            &WorkspaceController::analysisFinished,
            &window,
            [&diagnosticsLog, &diagnosticsMenu, &reportLoggingFailure](
                const AnalysisBatchResult& result) {
                if (!result.result.ok)
                    diagnosticsMenu.setRecentError(result.result.error);
                reportLoggingFailure(
                    diagnosticsLog.recordAnalysisOutcome(
                        analysisMetricName(result.metric),
                        result.result.ok,
                        result.result.error));
            });
        QObject::connect(
            &controller,
            &WorkspaceController::analysisStartFailed,
            &window,
            [&diagnosticsLog, &diagnosticsMenu, &reportLoggingFailure](
                SurfaceComparisonMetric metric,
                const QString& error) {
                diagnosticsMenu.setRecentError(error);
                reportLoggingFailure(
                    diagnosticsLog.recordAnalysisOutcome(
                        analysisMetricName(metric), false, error));
            });
        QObject::connect(
            &controller,
            &WorkspaceController::cameraSaveFinished,
            &window,
            [&diagnosticsLog, &diagnosticsMenu, &reportLoggingFailure](
                bool succeeded,
                const QString& error) {
                if (!succeeded)
                    diagnosticsMenu.setRecentError(error);
                reportLoggingFailure(
                    diagnosticsLog.recordCameraSave(succeeded, error));
            });
        QObject::connect(
            &controller,
            &WorkspaceController::cameraApplyFinished,
            &window,
            [&diagnosticsLog, &diagnosticsMenu, &reportLoggingFailure](
                bool succeeded,
                const QString& error) {
                if (!succeeded)
                    diagnosticsMenu.setRecentError(error);
                reportLoggingFailure(
                    diagnosticsLog.recordCameraApply(succeeded, error));
            });

        const OperationResult mounted =
            initializeRenderer(state, renderer, window.viewportHost());
        if (!mounted.ok) {
            OperationResult displayedFailure = mounted;
            const OperationResult logged =
                diagnosticsLog.recordFatalRendererError(mounted.error);
            if (!logged.ok) {
                appendNotice(
                    displayedFailure.error,
                    QStringLiteral("Diagnostics logging is unavailable: %1")
                        .arg(logged.error));
            }
            const int exitCode = presentFatalStartupError(displayedFailure);
            const OperationResult ended = diagnosticsLog.recordSessionEnd();
            if (!ended.ok)
                qWarning() << ended.error;
            return exitCode;
        }
        window.bindColoringCommands(controller);
        window.bindCameraCommands(controller);

        window.resize(1440, 900);
        window.show();

        if (startupMeshes.isEmpty() && !pendingStartupNotice.isEmpty()) {
            window.showStatusMessage(pendingStartupNotice);
            pendingStartupNotice.clear();
        }
        fileOpenBridge.setOpenHandler(
            [&window](const QStringList& paths) {
                window.requestImport(paths);
            });
        fileOpenBridge.queueOpenPaths(startupMeshes);
        const int exitCode = app.exec();
        fileOpenBridge.clearOpenHandler();
        window.setDiagnosticsMenu(nullptr);
        window.unbindCameraCommands();
        window.unbindColoringCommands();
        const OperationResult ended = diagnosticsLog.recordSessionEnd();
        if (!ended.ok)
            qWarning() << ended.error;
        return exitCode;
    }
    catch (const MLException& exception) {
        const OperationResult failure = OperationResult::failure(
            QString::fromLocal8Bit(exception.what()));
        OperationResult displayedFailure = failure;
        const OperationResult logged =
            diagnosticsLog.recordFatalRendererError(failure.error);
        if (!logged.ok) {
            appendNotice(
                displayedFailure.error,
                QStringLiteral("Diagnostics logging is unavailable: %1")
                    .arg(logged.error));
        }
        const int exitCode = presentFatalStartupError(displayedFailure);
        const OperationResult ended = diagnosticsLog.recordSessionEnd();
        if (!ended.ok)
            qWarning() << ended.error;
        return exitCode;
    }
}
