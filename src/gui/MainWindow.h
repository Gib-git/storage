#pragma once

#include <QMainWindow>

#include <memory>
#include <thread>
#include <vector>

#include "storage/core/node.hpp"
#include "storage/core/scanner.hpp"

class BreadcrumbBar;
class QLineEdit;
namespace storage {
class Service;
namespace http {
class Server;
}
}  // namespace storage
class CleanupView;
class DuplicatesView;
class HistoryView;
class OrganizeView;
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
    ~MainWindow() override;

    // Scan a path (or open its saved scan) — used for command-line arguments.
    void openPath(const QString& path, bool forceRescan);
    // Open a saved NAS: its saved scan if there is one, otherwise scan its folder.
    void openNas(const QString& name);
    // Switch to the Duplicates view for the current folder and start searching.
    void findDuplicatesNow();
    // Switch to the Organize view for the current folder and preview the selected rules.
    void previewOrganizeNow();
    // Switch to Clean Up and measure the system locations.
    void showCleanupNow();

signals:
    void scanFinished(bool ok);
    void duplicatesFound(int groups);
    void organizePreviewReady(int moves);
    void cleanupAnalyzed();

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
    void startScan(const QString& path, const std::string& volume = "local");
    void editNas(const QString& name);  // empty name = add a new one
    void removeNas(const QString& name);
    bool remote() const;
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

    void showSettings();
    void applyApiSettings();
    void stopApi();
    void notifyDone();

    enum Mode { UsageMode = 0, DuplicatesMode = 1, OrganizeMode = 2, CleanupMode = 3, HistoryMode = 4, ModeCount = 5 };
    void setMode(Mode m);
    void recoverInterruptedWork();

    ScanController* scanner_ = nullptr;
    std::shared_ptr<storage::ScanTree> tree_;
    storage::Node* current_ = nullptr;
    std::vector<storage::Node*> back_, forward_;
    QString pendingScanRoot_;

    Sidebar* sidebar_ = nullptr;
    QStackedWidget* modes_ = nullptr;   // Usage / Duplicates / Organize / History
    QStackedWidget* stack_ = nullptr;   // Usage: welcome / progress / browser
    DuplicatesView* dupes_ = nullptr;
    OrganizeView* organize_ = nullptr;
    CleanupView* cleanup_ = nullptr;
    HistoryView* history_ = nullptr;
    QAction* modeActions_[ModeCount] = {};
    int returnToMode_ = -1;  // mode to go back to after a scan started from another view
    BreadcrumbBar* crumbs_ = nullptr;
    QLabel* summary_ = nullptr;
    QLineEdit* filter_ = nullptr;
    std::unique_ptr<storage::Service> apiService_;
    std::unique_ptr<storage::http::Server> apiServer_;
    std::thread apiThread_;
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
