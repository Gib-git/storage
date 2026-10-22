#include "DuplicatesModel.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QLocale>

#include <algorithm>
#include <unordered_set>

#include "storage/core/format.hpp"

using namespace storage;

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
}

DuplicatesModel::DuplicatesModel(QObject* parent) : QAbstractItemModel(parent) {}

void DuplicatesModel::setGroups(std::vector<DupGroup> groups, const QString& base) {
    beginResetModel();
    base_ = base;
    groups_ = std::move(groups);
    remove_.assign(groups_.size(), {});
    for (size_t i = 0; i < groups_.size(); ++i) remove_[i].assign(groups_[i].files.size(), false);
    endResetModel();
    emit selectionChanged();
}

void DuplicatesModel::applyRule(KeepRule rule, const std::vector<std::string>& preferred) {
    for (size_t g = 0; g < groups_.size(); ++g) {
        const size_t keep = chooseKeeper(groups_[g], rule, preferred);
        for (size_t f = 0; f < remove_[g].size(); ++f) remove_[g][f] = f != keep;
    }
    if (!groups_.empty()) emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    for (int g = 0; g < rowCount(); ++g) {
        const QModelIndex parent = index(g, 0);
        if (rowCount(parent)) emit dataChanged(index(0, 0, parent), index(rowCount(parent) - 1, ColumnCount - 1, parent));
    }
    emit selectionChanged();
}

void DuplicatesModel::clearSelection() {
    for (auto& r : remove_) std::fill(r.begin(), r.end(), false);
    beginResetModel();
    endResetModel();
    emit selectionChanged();
}

void DuplicatesModel::keepOnly(int group, int file) {
    if (group < 0 || static_cast<size_t>(group) >= groups_.size()) return;
    auto& r = remove_[static_cast<size_t>(group)];
    for (size_t f = 0; f < r.size(); ++f) r[f] = static_cast<int>(f) != file;
    const QModelIndex parent = index(group, 0);
    emit dataChanged(index(0, 0, parent), index(rowCount(parent) - 1, ColumnCount - 1, parent));
    emit dataChanged(parent, index(group, ColumnCount - 1));
    emit selectionChanged();
}

void DuplicatesModel::removePaths(const std::vector<std::string>& gone) {
    const std::unordered_set<std::string> set(gone.begin(), gone.end());
    beginResetModel();
    std::vector<DupGroup> groups;
    std::vector<std::vector<bool>> remove;
    for (size_t g = 0; g < groups_.size(); ++g) {
        DupGroup kept = groups_[g];
        kept.files.clear();
        std::vector<bool> sel;
        for (size_t f = 0; f < groups_[g].files.size(); ++f) {
            if (set.count(groups_[g].files[f].path)) continue;
            kept.files.push_back(groups_[g].files[f]);
            sel.push_back(remove_[g][f]);
        }
        if (kept.files.size() < 2) continue;
        groups.push_back(std::move(kept));
        remove.push_back(std::move(sel));
    }
    groups_ = std::move(groups);
    remove_ = std::move(remove);
    endResetModel();
    emit selectionChanged();
}

size_t DuplicatesModel::selectedCount() const {
    size_t n = 0;
    for (const auto& r : remove_) n += static_cast<size_t>(std::count(r.begin(), r.end(), true));
    return n;
}

uint64_t DuplicatesModel::selectedBytes() const {
    uint64_t b = 0;
    for (size_t g = 0; g < groups_.size(); ++g)
        b += groups_[g].size * static_cast<uint64_t>(std::count(remove_[g].begin(), remove_[g].end(), true));
    return b;
}

uint64_t DuplicatesModel::reclaimableBytes() const {
    uint64_t b = 0;
    for (const auto& g : groups_) b += g.reclaimable();
    return b;
}

QString DuplicatesModel::relativeFolder(const std::string& path) const {
    const QString folder = QFileInfo(qs(path)).path();
    if (base_.isEmpty()) return QDir::toNativeSeparators(folder);
    const QString rel = QDir(base_).relativeFilePath(folder);
    return rel == "." ? tr("(top level)") : QDir::toNativeSeparators(rel);
}

int DuplicatesModel::groupOf(const QModelIndex& index) const {
    if (!index.isValid()) return -1;
    return isGroup(index) ? index.row() : static_cast<int>(index.internalId());
}

QString DuplicatesModel::pathAt(const QModelIndex& index) const {
    if (!index.isValid() || isGroup(index)) return {};
    const auto& g = groups_[index.internalId()];
    return QDir::toNativeSeparators(qs(g.files[static_cast<size_t>(index.row())].path));
}

