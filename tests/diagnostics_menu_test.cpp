#include <QtTest>

#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>

#include "app/diagnostics_menu.h"
#include "core/renderer_adapter.h"
#include "core/workspace_state.h"
#include "services/diagnostics_log.h"

namespace
{
class RecordingRenderer final : public IRendererAdapter
{
public:
    OperationResult mount(QWidget*) override { return OperationResult::success(); }
    void setEvents(RendererEvents) override {}
    OperationResult prepareScene(
        const SceneDescriptor&,
        const IMeshResourceProvider&) override
    {
        return OperationResult::success();
    }
    void commitPreparedScene() override {}
    void discardPreparedScene() override {}
    void clearScene() override {}
    void setSelectedMesh(MeshId) override {}
    void setReferenceMesh(MeshId) override {}
    OperationResult setMeshVisible(MeshId, bool) override
    {
        return OperationResult::success();
    }
    OperationResult setColorPresentations(
        const QVector<MeshColorPresentationUpdate>&) override
    {
        return OperationResult::success();
    }
    OperationResult setAnalysisOverlays(
        const QVector<MeshAnalysisOverlayUpdate>&) override
    {
        return OperationResult::success();
    }
    CameraPose captureCamera() const override { return {}; }
    OperationResult captureImage(QImage& image) override
    {
        Q_UNUSED(image);
        return OperationResult::failure(
            QStringLiteral("Diagnostics renderer image capture is unavailable."));
    }
    OperationResult restoreCamera(const CameraPose&) override
    {
        return OperationResult::success();
    }
    void resetCamera() override { ++resetCount; }
    void setDiagnostic(DiagnosticFlag flag, bool enabled) override
    {
        diagnosticCalls.append(qMakePair(flag, enabled));
    }
    RendererDiagnostics diagnostics() const override { return diagnosticReport; }

    int resetCount = 0;
    QVector<QPair<DiagnosticFlag, bool>> diagnosticCalls;
    RendererDiagnostics diagnosticReport;
};

QAction* actionWithText(DiagnosticsMenu& menu, const QString& text)
{
    for (QAction* action : menu.actions()) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

MeshEntry mesh(
    MeshId id,
    const QString& sourcePath,
    const QString& displayName)
{
    MeshEntry result;
    result.id = id;
    result.resourceId = id + 100;
    result.sourcePath = sourcePath;
    result.displayName = displayName;
    return result;
}

QVector<QJsonObject> readJsonLines(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return {};

    QVector<QJsonObject> result;
    const QList<QByteArray> lines = input.readAll().split('\n');
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject())
            return {};
        result.append(document.object());
    }
    return result;
}
} // namespace

class DiagnosticsMenuTest : public QObject
{
    Q_OBJECT

private slots:
    void menuContainsTheApprovedEnglishActions()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        WorkspaceState state;
        RecordingRenderer renderer;
        DiagnosticsLog log(root.path());

        DiagnosticsMenu menu(renderer, state, log);

