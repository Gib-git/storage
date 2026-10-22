#pragma once

#include <QWidget>

#include "storage/core/node.hpp"

class QHBoxLayout;

// Clickable path from the scan root down to the folder being viewed.
class BreadcrumbBar : public QWidget {
    Q_OBJECT
public:
    explicit BreadcrumbBar(QWidget* parent = nullptr);
    void setNode(storage::Node* node);

signals:
    void navigate(storage::Node* node);

private:
    QHBoxLayout* layout_;
};
