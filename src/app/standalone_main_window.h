#pragma once

#include <QMainWindow>
#include <QStringList>

#include "../core/meshcompare_types.h"

class CameraPanel;
class ColoringPanel;
class ICameraCommands;
class IColoringCommands;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QMenu;
class QPushButton;
class QResizeEvent;
class QToolButton;
class QWidget;
class WorkspaceState;
struct AnalysisBatchResult;
struct WorkspaceImportOutcome;

class StandaloneMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit StandaloneMainWindow(WorkspaceState& state, QWidget* parent = nullptr);

    QWidget* viewportHost() const;
    void requestImport(const QStringList& paths);
    void presentImportOutcome(const WorkspaceImportOutcome& outcome);
    void refreshWorkspace();
    void bindColoringCommands(IColoringCommands& commands);
    void unbindColoringCommands();
    void bindCameraCommands(ICameraCommands& commands);
    void unbindCameraCommands();
    void setDiagnosticsMenu(QMenu* menu);
    void presentAnalysisProgress(
        quint64 generation,
        quint64 batchSerial,
        MeshId meshId,
        int percent,
        const QString& message);
    void presentAnalysisFinished(const AnalysisBatchResult& result);

public slots:
    void showStatusMessage(const QString& message);

signals:
    void importRequested(const QStringList& paths);
    void operationFailed(const QString& message);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* buildCommandBar(QWidget* parent);
    bool confirmWorkspaceReplacement();
    void toggleColoringPanel();
    void positionColoringPanel();
    void toggleCameraPanel();
    void positionCameraPanel();
    void presentPanelFailure(const QString& message);

    WorkspaceState& state_;
    QWidget* commandBar_ = nullptr;
    QPushButton* importMeshesButton_ = nullptr;
    QPushButton* coloringButton_ = nullptr;
    QPushButton* cameraButton_ = nullptr;
    QToolButton* diagnosticsButton_ = nullptr;
    QWidget* viewportHost_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* meshCountLabel_ = nullptr;
    ColoringPanel* coloringPanel_ = nullptr;
    CameraPanel* cameraPanel_ = nullptr;
};
