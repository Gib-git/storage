#pragma once

#include <QWidget>

#include <memory>

#include "BackgroundTask.h"
#include "storage/core/duplicates.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/index.hpp"
#include "storage/core/journal.hpp"
#include "storage/core/node.hpp"

class DuplicatesModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeView;

class DuplicatesView : public QWidget {
    Q_OBJECT
public:
    explicit DuplicatesView(QWidget* parent = nullptr);
    ~DuplicatesView() override;

    // The folder to search: a node of a loaded scan (kept alive by `tree`).
    void setScope(std::shared_ptr<storage::ScanTree> tree, storage::Node* folder);
    bool busy() const;
    void startSearch() { find(); }

signals:
    void chooseFolderRequested();
    void filesChanged();  // something was trashed/linked/deleted: the usage view is stale
    void message(const QString& text);
    void searchFinished(int groups);

private:
    void find();
    void cancel();
    void onFound(std::vector<storage::DupGroup> groups);
    void apply(const char* action);
    void onApplied(int64_t batch, storage::ExecResult result);
    void updateSummary();
    void showContextMenu(const QPoint& pos);
    void setBusy(bool busy, const QString& title = {});

    std::shared_ptr<storage::ScanTree> tree_;
    storage::Node* folder_ = nullptr;

    std::unique_ptr<storage::Index> cache_;  // used only by the worker while a search runs
    std::unique_ptr<storage::DuplicateFinder> finder_;
    std::unique_ptr<storage::Journal> journal_;  // outlives executor_
    std::unique_ptr<storage::Executor> executor_;
    BackgroundTask task_;
    QTimer* poll_ = nullptr;

    QLabel* scopeLabel_ = nullptr;
    QComboBox* minSize_ = nullptr;
    QCheckBox* skipHidden_ = nullptr;
    QCheckBox* paranoid_ = nullptr;
    QPushButton* findButton_ = nullptr;

    QStackedWidget* stack_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QLabel* progressTitle_ = nullptr;
    QLabel* progressDetail_ = nullptr;
    QProgressBar* progressBar_ = nullptr;

    DuplicatesModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTreeView* view_ = nullptr;
    QComboBox* keepRule_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* trashButton_ = nullptr;
    QToolButton* moreButton_ = nullptr;
};
