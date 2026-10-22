#pragma once

#include <QWidget>

#include <memory>

#include "BackgroundTask.h"
#include "storage/core/executor.hpp"
#include "storage/core/journal.hpp"

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;

// Every journaled batch of file operations, with undo and resume.
class HistoryView : public QWidget {
    Q_OBJECT
public:
    explicit HistoryView(QWidget* parent = nullptr);
    ~HistoryView() override;

    void reload();
    bool busy() const { return task_.running(); }

signals:
    void filesChanged();
    void message(const QString& text);

private:
    void showBatch(int64_t id);
    void run(bool undo);
    int64_t selectedBatch() const;

    QTreeWidget* batches_ = nullptr;
    QTreeWidget* ops_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* undo_ = nullptr;
    QPushButton* resume_ = nullptr;

    std::unique_ptr<storage::Journal> journal_;
    std::unique_ptr<storage::Executor> executor_;
    BackgroundTask task_;
    QTimer* poll_ = nullptr;
};
