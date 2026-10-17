#include "singleinstance.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QtGlobal>

namespace {
// Per user: Qt puts the named pipe in this session's namespace on Windows.
const char *kName = "Playerui.single";
}

SingleInstance::SingleInstance(QObject *parent) : QObject(parent)
{
}

bool SingleInstance::start(bool allowMulti, const QString &path, const QString &page)
{
    m_pendingPath = path;
    m_pendingPage = page;
    if (allowMulti)
        return true;

    QLocalSocket probe;
    probe.connectToServer(QLatin1String(kName));
    if (probe.waitForConnected(200)) {
        probe.write((path + QChar('\n') + page).toUtf8());
        probe.waitForBytesWritten(200);
        probe.disconnectFromServer();
        return false;
    }
    // Nothing was listening, so any name left behind belongs to a dead process.
    QLocalServer::removeServer(QLatin1String(kName));
    m_server = new QLocalServer(this);
    if (!m_server->listen(QLatin1String(kName))) {
        qWarning("SINGLE cannot listen: %s", qPrintable(m_server->errorString()));
        return true;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        QLocalSocket *s = m_server->nextPendingConnection();
        if (!s)
            return;
        connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        s->waitForReadyRead(150);
        const QByteArray msg = s->readAll();
        const int nl = msg.indexOf('\n');
        const QString p = nl < 0 ? QString::fromUtf8(msg) : QString::fromUtf8(msg.left(nl));
        const QString pg = nl < 0 ? QString() : QString::fromUtf8(msg.mid(nl + 1));
        s->disconnectFromServer();
        qInfo("SINGLE handed over open=%s page=%s", qPrintable(p), qPrintable(pg));
        emit requested(p, pg);
    });
    return true;
}
