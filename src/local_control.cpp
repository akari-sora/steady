#include "local_control.h"
#include "app_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr int kMaxLocalClients = 8;

QString controlDirectory()
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty() && AppPaths::dataDirectory().isEmpty()) return {};
#ifdef Q_OS_WIN
    const QString directory = runtime.isEmpty() ? AppPaths::dataDirectory() : runtime;
    return QDir().mkpath(directory) ? directory : QString();
#else
    const auto isPrivate = [](const QString &directory) {
        struct stat info{};
        return lstat(QFile::encodeName(directory).constData(), &info) == 0 &&
            S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 0777) == 0700;
    };
    if (isPrivate(runtime)) return runtime;
    const QString data = AppPaths::dataDirectory();
    if (data.isEmpty() || !QDir().mkpath(data)) return {};
    const QString directory = QDir(data).filePath(QStringLiteral("runtime"));
    QDir().mkdir(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return isPrivate(directory) ? directory : QString();
#endif
}

QString controlName(const QString &directory, const QString &scope)
{
    if (directory.isEmpty()) return {};
#ifdef Q_OS_WIN
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(size, '\0');
    const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) return {};
    const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.constData());
    const QByteArray sid(static_cast<const char *>(user->User.Sid), GetLengthSid(user->User.Sid));
    const auto hash = QCryptographicHash::hash(sid, QCryptographicHash::Sha256).toHex().left(24);
    return QStringLiteral("steady-%1-%2").arg(QString::fromLatin1(hash), scope);
#else
    return QDir(directory).filePath(QStringLiteral("steady-") + scope);
#endif
}
}

LocalControl::LocalControl(const QString &scope)
    : LocalControl(scope, controlDirectory())
{
}

LocalControl::LocalControl(const QString &scope, const QString &directory)
    : m_directory(directory)
    , m_name(controlName(m_directory, scope))
    , m_lock(QDir(m_directory).filePath(QFileInfo(m_name).fileName() + QStringLiteral(".lock")))
{
    m_lock.setStaleLockTime(0);
}

bool LocalControl::listen(const QByteArray &commands, const std::function<Reply(char)> &handle)
{
    if (!m_lock.isLocked()) return false;
    QLocalServer::removeServer(m_name);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(kMaxLocalClients);
    if (!m_server.listen(m_name)) {
        m_lock.unlock();
        return false;
    }

    connect(&m_server, &QLocalServer::newConnection, this, [this, commands, handle]() {
        while (auto *socket = m_server.nextPendingConnection()) {
            if (m_clients.size() >= kMaxLocalClients || socket->state() != QLocalSocket::ConnectedState) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            m_clients.insert(socket);
            socket->setReadBufferSize(1);
            connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
                m_clients.remove(socket);
                socket->deleteLater();
            });
            QTimer::singleShot(500, socket, &QLocalSocket::abort);
            const auto read = [socket, commands, handle]() {
                char command;
                if (!socket->getChar(&command)) return;
                const auto reply = commands.contains(command) ? handle(command) : Reply::Failed;
                socket->putChar(reply == Reply::Accepted ? '1' : reply == Reply::NoSource ? '2' : '0');
                socket->flush();
                socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, socket, read);
            read();
        }
    });
    return true;
}

LocalControl::Reply LocalControl::send(char command)
{
    if (m_name.isEmpty()) return Reply::Unavailable;
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(200)) return Reply::Unavailable;
    if (!socket.putChar(command)) return Reply::Failed;
    socket.flush();
    if (!socket.bytesAvailable() && !socket.waitForReadyRead(3000)) return Reply::Failed;
    char response;
    if (!socket.getChar(&response)) return Reply::Failed;
    if (response == '1') return Reply::Accepted;
    return response == '2' ? Reply::NoSource : Reply::Failed;
}
