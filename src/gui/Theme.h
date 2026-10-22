#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include "storage/core/category.hpp"

namespace theme {

enum class Mode { System, Light, Dark };

void apply(Mode mode);
Mode current();
bool isDark();

QString modeName(Mode m);
Mode modeFromName(const QString& s);

QColor accent();
QColor categoryColor(storage::Category c);
QColor folderColor(int depth);
QColor mutedText();

// The application icon, drawn in code so no image plugins are needed.
QIcon appIcon();

}  // namespace theme
