#pragma once

#include <QMainWindow>

#include <memory>
#include <vector>

#include "storage/core/node.hpp"
#include "storage/core/scanner.hpp"

class BreadcrumbBar;
class CategoryBar;
class QAction;
class QLabel;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QTreeView;
class ScanController;
class ScanModel;
class Sidebar;
class TreemapWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Scan a path (or open its saved scan) — used for command-line arguments.
    void openPath(const QString& path, bool forceRescan);

signals:
    void scanFinished(bool ok);

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void buildActions();
    void buildUi();
    QWidget* buildWelcomePage();
    QWidget* buildProgressPage();
    QWidget* buildBrowserPage();

    void chooseFolder();
    void startScan(const QString& path);
    void onScanStarted(const QString& what);
    void onScanProgress(const storage::ScanProgress& p);
    void onScanFinished(std::shared_ptr<storage::ScanTree> tree, const QString& error);

    void navigate(storage::Node* node, bool record = true);
    void goBack();
    void goForward();
    void goUp();
    void selectNode(storage::Node* node);
    storage::Node* selectedNode() const;
    void updateActions();
    void updateSummary();

    void showContextMenu(storage::Node* node, const QPoint& globalPos);
    void revealInFileManager(const QString& path);
    QString nativePath(const storage::Node* node) const;

    void setThemeMode(int mode);
    void showAbout();

    ScanController* scanner_ = nullptr;
    std::shared_ptr<storage::ScanTree> tree_;
    storage::Node* current_ = nullptr;
    std::vector<storage::Node*> back_, forward_;
    QString pendingScanRoot_;

    Sidebar* sidebar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    BreadcrumbBar* crumbs_ = nullptr;
    QLabel* summary_ = nullptr;
    QTreeView* view_ = nullptr;
    ScanModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    TreemapWidget* treemap_ = nullptr;
    CategoryBar* categories_ = nullptr;

    QLabel* progressTitle_ = nullptr;
    QLabel* progressStats_ = nullptr;
    QLabel* progressPath_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* progressCancel_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QAction* actBack_ = nullptr;
    QAction* actForward_ = nullptr;
    QAction* actUp_ = nullptr;
    QAction* actScan_ = nullptr;
    QAction* actRescan_ = nullptr;
    QAction* actStop_ = nullptr;
    QAction* actReveal_ = nullptr;
};
