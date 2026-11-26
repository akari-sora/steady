#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QSet>
#include <functional>

// Same-user control channel. The lock serializes startup and stale-socket cleanup.
class LocalControl : public QObject {
public:
    explicit LocalControl(const QString &scope);
    LocalControl(const QString &scope, const QString &directory);

    bool tryOwn() { return !m_name.isEmpty() && m_lock.tryLock(); }
    bool mayHaveOwner() const { return !m_name.isEmpty() && m_lock.error() == QLockFile::LockFailedError; }
    enum class Reply { Unavailable, Accepted, Failed, NoSource };
    bool listen(const QByteArray &commands, const std::function<Reply(char)> &handle);
    Reply send(char command);
    QString serverName() const { return m_name; }
    qsizetype activeClientCount() const { return m_clients.size(); }

private:
    QString m_directory;
    QString m_name;
    QLockFile m_lock;
    QSet<QLocalSocket *> m_clients;
    QLocalServer m_server;
};
