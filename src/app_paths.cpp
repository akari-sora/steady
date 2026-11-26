#include "app_paths.h"

#include <QDir>
#include <QStandardPaths>

QString AppPaths::cacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString AppPaths::dataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString AppPaths::configDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QStringList AppPaths::apkCandidates(const QString &executableDirectory, const QString &userDataDirectory)
{
    const QString apk = QStringLiteral("steady-sender.apk");
    const QDir executable(executableDirectory);
    QStringList paths = {
        executable.filePath(apk),
        QDir::cleanPath(executable.filePath(QStringLiteral("../share/steady/") + apk))
    };
    if (!userDataDirectory.isEmpty()) {
        paths.append(QDir(userDataDirectory).filePath(apk));
    }
#ifdef Q_OS_LINUX
    paths.append(QStringLiteral("/usr/share/steady/") + apk);
#endif
    return paths;
}
