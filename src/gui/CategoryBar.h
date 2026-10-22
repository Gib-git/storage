#pragma once

#include <QWidget>

#include <array>

#include "storage/core/category.hpp"
#include "storage/core/node.hpp"

// Stacked bar + legend showing how a folder's space splits by file type.
class CategoryBar : public QWidget {
    Q_OBJECT
public:
    explicit CategoryBar(QWidget* parent = nullptr);
    void setNode(const storage::Node* node);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    static constexpr int kCount = static_cast<int>(storage::Category::Other) + 1;
    std::array<uint64_t, kCount> bytes_{};
    uint64_t total_ = 0;
};
