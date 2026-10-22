#pragma once

#include <QStyledItemDelegate>

// Draws the "% of parent" column as a slim proportional bar with a label.
class ShareDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
