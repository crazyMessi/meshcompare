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
class QEvent;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QResizeEvent;
class QToolButton;
class QWidget;
class WorkspaceState;
struct AnalysisBatchResult;
struct WorkspaceImportOutcome;
namespace python_hole_filling
{
class ICommands;
class Panel;
}

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
    void bindHoleFillingCommands(
        python_hole_filling::ICommands& commands);
    void unbindHoleFillingCommands();
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
    void layoutModeRequested(SceneLayoutMode layoutMode);
    void gridNormalizationRequested(bool enabled);
    void meshVisibilityRequested(MeshId meshId, bool visible);
    void operationFailed(const QString& message);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* buildCommandBar(QWidget* parent);
    QFrame* buildNoticeBanner(QWidget* parent);
    QFrame* buildStatusBar(QWidget* parent);
    void rebuildLayersMenu();
    bool confirmWorkspaceReplacement();
    void toggleColoringPanel();
    void positionColoringPanel();
    void toggleCameraPanel();
    void positionCameraPanel();
    void toggleHoleFillingPanel();
    void positionHoleFillingPanel();
    void requestMeshVisibilityChange(MeshId meshId, bool visible);
    void showPassiveStatusMessage(const QString& message);
    void updateWorkspaceStatus();
    void presentPanelFailure(const QString& message);

    WorkspaceState& state_;
    QWidget* commandBar_ = nullptr;
    QPushButton* importMeshesButton_ = nullptr;
    QPushButton* coloringButton_ = nullptr;
    QPushButton* cameraButton_ = nullptr;
    QPushButton* holeFillingButton_ = nullptr;
    QPushButton* overlayViewButton_ = nullptr;
    QPushButton* gridViewButton_ = nullptr;
    QPushButton* normalizeGridButton_ = nullptr;
    QToolButton* layersButton_ = nullptr;
    QMenu* layersMenu_ = nullptr;
    QToolButton* diagnosticsButton_ = nullptr;
    QFrame* viewportFrame_ = nullptr;
    QWidget* viewportHost_ = nullptr;
    QWidget* noticeArea_ = nullptr;
    QFrame* noticeBanner_ = nullptr;
    QLabel* noticeLabel_ = nullptr;
    QFrame* statusBar_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* workspaceStatusLabel_ = nullptr;
    QLabel* meshCountLabel_ = nullptr;
    ColoringPanel* coloringPanel_ = nullptr;
    CameraPanel* cameraPanel_ = nullptr;
    python_hole_filling::Panel* holeFillingPanel_ = nullptr;
};
