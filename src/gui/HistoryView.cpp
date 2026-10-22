#include "HistoryView.h"

#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Theme.h"
#include "storage/core/format.hpp"
#include "storage/core/volumes.hpp"

using namespace storage;

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }


QString stateLabel(const std::string& s) {
    if (s == BatchState::Done) return QObject::tr("Done");
    if (s == BatchState::Undone) return QObject::tr("Undone");
    if (s == BatchState::Interrupted) return QObject::tr("Interrupted");
    if (s == BatchState::Running) return QObject::tr("Running");
    if (s == BatchState::Planned) return QObject::tr("Not started");
    return qs(s);
}

QColor stateColor(const std::string& s) {
    if (s == OpState::Done) return QColor(0x22, 0xA0, 0x6B);
    if (s == OpState::Failed) return QColor(0xEF, 0x44, 0x44);
    if (s == OpState::Skipped || s == BatchState::Interrupted) return QColor(0xF5, 0x9E, 0x0B);
    return theme::mutedText();
}
}  // namespace

HistoryView::HistoryView(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName("Header");
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(12, 8, 12, 8);
    auto* title = new QLabel(tr("<b>History</b> — every change Storage made, recorded before it happened"), header);
    h->addWidget(title, 1);
    resume_ = new QPushButton(tr("Resume"), header);
    resume_->setToolTip(tr("Finish a batch that was interrupted"));
    connect(resume_, &QPushButton::clicked, this, [this] { run(false); });
    undo_ = new QPushButton(tr("Undo"), header);
    undo_->setToolTip(tr("Restore trashed files, unlink hard links and clones, move files back"));
    connect(undo_, &QPushButton::clicked, this, [this] { run(true); });
    h->addWidget(resume_);
    h->addWidget(undo_);
    v->addWidget(header);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setHandleWidth(1);
    batches_ = new QTreeWidget(split);
    batches_->setHeaderLabels({tr("When"), tr("What"), tr("State"), tr("Files"), tr("Size")});
    batches_->setRootIsDecorated(false);
    batches_->setAlternatingRowColors(true);
    batches_->header()->setStretchLastSection(false);
    batches_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    batches_->header()->resizeSection(0, 150);
    connect(batches_, &QTreeWidget::currentItemChanged, this, [this] { showBatch(selectedBatch()); });

    auto* lower = new QWidget(split);
    auto* lv = new QVBoxLayout(lower);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);
    detail_ = new QLabel(lower);
    detail_->setContentsMargins(12, 6, 12, 6);
    lv->addWidget(detail_);
    ops_ = new QTreeWidget(lower);
    ops_->setHeaderLabels({tr("State"), tr("Action"), tr("File"), tr("Result")});
    ops_->setRootIsDecorated(false);
    ops_->setAlternatingRowColors(true);
    ops_->setUniformRowHeights(true);
    ops_->header()->resizeSection(0, 90);
    ops_->header()->resizeSection(1, 90);
    ops_->header()->resizeSection(2, 480);
    lv->addWidget(ops_, 1);
    split->addWidget(batches_);
    split->addWidget(lower);
    split->setSizes({260, 400});
    v->addWidget(split, 1);

    poll_ = new QTimer(this);
    poll_->setInterval(150);
    connect(poll_, &QTimer::timeout, this, [this] {
        if (!executor_) return;
        const auto p = executor_->progress();
        detail_->setText(tr("Working… %1 of %2").arg(p.done).arg(p.total));
    });
    reload();
}

HistoryView::~HistoryView() {
    if (executor_) executor_->cancel();
    task_.join();
}

int64_t HistoryView::selectedBatch() const {
    auto* item = batches_->currentItem();
    return item ? item->data(0, Qt::UserRole).toLongLong() : 0;
}

