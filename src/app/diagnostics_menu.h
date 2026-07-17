#pragma once

#include <functional>

#include <QMenu>
#include <QString>
#include <QStringList>
#include <QUrl>

class DiagnosticsLog;
class IRendererAdapter;
class WorkspaceState;

struct DiagnosticsMenuServices
{
    std::function<void(const QString&)> copyText;
    std::function<bool(const QUrl&)> openUrl;
    std::function<void()> showAbout;
};

class DiagnosticsMenu final : public QMenu
{
public:
    DiagnosticsMenu(
        IRendererAdapter& renderer,
        const WorkspaceState& state,
        DiagnosticsLog& log,
        QWidget* parent = nullptr,
        DiagnosticsMenuServices services = {});

    QStringList actionTexts() const;
    QString diagnosticsText() const;
    void setRecentError(QString error);
    void setLoggingError(QString error);

private:
    IRendererAdapter& renderer_;
    const WorkspaceState& state_;
    DiagnosticsLog& log_;
    DiagnosticsMenuServices services_;
    QString recentError_;
    QString loggingError_;
};
