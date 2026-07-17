#include "diagnostics_menu.h"

#include "../core/renderer_adapter.h"
#include "../core/workspace_state.h"
#include "../services/diagnostics_log.h"

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMessageBox>
#include <QSysInfo>

#include <utility>

namespace
{
QString presentValue(const QString& value)
{
    return value.isEmpty() ? QStringLiteral("Unavailable") : value;
}

QString applicationDisplayName()
{
    QString name = QGuiApplication::applicationDisplayName();
    if (name.isEmpty())
        name = QCoreApplication::applicationName();
    return presentValue(name);
}

QString operatingSystemName()
{
    QString name = QSysInfo::prettyProductName();
    if (name.isEmpty()) {
        name = QStringLiteral("%1 %2")
                   .arg(QSysInfo::kernelType(), QSysInfo::kernelVersion())
                   .trimmed();
    }
    return presentValue(name);
}

QString redactedRecentError(
    const QString& error,
    const WorkspaceState& state)
{
    QString redacted = error;
    for (const MeshEntry& mesh : state.meshes()) {
        if (mesh.sourcePath.isEmpty())
            continue;
        redacted.replace(
            mesh.sourcePath,
            DiagnosticsLog::redactDisplayName(mesh.displayName));
    }
    if (redacted.contains(QLatin1Char('/')) ||
        redacted.contains(QLatin1Char('\\'))) {
        return QStringLiteral("path_like_error_omitted");
    }
    return redacted;
}
} // namespace

DiagnosticsMenu::DiagnosticsMenu(
    IRendererAdapter& renderer,
    const WorkspaceState& state,
    DiagnosticsLog& log,
    QWidget* parent,
    DiagnosticsMenuServices services)
    : QMenu(QStringLiteral("Diagnostics"), parent),
      renderer_(renderer),
      state_(state),
      log_(log),
      services_(std::move(services))
{
    if (!services_.copyText) {
        services_.copyText = [](const QString& text) {
            if (QGuiApplication::clipboard() != nullptr)
                QGuiApplication::clipboard()->setText(text);
        };
    }
    if (!services_.openUrl) {
        services_.openUrl = [](const QUrl& url) {
            return QDesktopServices::openUrl(url);
        };
    }
    if (!services_.showAbout) {
        services_.showAbout = [this] {
            QMessageBox::about(
                this,
                QStringLiteral("About Mesh Compare"),
                QStringLiteral("Mesh Compare %1")
                    .arg(presentValue(QCoreApplication::applicationVersion())));
        };
    }

    QAction* resetCamera = addAction(QStringLiteral("Reset Camera"));
    resetCamera->setObjectName(QStringLiteral("resetCameraAction"));
    connect(resetCamera, &QAction::triggered, this, [this] {
        renderer_.resetCamera();
    });

    QAction* orthographic = addAction(QStringLiteral("Orthographic"));
    orthographic->setObjectName(QStringLiteral("orthographicAction"));
    orthographic->setCheckable(true);
    connect(orthographic, &QAction::toggled, this, [this](bool enabled) {
        renderer_.setDiagnostic(DiagnosticFlag::Orthographic, enabled);
    });

    QAction* wireframe = addAction(QStringLiteral("Wireframe Overlay"));
    wireframe->setObjectName(QStringLiteral("wireframeOverlayAction"));
    wireframe->setCheckable(true);
    connect(wireframe, &QAction::toggled, this, [this](bool enabled) {
        renderer_.setDiagnostic(DiagnosticFlag::Wireframe, enabled);
    });

    QAction* normals = addAction(QStringLiteral("Show Normals"));
    normals->setObjectName(QStringLiteral("showNormalsAction"));
    normals->setCheckable(true);
    connect(normals, &QAction::toggled, this, [this](bool enabled) {
        renderer_.setDiagnostic(DiagnosticFlag::Normals, enabled);
    });

    QAction* copyDiagnostics = addAction(QStringLiteral("Copy Diagnostics"));
    copyDiagnostics->setObjectName(QStringLiteral("copyDiagnosticsAction"));
    connect(copyDiagnostics, &QAction::triggered, this, [this] {
        services_.copyText(diagnosticsText());
    });

    QAction* openLog = addAction(QStringLiteral("Open Local Log"));
    openLog->setObjectName(QStringLiteral("openLocalLogAction"));
    connect(openLog, &QAction::triggered, this, [this] {
        services_.openUrl(QUrl::fromLocalFile(log_.filePath()));
    });

    QAction* about = addAction(QStringLiteral("About"));
    about->setObjectName(QStringLiteral("aboutAction"));
    connect(about, &QAction::triggered, this, [this] {
        services_.showAbout();
    });
}

QStringList DiagnosticsMenu::actionTexts() const
{
    QStringList result;
    for (const QAction* action : actions())
        result.append(action->text());
    return result;
}

QString DiagnosticsMenu::diagnosticsText() const
{
    const RendererDiagnostics rendererDiagnostics = renderer_.diagnostics();
    QStringList displayNames;
    displayNames.reserve(state_.meshes().size());
    for (const MeshEntry& mesh : state_.meshes()) {
        displayNames.append(DiagnosticsLog::redactDisplayName(mesh.displayName));
    }

    QStringList lines;
    lines.append(QStringLiteral("Application: %1").arg(applicationDisplayName()));
    lines.append(QStringLiteral("Application version: %1")
                     .arg(presentValue(QCoreApplication::applicationVersion())));
    lines.append(QStringLiteral("Qt version: %1")
                     .arg(QString::fromLatin1(qVersion())));
    lines.append(QStringLiteral("Operating system: %1")
                     .arg(operatingSystemName()));
    lines.append(QStringLiteral("Renderer backend: %1")
                     .arg(presentValue(rendererDiagnostics.backendName)));
    lines.append(QStringLiteral("OpenGL vendor: %1")
                     .arg(presentValue(rendererDiagnostics.openGlVendor)));
    lines.append(QStringLiteral("OpenGL renderer: %1")
                     .arg(presentValue(rendererDiagnostics.openGlRenderer)));
    lines.append(QStringLiteral("OpenGL version: %1")
                     .arg(presentValue(rendererDiagnostics.openGlVersion)));
    lines.append(QStringLiteral("Workspace mesh count: %1")
                     .arg(state_.meshes().size()));
    lines.append(QStringLiteral("Workspace display names: %1")
                     .arg(displayNames.isEmpty()
                              ? QStringLiteral("None")
                              : displayNames.join(QStringLiteral(", "))));
    lines.append(QStringLiteral("Diagnostics logging: %1")
                     .arg(loggingError_.isEmpty()
                              ? QStringLiteral("Available")
                              : redactedRecentError(loggingError_, state_)));
    lines.append(QStringLiteral("Most recent error: %1")
                     .arg(recentError_.isEmpty()
                              ? QStringLiteral("None")
                              : redactedRecentError(recentError_, state_)));
    return lines.join(QLatin1Char('\n'));
}

void DiagnosticsMenu::setRecentError(QString error)
{
    recentError_ = std::move(error);
}

void DiagnosticsMenu::setLoggingError(QString error)
{
    loggingError_ = std::move(error);
}