        QCOMPARE(
            menu.actionTexts(),
            QStringList({
                QStringLiteral("Reset Camera"),
                QStringLiteral("Orthographic"),
                QStringLiteral("Wireframe Overlay"),
                QStringLiteral("Double-Sided Rendering"),
                QStringLiteral("Show Normals"),
                QStringLiteral("Copy Diagnostics"),
                QStringLiteral("Open Local Log"),
                QStringLiteral("About")}));
        QCOMPARE(menu.actions().size(), 8);
        QVERIFY(!menu.actions().at(0)->isCheckable());
        QVERIFY(menu.actions().at(1)->isCheckable());
        QVERIFY(menu.actions().at(2)->isCheckable());
        QVERIFY(menu.actions().at(3)->isCheckable());
        QVERIFY(menu.actions().at(4)->isCheckable());
        QVERIFY(!menu.actions().at(5)->isCheckable());
        QVERIFY(!menu.actions().at(6)->isCheckable());
        QVERIFY(!menu.actions().at(7)->isCheckable());
    }

    void rendererCommandsAreReachedOnlyThroughTheAdapter()
    {
        QTemporaryDir root;
        WorkspaceState state;
        RecordingRenderer renderer;
        DiagnosticsLog log(root.path());
        DiagnosticsMenu menu(renderer, state, log);

        actionWithText(menu, QStringLiteral("Reset Camera"))->trigger();
        actionWithText(menu, QStringLiteral("Orthographic"))->setChecked(true);
        actionWithText(menu, QStringLiteral("Wireframe Overlay"))->setChecked(true);
        actionWithText(menu, QStringLiteral("Double-Sided Rendering"))
            ->setChecked(true);
        actionWithText(menu, QStringLiteral("Show Normals"))->setChecked(true);

        QCOMPARE(renderer.resetCount, 1);
        QCOMPARE(renderer.diagnosticCalls.size(), 4);
        QCOMPARE(
            renderer.diagnosticCalls.at(0).first,
            DiagnosticFlag::Orthographic);
        QVERIFY(renderer.diagnosticCalls.at(0).second);
        QCOMPARE(
            renderer.diagnosticCalls.at(1).first,
            DiagnosticFlag::Wireframe);
        QVERIFY(renderer.diagnosticCalls.at(1).second);
        QCOMPARE(
            renderer.diagnosticCalls.at(2).first,
            DiagnosticFlag::DoubleSided);
        QVERIFY(renderer.diagnosticCalls.at(2).second);
        QCOMPARE(
            renderer.diagnosticCalls.at(3).first,
            DiagnosticFlag::Normals);
        QVERIFY(renderer.diagnosticCalls.at(3).second);
    }

    void copyOpenAndAboutUseInjectedPlatformServices()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        WorkspaceState state;
        QVERIFY(state.commitWorkspace(
                         {mesh(
                              1,
                              QStringLiteral("/private/secret/reference.obj"),
                              QStringLiteral("reference.obj")),
                          mesh(
                              2,
                              QStringLiteral("C:\\secret\\candidate.ply"),
                              QStringLiteral("candidate.ply"))},
                         1)
                    .ok);
        RecordingRenderer renderer;
        renderer.diagnosticReport = {
            QStringLiteral("MeshLab standalone renderer"),
            QStringLiteral("Example GPU Vendor"),
            QStringLiteral("Example GPU Renderer"),
            QStringLiteral("4.1 Test")};
        DiagnosticsLog log(root.path());
        QString copiedText;
        QUrl openedUrl;
        int aboutCount = 0;
        DiagnosticsMenuServices services;
        services.copyText =
            [&copiedText](const QString& text) { copiedText = text; };
        services.openUrl = [&openedUrl](const QUrl& url) {
            openedUrl = url;
            return true;
        };
        services.showAbout = [&aboutCount] { ++aboutCount; };
        DiagnosticsMenu menu(renderer, state, log, nullptr, services);
        menu.setRecentError(QStringLiteral("GPU upload failed"));

        actionWithText(menu, QStringLiteral("Copy Diagnostics"))->trigger();
        actionWithText(menu, QStringLiteral("Open Local Log"))->trigger();
        actionWithText(menu, QStringLiteral("About"))->trigger();

        QCOMPARE(copiedText, menu.diagnosticsText());
        QVERIFY(copiedText.contains(QStringLiteral("Application version:")));
        QVERIFY(copiedText.contains(QStringLiteral("Qt version:")));
        QVERIFY(copiedText.contains(QStringLiteral("Operating system:")));
        QVERIFY(copiedText.contains(
            QStringLiteral("Renderer backend: MeshLab standalone renderer")));
        QVERIFY(copiedText.contains(
            QStringLiteral("OpenGL vendor: Example GPU Vendor")));
        QVERIFY(copiedText.contains(
            QStringLiteral("OpenGL renderer: Example GPU Renderer")));
        QVERIFY(copiedText.contains(QStringLiteral("OpenGL version: 4.1 Test")));
        QVERIFY(copiedText.contains(QStringLiteral("Workspace mesh count: 2")));
        QVERIFY(copiedText.contains(QStringLiteral("reference.obj")));
        QVERIFY(copiedText.contains(QStringLiteral("candidate.ply")));
        QVERIFY(copiedText.contains(
            QStringLiteral("Most recent error: GPU upload failed")));
        QVERIFY(!copiedText.contains(QStringLiteral("/private/secret")));
        QVERIFY(!copiedText.contains(QStringLiteral("C:\\secret")));

        menu.setRecentError(QStringLiteral(
            "Unable to read /private/secret/reference.obj."));
        const QString redactedErrorDiagnostics = menu.diagnosticsText();
        QVERIFY(redactedErrorDiagnostics.contains(
            QStringLiteral("Unable to read reference.obj.")));
        QVERIFY(!redactedErrorDiagnostics.contains(
            QStringLiteral("/private/secret")));

        menu.setLoggingError(QStringLiteral(
            "Unable to write /private/secret/reference.obj."));
        menu.setRecentError(QStringLiteral("A later operation failed"));
        const QString persistentLoggingDiagnostics = menu.diagnosticsText();
        QVERIFY(persistentLoggingDiagnostics.contains(
            QStringLiteral("Diagnostics logging: Unable to write reference.obj.")));
        QVERIFY(persistentLoggingDiagnostics.contains(
            QStringLiteral("Most recent error: A later operation failed")));
        QVERIFY(!persistentLoggingDiagnostics.contains(
            QStringLiteral("/private/secret")));
        QVERIFY(openedUrl.isLocalFile());
        QCOMPARE(openedUrl.toLocalFile(), log.filePath());
        QCOMPARE(aboutCount, 1);
    }

    void jsonLinesRecordEveryRequiredEventAndRedactPathLikeDisplayNames()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DiagnosticsLog log(root.path());

        QVERIFY(log.recordSessionStart().ok);
        QVERIFY(log.recordImportOutcome(
                       true,
                       3,
                       {QStringLiteral("safe.obj"),
                        QStringLiteral("/secret/path.obj"),
                        QStringLiteral("folder\\private.ply")})
                    .ok);
        QVERIFY(log.recordAnalysisOutcome(
                       QStringLiteral("precision"),
                       false,
                       QStringLiteral("analysis failed"))
                    .ok);
        QVERIFY(log.recordCameraSave(true).ok);
        QVERIFY(log.recordCameraApply(false, QStringLiteral("pose is corrupt")).ok);
        QVERIFY(log.recordFatalRendererError(QStringLiteral("context lost")).ok);
        QVERIFY(log.recordSessionEnd().ok);

        QCOMPARE(
            log.filePath(),
            root.filePath(QStringLiteral("meshcompare-diagnostics.jsonl")));
        const QVector<QJsonObject> lines = readJsonLines(log.filePath());
        QCOMPARE(lines.size(), 7);
        QCOMPARE(lines.at(0).value(QStringLiteral("event")).toString(),
                 QStringLiteral("session_start"));
        QCOMPARE(lines.at(1).value(QStringLiteral("event")).toString(),
                 QStringLiteral("import_outcome"));
        QCOMPARE(lines.at(2).value(QStringLiteral("event")).toString(),
                 QStringLiteral("analysis_outcome"));
        QCOMPARE(lines.at(3).value(QStringLiteral("event")).toString(),
                 QStringLiteral("camera_save"));
        QCOMPARE(lines.at(4).value(QStringLiteral("event")).toString(),
                 QStringLiteral("camera_apply"));
        QCOMPARE(lines.at(5).value(QStringLiteral("event")).toString(),
                 QStringLiteral("fatal_renderer_error"));
        QCOMPARE(lines.at(6).value(QStringLiteral("event")).toString(),
                 QStringLiteral("session_end"));
        for (const QJsonObject& line : lines) {
            QVERIFY(!line.value(QStringLiteral("timestamp_utc")).toString().isEmpty());
        }

        const QJsonArray names =
            lines.at(1).value(QStringLiteral("display_names")).toArray();
        QCOMPARE(names.size(), 3);
        QCOMPARE(names.at(0).toString(), QStringLiteral("safe.obj"));
        QCOMPARE(
            names.at(1).toString(),
            QStringLiteral("path_like_name_omitted"));
        QCOMPARE(
            names.at(2).toString(),
            QStringLiteral("path_like_name_omitted"));
        QCOMPARE(lines.at(1).value(QStringLiteral("mesh_count")).toInt(), 3);
        QCOMPARE(lines.at(1).value(QStringLiteral("outcome")).toString(),
                 QStringLiteral("success"));
        QCOMPARE(lines.at(2).value(QStringLiteral("metric")).toString(),
                 QStringLiteral("precision"));
        QCOMPARE(lines.at(2).value(QStringLiteral("outcome")).toString(),
                 QStringLiteral("failure"));
        QCOMPARE(lines.at(4).value(QStringLiteral("error")).toString(),
                 QStringLiteral("pose is corrupt"));

        QFile raw(log.filePath());
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QByteArray bytes = raw.readAll();
        QVERIFY(!bytes.contains("/secret/path.obj"));
        QVERIFY(!bytes.contains("folder\\private.ply"));
    }

    void rotatesBeforeTheNextRecordWouldExceedTheLimit()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DiagnosticsLog log(root.path(), 256);

        QCOMPARE(DiagnosticsLog::RotationLimitBytes, qint64(2 * 1024 * 1024));
        QVERIFY(log.recordSessionStart().ok);
        QVERIFY(log.recordFatalRendererError(QString(400, QLatin1Char('x'))).ok);

        QVERIFY(QFileInfo::exists(log.filePath()));
        QVERIFY(QFileInfo::exists(log.rotatedFilePath()));
        const QVector<QJsonObject> current = readJsonLines(log.filePath());
        const QVector<QJsonObject> previous = readJsonLines(log.rotatedFilePath());
        QCOMPARE(current.size(), 1);
        QCOMPARE(current.front().value(QStringLiteral("event")).toString(),
                 QStringLiteral("fatal_renderer_error"));
        QCOMPARE(previous.size(), 1);
        QCOMPARE(previous.front().value(QStringLiteral("event")).toString(),
                 QStringLiteral("session_start"));
    }
};

QTEST_MAIN(DiagnosticsMenuTest)
#include "diagnostics_menu_test.moc"
