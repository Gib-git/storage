#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "storage/core/node.hpp"
#include "storage/core/scanner.hpp"

// Runs scans and index loads off the GUI thread and reports back on it.
class ScanController : public QObject {
    Q_OBJECT
public:
    explicit ScanController(QObject* parent = nullptr);
    ~ScanController() override;

    bool busy() const { return busy_; }
    // `volume` is "local" or a saved NAS's "ssh://..." volume.
    void scan(const QString& path, const std::string& volume = "local");
    void load(qint64 scanId);
    void cancel();

signals:
    void started(const QString& what);
    void progress(const storage::ScanProgress& p);
    void finished(std::shared_ptr<storage::ScanTree> tree, const QString& error);

private:
    void deliver(std::shared_ptr<storage::ScanTree> tree, const QString& error);
    void joinWorker();

    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::mutex scannerMu_;
    std::unique_ptr<storage::Scanner> scanner_;
    QTimer poll_;
};
