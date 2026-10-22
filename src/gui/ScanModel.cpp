#include "ScanModel.h"

#include <QDateTime>
#include <QLocale>

#include "storage/core/format.hpp"

using namespace storage;

ScanModel::ScanModel(QObject* parent) : QAbstractItemModel(parent) {
    folderIcon_ = icons_.icon(QFileIconProvider::Folder);
    fileIcon_ = icons_.icon(QFileIconProvider::File);
    linkIcon_ = icons_.icon(QFileIconProvider::File);
}

void ScanModel::setTree(ScanTree* tree) {
    beginResetModel();
    tree_ = tree;
    endResetModel();
}

Node* ScanModel::nodeAt(const QModelIndex& index) const {
    if (!index.isValid()) return nullptr;
    return static_cast<Node*>(index.internalPointer());
}

QModelIndex ScanModel::indexFor(Node* node, int column) const {
    if (!node || !tree_) return {};
    if (node == tree_->root.get()) return createIndex(0, column, node);
    return createIndex(static_cast<int>(node->row), column, node);
}

QModelIndex ScanModel::index(int row, int column, const QModelIndex& parent) const {
    if (!tree_ || row < 0 || column < 0 || column >= ColumnCount) return {};
    if (!parent.isValid()) return row == 0 ? createIndex(0, column, tree_->root.get()) : QModelIndex();
    Node* p = nodeAt(parent);
    if (!p || row >= static_cast<int>(p->children.size())) return {};
    return createIndex(row, column, p->children[static_cast<size_t>(row)].get());
}

QModelIndex ScanModel::parent(const QModelIndex& child) const {
    Node* n = nodeAt(child);
    if (!n || !n->parent) return {};
    return indexFor(n->parent);
}

int ScanModel::rowCount(const QModelIndex& parent) const {
    if (!tree_) return 0;
    if (!parent.isValid()) return tree_->root ? 1 : 0;
    if (parent.column() > 0) return 0;
    Node* n = nodeAt(parent);
    return n ? static_cast<int>(n->children.size()) : 0;
}

int ScanModel::columnCount(const QModelIndex&) const { return ColumnCount; }

bool ScanModel::hasChildren(const QModelIndex& parent) const {
    if (!parent.isValid()) return tree_ && tree_->root;
    Node* n = nodeAt(parent);
    return n && !n->children.empty();
}

QVariant ScanModel::data(const QModelIndex& index, int role) const {
    Node* n = nodeAt(index);
    if (!n) return {};
    const Node* parent = n->parent;
    const double share =
        parent && parent->size ? static_cast<double>(n->size) / static_cast<double>(parent->size) : 1.0;

    switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
                case Name: {
                    QString name = QString::fromStdString(n->name);
                    if (n->flags & NodeFlag::Excluded) name += tr("  (not scanned)");
                    if (n->flags & NodeFlag::Unreadable) name += tr("  (no access)");
                    return name;
                }
                case Size: return QString::fromStdString(humanSize(n->size));
                case Share: return QString::number(share * 100.0, 'f', 1) + "%";
                case Items:
                    return n->isDir() ? QLocale().toString(static_cast<qulonglong>(n->files)) : QString();
                case Modified:
                    return n->mtime ? QLocale().toString(QDateTime::fromSecsSinceEpoch(n->mtime), QLocale::ShortFormat)
                                    : QString();
            }
            break;
        case Qt::DecorationRole:
            if (index.column() == Name)
                return n->isDir() ? folderIcon_ : n->kind == NodeKind::Symlink ? linkIcon_ : fileIcon_;
            break;
        case Qt::TextAlignmentRole:
            if (index.column() == Size || index.column() == Items)
                return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
            break;
        case Qt::ToolTipRole:
            if (index.column() == Name) {
                QString tip = QString::fromStdString(n->path(n->name.find('\\') != std::string::npos ? '\\' : '/'));
                if (n->flags & NodeFlag::HardlinkRepeat) tip += tr("\nHard link — space already counted elsewhere");
                if (n->errors) tip += tr("\n%n folder(s) could not be read", nullptr, static_cast<int>(n->errors));
                return tip;
            }
            if (index.column() == Size) return QLocale().toString(static_cast<qulonglong>(n->size)) + tr(" bytes");
            break;
        case SortRole:
            switch (index.column()) {
                case Name: return QString::fromStdString(n->name).toLower();
                case Size:
                case Share: return static_cast<qulonglong>(n->size);
                case Items: return static_cast<qulonglong>(n->files);
                case Modified: return static_cast<qlonglong>(n->mtime);
            }
            break;
        case ShareRole: return share;
        case NodeRole: return QVariant::fromValue(static_cast<void*>(n));
    }
    return {};
}

QVariant ScanModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) return {};
    if (role == Qt::TextAlignmentRole && (section == Size || section == Items))
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole) return {};
    switch (section) {
        case Name: return tr("Name");
        case Size: return tr("Size");
        case Share: return tr("% of Parent");
        case Items: return tr("Files");
        case Modified: return tr("Modified");
    }
    return {};
}
