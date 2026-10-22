#pragma once

#include <QAbstractItemModel>
#include <QFileIconProvider>

#include "storage/core/node.hpp"

// Read-only Qt model over a finalized ScanTree. Rows mirror Node::children.
class ScanModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Share, Items, Modified, ColumnCount };
    enum Role { SortRole = Qt::UserRole + 1, ShareRole, NodeRole };

    explicit ScanModel(QObject* parent = nullptr);

    void setTree(storage::ScanTree* tree);
    storage::ScanTree* tree() const { return tree_; }

    storage::Node* nodeAt(const QModelIndex& index) const;
    QModelIndex indexFor(storage::Node* node, int column = 0) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    storage::ScanTree* tree_ = nullptr;
    QFileIconProvider icons_;
    QIcon folderIcon_, fileIcon_, linkIcon_;
};
