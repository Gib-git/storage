#pragma once

#include <QWidget>

#include <memory>

#include "BackgroundTask.h"
#include "storage/core/cleaner.hpp"
#include "storage/core/executor.hpp"
#include "storage/core/journal.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class CleanupView : public QWidget {
    Q_OBJECT
public:
    explicit CleanupView(QWidget* parent = nullptr);
    ~CleanupView() override;

    void setScope(std::shared_ptr<storage::ScanTree> tree, storage::Node* folder);
    bool busy() const { return task_.running(); }
    // Measure the system targets if that has not happened yet.
    void ensureAnalyzed();

signals:
    void filesChanged();
    void message(const QString& text);
    void analyzed();

protected:
    void showEvent(QShowEvent* e) override;

private:
    void analyze();
    void showItems();
    void updateSystemSummary();
    void cleanSystem();
    void findInFolder();
    void updateFolderSummary();
    void cleanFolder();
    void run(std::vector<storage::JournalOp> ops, const QString& title, std::function<void()> after);
    void setWorking(bool on, const QString& text = {});

    QTabWidget* tabs_ = nullptr;

    // System & apps
    std::vector<storage::CleanItem> items_;
    bool analyzedOnce_ = false;
    QTreeWidget* targets_ = nullptr;
    QCheckBox* permanent_ = nullptr;
    QLabel* systemSummary_ = nullptr;
    QPushButton* cleanButton_ = nullptr;
    QPushButton* rescanButton_ = nullptr;
    QTreeWidget* commands_ = nullptr;

    // In this folder
    std::shared_ptr<storage::ScanTree> tree_;
    storage::Node* folder_ = nullptr;
    QLabel* folderLabel_ = nullptr;
    QComboBox* age_ = nullptr;
    QComboBox* minSize_ = nullptr;
    QTreeWidget* findings_ = nullptr;
    QLabel* folderSummary_ = nullptr;
    QPushButton* folderClean_ = nullptr;

    QProgressBar* progress_ = nullptr;
    std::unique_ptr<storage::Journal> journal_;
    std::unique_ptr<storage::Executor> executor_;
    std::atomic<bool> cancel_{false};
    BackgroundTask task_;
    QTimer* poll_ = nullptr;
};
