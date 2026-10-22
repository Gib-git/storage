#include "ScanController.h"

#include "storage/core/filesystem.hpp"
#include "storage/core/index.hpp"
#include "storage/core/paths.hpp"

using namespace storage;

ScanController::ScanController(QObject* parent) : QObject(parent) {
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, [this] {
        std::lock_guard lk(scannerMu_);
        if (scanner_) emit progress(scanner_->progress());
    });
}

ScanController::~ScanController() {
    cancel();
    joinWorker();
}

void ScanController::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

void ScanController::cancel() {
    std::lock_guard lk(scannerMu_);
    if (scanner_) scanner_->cancel();
}

void ScanController::deliver(std::shared_ptr<ScanTree> tree, const QString& error) {
    QMetaObject::invokeMethod(
        this,
        [this, tree = std::move(tree), error] {
            poll_.stop();
            joinWorker();
            {
                std::lock_guard lk(scannerMu_);
                scanner_.reset();
            }
            busy_ = false;
            emit finished(tree, error);
        },
        Qt::QueuedConnection);
}

void ScanController::scan(const QString& path) {
    if (busy_) return;
    joinWorker();
    busy_ = true;
    const std::string root = normalizeLocalPath(path.toStdString());
    {
        std::lock_guard lk(scannerMu_);
        static LocalFileSystem localFs;  // stateless
        scanner_ = std::make_unique<Scanner>(localFs, ScanOptions::defaultsFor(root));
    }
    emit started(QString::fromStdString(root));
    poll_.start();

    worker_ = std::thread([this] {
        Scanner* s = nullptr;
        {
            std::lock_guard lk(scannerMu_);
            s = scanner_.get();
        }
        std::shared_ptr<ScanTree> tree = s->run();
        QString error;
        if (tree->complete) {
            try {
                Index(defaultIndexPath()).save(*tree);
            } catch (const std::exception& e) {
                error = QString::fromStdString(e.what());
            }
        }
        deliver(std::move(tree), error);
    });
}

void ScanController::load(qint64 scanId) {
    if (busy_) return;
    joinWorker();
    busy_ = true;
    emit started(tr("Loading saved scan…"));
    worker_ = std::thread([this, scanId] {
        std::shared_ptr<ScanTree> tree;
        QString error;
        try {
            tree = Index(defaultIndexPath()).load(scanId);
            if (!tree) error = tr("That scan is no longer in the index.");
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
        }
        deliver(std::move(tree), error);
    });
}