QModelIndex DuplicatesModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column < 0 || column >= ColumnCount) return {};
    if (!parent.isValid()) {
        if (static_cast<size_t>(row) >= groups_.size()) return {};
        return createIndex(row, column, kGroupId);
    }
    if (!isGroup(parent)) return {};
    const auto& g = groups_[static_cast<size_t>(parent.row())];
    if (static_cast<size_t>(row) >= g.files.size()) return {};
    return createIndex(row, column, static_cast<quintptr>(parent.row()));
}

QModelIndex DuplicatesModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || isGroup(child)) return {};
    return createIndex(static_cast<int>(child.internalId()), 0, kGroupId);
}

int DuplicatesModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) return static_cast<int>(groups_.size());
    if (parent.column() > 0 || !isGroup(parent)) return 0;
    return static_cast<int>(groups_[static_cast<size_t>(parent.row())].files.size());
}

int DuplicatesModel::columnCount(const QModelIndex&) const { return ColumnCount; }

QVariant DuplicatesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    if (isGroup(index)) {
        const auto& g = groups_[static_cast<size_t>(index.row())];
        const auto& sel = remove_[static_cast<size_t>(index.row())];
        const int removing = static_cast<int>(std::count(sel.begin(), sel.end(), true));
        switch (role) {
            case Qt::DisplayRole:
                switch (index.column()) {
                    case Name: {
                        // Title the group after the copy being kept.
                        size_t kept = 0;
                        while (kept < sel.size() && sel[kept]) ++kept;
                        const auto& f = g.files[kept < g.files.size() ? kept : 0];
                        return tr("%1  —  %2 copies").arg(QFileInfo(qs(f.path)).fileName()).arg(g.files.size());
                    }
                    case Size: return qs(humanSize(g.size)) + tr(" each");
                    case Modified: return removing ? tr("%n to remove", nullptr, removing) : QString();
                    case Folder: return tr("%1 reclaimable").arg(qs(humanSize(g.reclaimable())));
                }
                break;
            case Qt::FontRole: {
                QFont f;
                f.setBold(true);
                return f;
            }
            case SortRole:
                if (index.column() == Name) return QFileInfo(qs(g.files.front().path)).fileName().toLower();
                return static_cast<qulonglong>(g.reclaimable());
            case Qt::TextAlignmentRole:
                if (index.column() == Size) return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
                break;
        }
        return {};
    }

    const size_t gi = index.internalId(), fi = static_cast<size_t>(index.row());
    const auto& f = groups_[gi].files[fi];
    const bool remove = remove_[gi][fi];
    const QFileInfo info(qs(f.path));
    switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
                case Name: return info.fileName();
                case Size: return qs(humanSize(f.size));
                case Modified:
                    return f.mtime ? QLocale().toString(QDateTime::fromSecsSinceEpoch(f.mtime), QLocale::ShortFormat) : QString();
                case Folder: return relativeFolder(f.path);
            }
            break;
        case Qt::CheckStateRole:
            if (index.column() == Name) return remove ? Qt::Checked : Qt::Unchecked;
            break;
        case Qt::ToolTipRole: return QDir::toNativeSeparators(qs(f.path));
        case Qt::ForegroundRole:
            if (remove) return QColor(0xEF, 0x44, 0x44);
            break;
        case Qt::FontRole:
            if (remove) {
                QFont font;
                font.setStrikeOut(true);
                return font;
            }
            break;
        case Qt::TextAlignmentRole:
            if (index.column() == Size) return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
            break;
        case SortRole:
            switch (index.column()) {
                case Name: return info.fileName().toLower();
                case Size: return static_cast<qulonglong>(f.size);
                case Modified: return static_cast<qlonglong>(f.mtime);
                case Folder: return info.path().toLower();
            }
            break;
        case PathRole: return qs(f.path);
    }
    return {};
}

bool DuplicatesModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::CheckStateRole || !index.isValid() || isGroup(index)) return false;
    const size_t gi = index.internalId(), fi = static_cast<size_t>(index.row());
    const bool want = value.toInt() == Qt::Checked;
    if (want) {
        size_t kept = 0;
        for (size_t k = 0; k < remove_[gi].size(); ++k) kept += remove_[gi][k] ? 0 : 1;
        if (kept <= 1) {
            emit refused(tr("At least one copy of every file is always kept."));
            return false;
        }
    }
    remove_[gi][fi] = want;
    emit dataChanged(this->index(index.row(), 0, index.parent()), this->index(index.row(), ColumnCount - 1, index.parent()));
    const QModelIndex group = this->index(static_cast<int>(gi), 0);
    emit dataChanged(group, this->index(static_cast<int>(gi), ColumnCount - 1));
    emit selectionChanged();
    return true;
}

Qt::ItemFlags DuplicatesModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!isGroup(index) && index.column() == Name) f |= Qt::ItemIsUserCheckable;
    return f;
}

QVariant DuplicatesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Name: return tr("File");
        case Size: return tr("Size");
        case Modified: return tr("Modified");
        case Folder: return tr("Folder");
    }
    return {};
}
