#pragma once

#include <QStringList>

namespace AppPaths {
QString cacheDirectory();
QString dataDirectory();
QString configDirectory();
QStringList apkCandidates(const QString &executableDirectory, const QString &userDataDirectory);
}
