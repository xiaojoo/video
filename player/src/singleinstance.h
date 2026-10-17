#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

// One window per user, not one per launch. A second start passes its file (or its
// page request) to the window that is already up and exits, instead of leaving a
// second pair of host+chrome windows on the desktop that look like a duplicate
// player.
class SingleInstance final : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr);

    // False means "someone else is already the window; this process must exit".
    // allowMulti is the dev escape hatch: probes and --scan-only runs start their
    // own window on purpose.
    bool start(bool allowMulti, const QString &path, const QString &page);

signals:
    void requested(QString path, QString page);

private:
    QString m_pendingPath;
    QString m_pendingPage;
    QLocalServer *m_server = nullptr;
};
