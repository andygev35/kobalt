#pragma once
#include "check.h"
#include <QMap>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <functional>

namespace kobalt::test {
struct Request {
    QByteArray method, path, body;
    QMap<QByteArray, QByteArray> headers;
};
// A one-purpose HTTP server. `respond` returns a complete response, or an empty
// array to leave the request unanswered.
class Server : public QObject {
public:
    Server() {
        require(listener.listen(QHostAddress::LocalHost, 0), "Could not listen");
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = listener.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    buffers[socket].append(socket->readAll());
                    handle(socket);
                });
            }
        });
    }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(listener.serverPort()));
    }
    std::function<QByteArray(const Request &)> respond;
    QList<Request> requests;

private:
    QTcpServer listener;
    QMap<QTcpSocket *, QByteArray> buffers;
    void handle(QTcpSocket *socket) {
        const auto &data = buffers[socket];
        const int end = data.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        Request request;
        const auto lines = data.left(end).split('\n');
        const auto start = lines.value(0).trimmed().split(' ');
        request.method = start.value(0);
        request.path = start.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon > 0)
                request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const int length = request.headers.value("content-length", "0").toInt();
        if (data.size() < end + 4 + length)
            return;
        request.body = data.mid(end + 4, length);
        buffers.remove(socket);
        requests.append(request);
        const auto reply = respond ? respond(request) : QByteArray();
        if (reply.isEmpty())
            return;
        socket->write(reply);
        socket->flush();
        socket->disconnectFromHost();
    }
};
inline QByteArray http(int status, const QByteArray &body = {}) {
    return "HTTP/1.1 " + QByteArray::number(status) +
           " X\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}
}
