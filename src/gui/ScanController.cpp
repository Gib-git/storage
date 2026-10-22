#include "ScanController.h"

#include "storage/core/filesystem.hpp"
#include "storage/core/index.hpp"
#include "storage/core/paths.hpp"
#include "storage/core/ssh_fs.hpp"
#include "storage/core/volumes.hpp"

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

void ScanController::scan(const QString& path, const std::string& volume) {
    if (busy_) return;
    FileSystem* fs = filesystemFor(volume);
    if (!fs) {
        emit finished(nullptr, tr("That NAS is no longer saved."));
        return;
    }
    joinWorker();
    busy_ = true;
    const bool local = isLocalVolume(volume);
    const std::string root = local ? normalizeLocalPath(path.toStdString()) : path.toStdString();
    {
        std::lock_guard lk(scannerMu_);
        scanner_ = std::make_unique<Scanner>(*fs, ScanOptions::defaultsFor(root));
    }
    emit started(QString::fromStdString(root));
    poll_.start();

    worker_ = std::thread([this, fs, local] {
        // Connect first so ssh's own error message (bad key, unknown host...) reaches the user.
        if (!local) {
            std::string message;
            if (auto ec = static_cast<SshFileSystem*>(fs)->connect(message)) {
                deliver(nullptr, tr("Could not connect: %1").arg(QString::fromStdString(message.empty() ? ec.message() : message)));
                return;
            }
        }
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