void HistoryView::reload() {
    if (busy()) return;
    const int64_t keep = selectedBatch();
    batches_->clear();
    try {
        Journal journal(Journal::defaultPath());
        for (const auto& b : journal.batches(200)) {
            auto* it = new QTreeWidgetItem(batches_);
            it->setData(0, Qt::UserRole, static_cast<qlonglong>(b.id));
            it->setText(0, qs(formatTime(b.created)));
            it->setText(1, qs(b.title));
            it->setText(2, stateLabel(b.state));
            it->setForeground(2, stateColor(b.state));
            it->setText(3, tr("%1 / %2").arg(b.done + b.undone).arg(b.total));
            it->setText(4, qs(humanSize(b.bytes)));
            if (b.id == keep) batches_->setCurrentItem(it);
        }
    } catch (const std::exception& e) {
        detail_->setText(qs(e.what()));
    }
    if (!batches_->currentItem() && batches_->topLevelItemCount()) batches_->setCurrentItem(batches_->topLevelItem(0));
    if (!batches_->topLevelItemCount()) {
        ops_->clear();
        detail_->setText(tr("Nothing yet. Actions you take on duplicates and files will appear here."));
        undo_->setEnabled(false);
        resume_->setEnabled(false);
    }
}

void HistoryView::showBatch(int64_t id) {
    ops_->clear();
    if (!id) return;
    try {
        Journal journal(Journal::defaultPath());
        const JournalBatch b = journal.batch(id);
        for (const auto& op : journal.ops(id)) {
            auto* it = new QTreeWidgetItem(ops_);
            it->setText(0, qs(op.state));
            it->setForeground(0, stateColor(op.state));
            it->setText(1, qs(op.action));
            it->setText(2, QDir::toNativeSeparators(qs(op.src)));
            it->setToolTip(2, QDir::toNativeSeparators(qs(op.src)) +
                                  (op.dst.empty() ? QString() : "\n→ " + QDir::toNativeSeparators(qs(op.dst))));
            it->setText(3, qs(op.detail));
            it->setToolTip(3, qs(op.detail));
        }
        detail_->setText(tr("%1 done · %2 skipped · %3 failed · %4 undone")
                             .arg(b.done)
                             .arg(b.skipped)
                             .arg(b.failed)
                             .arg(b.undone));
        undo_->setEnabled(b.done > 0 && !busy());
        resume_->setEnabled((b.state == BatchState::Interrupted || b.state == BatchState::Planned) && !busy());
    } catch (const std::exception& e) {
        detail_->setText(qs(e.what()));
    }
}

void HistoryView::run(bool undo) {
    const int64_t id = selectedBatch();
    if (!id || busy()) return;
    const QString what = batches_->currentItem()->text(1);
    if (QMessageBox::question(this, undo ? tr("Undo") : tr("Resume"),
                              (undo ? tr("Undo “%1”?") : tr("Finish “%1”?")).arg(what)) != QMessageBox::Yes)
        return;
    FileSystem* fs = nullptr;
    try {
        journal_ = std::make_unique<Journal>(Journal::defaultPath());
        fs = filesystemFor(journal_->batch(id).volume);
        if (!fs) throw std::runtime_error("This batch belongs to a NAS that is no longer saved. Add it again to continue.");
    } catch (const std::exception& e) {
        journal_.reset();
        QMessageBox::critical(this, tr("History"), qs(e.what()));
        return;
    }
    executor_ = std::make_unique<Executor>(*fs, *journal_);
    undo_->setEnabled(false);
    resume_->setEnabled(false);
    poll_->start();
    Executor* ex = executor_.get();
    Journal* journal = journal_.get();
    task_.start<ExecResult>(
        this,
        [ex, id, undo, fs, journal] {
            // A NAS batch is only checked for interrupted steps once we can reach the NAS.
            try {
                Executor::recover(*fs, *journal);
            } catch (const std::exception&) {
            }
            return undo ? ex->undo(id) : ex->run(id);
        },
        [this, undo](ExecResult r) {
            poll_->stop();
            executor_.reset();
            journal_.reset();
            reload();
            QString msg = (undo ? tr("Reverted %n file(s)", nullptr, r.done) : tr("Completed %n file(s)", nullptr, r.done));
            if (r.skipped) msg += tr(" · %n skipped", nullptr, r.skipped);
            if (r.failed) msg += tr(" · %n failed", nullptr, r.failed);
            emit message(msg);
            if (r.done) emit filesChanged();
        });
}
