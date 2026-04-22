/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include <qt/IntegratorFilePicker.hpp>

#include <common/Log.hpp>
#include <common/MobileApp.hpp>
#include <net/FakeSocket.hpp>
#include <qt/bridge.hpp>
#include <qt/qt.hpp>
#include <qt/RemoteOpen.hpp>
#include <qt/WebView.hpp>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

/// Custom page that intercepts iframe navigations to detect the COOL
/// URL that the integrator constructs when opening a document.
class InterceptPage : public QWebEnginePage
{
public:
    IntegratorFilePicker* picker;

    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(
        const QUrl& url, NavigationType type, bool isMainFrame) override
    {
        QUrlQuery q(url);
        // Once a Bridge has been attached, the picker's webview is
        // the document editor; subsequent WOPISrc navigations (e.g.
        // the one switchToServerMode triggers when the local editor
        // hands off to server-mode collab) must pass through, not
        // get reinterpreted as a fresh document open.
        if (q.hasQueryItem("WOPISrc") && picker->_bridge == nullptr)
        {
            if (picker->_embedPort != 0)
            {
                // URL is already pointing at our local HTTP server
                // (second acceptNavigationRequest after our defer):
                // let it proceed so cool.html loads in the picker.
                if (url.host() == "localhost"
                    && url.port() == picker->_embedPort)
                    return true;

                // Extract WOPI params.  If the iframe URL did not
                // carry the access_token (e.g. NC richdocuments
                // leaves it in a POST form), fall back to the same
                // integrator-specific DOM scrape used by the non-
                // embed flow.  Defer the actual download + Bridge
                // attach: we must not run a nested event loop
                // inside acceptNavigationRequest.
                IntegratorFilePicker* p = picker;
                QString wopiSrc = q.queryItemValue(
                    "WOPISrc", QUrl::FullyDecoded);
                QString accessToken = q.queryItemValue(
                    "access_token", QUrl::FullyDecoded);
                QString coolServer = url.scheme() + "://" + url.host()
                    + (url.port(-1) != -1
                        ? ":" + QString::number(url.port())
                        : QString());
                QString coolPath = url.path();
                QUrlQuery origQuery(url);
                auto proceed =
                    [p, wopiSrc, coolServer, coolPath, origQuery]
                    (const QString& token) {
                        QMetaObject::invokeMethod(p,
                            [p, wopiSrc, token, coolServer, coolPath,
                             origQuery]() {
                                p->attachEmbeddedDocument(
                                    wopiSrc, token, coolServer,
                                    coolPath, origQuery);
                            }, Qt::QueuedConnection);
                    };
                if (accessToken.isEmpty())
                    picker->extractAccessTokenAsync(std::move(proceed));
                else
                    proceed(accessToken);
                return false;
            }

            LOG_TRC("IntegratorFilePicker: intercepted COOL URL: "
                    << url.toString().toStdString());

            picker->_wopiSrc = q.queryItemValue(
                "WOPISrc", QUrl::FullyDecoded);
            picker->_accessToken = q.queryItemValue(
                "access_token", QUrl::FullyDecoded);
            picker->_coolServer = url.scheme() + "://"
                + url.host()
                + (url.port(-1) != -1
                       ? ":" + QString::number(url.port())
                       : QString());
            picker->_coolPath = url.path();

            if (picker->_accessToken.isEmpty())
            {
                // Token not in the URL - let the integrator-specific
                // subclass extract it.
                picker->extractAccessToken();
            }
            else
            {
                picker->accept();
            }
            return false;
        }

        return QWebEnginePage::acceptNavigationRequest(
            url, type, isMainFrame);
    }

    QWebEnginePage* createWindow(WebWindowType) override
    {
        return this;
    }
};

// --- IntegratorFilePicker (generic) ---

namespace
{
/// CODA_EMBED_IFRAME=1 switches on the POC for rendering the
/// integrator's document iframe in-place (instead of bouncing to a
/// separate CODA window).
bool isEmbedMode()
{
    return qEnvironmentVariableIsSet("CODA_EMBED_IFRAME");
}

/// Trivial static-file server on 127.0.0.1:<ephemeral>.  Serves
/// browser/dist/ so cool.html and its assets can be reached under an
/// origin the integrator's `frame-src` allowlist admits (see the
/// `http://localhost:*` hack in nextcloud-cool-test-env.sh).  GET
/// only, no keep-alive, no thread pool - fine for the POC.
class EmbedHttpServer : public QObject
{
public:
    EmbedHttpServer(const QString& rootDir, QObject* parent)
        : QObject(parent)
        , _root(QDir(rootDir).absolutePath())
        , _server(new QTcpServer(this))
    {
        QObject::connect(_server, &QTcpServer::newConnection,
                         this, [this] { onNewConnection(); });
    }

    bool listen() { return _server->listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return _server->serverPort(); }

private:
    void onNewConnection()
    {
        while (QTcpSocket* client = _server->nextPendingConnection())
        {
            QObject::connect(client, &QTcpSocket::readyRead,
                             client, [this, client] { serve(client); });
            QObject::connect(client, &QTcpSocket::disconnected,
                             client, &QObject::deleteLater);
        }
    }

    void serve(QTcpSocket* client)
    {
        QByteArray buf = client->peek(client->bytesAvailable());
        int sep = buf.indexOf("\r\n\r\n");
        if (sep < 0)
            return;
        client->read(sep + 4);

        int nl = buf.indexOf("\r\n");
        QList<QByteArray> parts = buf.left(nl).split(' ');
        if (parts.size() < 2 || parts[0] != "GET")
        {
            sendError(client, 405, "Method Not Allowed");
            return;
        }

        QByteArray rawPath = parts[1];
        int q = rawPath.indexOf('?');
        if (q >= 0)
            rawPath = rawPath.left(q);
        QString path = QUrl::fromPercentEncoding(rawPath);

        QFileInfo fi(_root + path);
        QString canonical = fi.canonicalFilePath();
        if (canonical.isEmpty() || !canonical.startsWith(_root))
        {
            LOG_WRN("EmbedHttpServer: 404 for " << path.toStdString()
                    << " (resolved: " << canonical.toStdString() << ')');
            sendError(client, 404, "Not Found");
            return;
        }

        QFile f(canonical);
        if (!f.open(QIODevice::ReadOnly))
        {
            LOG_WRN("EmbedHttpServer: 404 opening "
                    << canonical.toStdString());
            sendError(client, 404, "Not Found");
            return;
        }
        LOG_TRC("EmbedHttpServer: 200 " << path.toStdString()
                << " -> " << canonical.toStdString());

        QMimeDatabase db;
        QByteArray mime = db.mimeTypeForFile(canonical).name().toUtf8();
        QByteArray body = f.readAll();
        QByteArray resp;
        resp += "HTTP/1.1 200 OK\r\n";
        resp += "Content-Type: " + mime + "\r\n";
        resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        resp += body;
        client->write(resp);
        client->disconnectFromHost();
    }

    void sendError(QTcpSocket* client, int code, const char* text)
    {
        QByteArray resp = "HTTP/1.1 " + QByteArray::number(code) + ' '
                          + text + "\r\nContent-Length: 0\r\n"
                            "Connection: close\r\n\r\n";
        client->write(resp);
        client->disconnectFromHost();
    }

    QString _root;
    QTcpServer* _server;
};

/// Attaches an X-Collab-Frame-Origin header to every outgoing
/// request from the picker page, carrying the origin of our local
/// HTTP server (http://localhost:<embed-port>).  The integrator's
/// CSP listener is expected to read it and add that single origin
/// to frame-src, so the integrator does not have to allowlist all
/// of localhost:*.  Piggybacks on the normal request flow so no
/// extra endpoint or handshake is needed.
class FrameOriginInterceptor
    : public QWebEngineUrlRequestInterceptor
{
public:
    FrameOriginInterceptor(quint16 port, QObject* parent)
        : QWebEngineUrlRequestInterceptor(parent)
        , _headerValue(
            ("http://localhost:" + QString::number(port)).toUtf8())
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        info.setHttpHeader("X-Collab-Frame-Origin", _headerValue);
    }

private:
    QByteArray _headerValue;
};

/// Probe serverUrl and, when the integrator is recognized, return a
/// URL that drops the user straight onto the files view instead of
/// the default landing page.  Returns serverUrl unchanged otherwise.
QString resolveLandingUrl(const QString& serverUrl)
{
    QUrl base(serverUrl);
    QUrl probe = base;
    QString probePath = base.path();
    if (!probePath.endsWith('/'))
        probePath += '/';
    probe.setPath(probePath + "status.php");

    QNetworkAccessManager nam;
    QEventLoop loop;
    QNetworkRequest req(probe);
    QNetworkReply* reply = nam.get(req);
    QObject::connect(reply, &QNetworkReply::finished,
                     &loop, &QEventLoop::quit);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();

    QString result = serverUrl;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError)
    {
        QJsonDocument jdoc = QJsonDocument::fromJson(reply->readAll());
        if (jdoc.object()["productname"].toString() == "Nextcloud")
        {
            QUrl u = base;
            u.setPath(probePath + "apps/files/files");
            result = u.toString();
            LOG_TRC("IntegratorFilePicker: detected Nextcloud, "
                    "landing on " << result.toStdString());
        }
    }
    reply->deleteLater();
    return result;
}
}

IntegratorFilePicker::IntegratorFilePicker(const QString& serverUrl,
                                           QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Open Remote Document");
    resize(800, 900);

    auto* layout = new QVBoxLayout(this);
    _webView = new QWebEngineView;

    auto* page = new InterceptPage(_webView);
    page->picker = this;
    _webView->setPage(page);

    layout->addWidget(_webView);
    layout->setContentsMargins(0, 0, 0, 0);

    if (isEmbedMode())
    {
        QString distRoot = QString::fromStdString(getDataDir())
                         + "/browser/dist";
        auto* httpServer = new EmbedHttpServer(distRoot, this);
        if (httpServer->listen())
        {
            _embedPort = httpServer->port();
            LOG_INF("IntegratorFilePicker: embed mode on, serving "
                    << distRoot.toStdString() << " at "
                    << "http://localhost:" << _embedPort);
            // Advertise the port to the integrator via a custom
            // request header on every outgoing request, so its CSP
            // listener can whitelist exactly our origin in frame-src
            // instead of all of localhost:*.
            page->setUrlRequestInterceptor(
                new FrameOriginInterceptor(_embedPort, this));
        }
        else
        {
            LOG_WRN("IntegratorFilePicker: embed mode requested but "
                    "HTTP server failed to listen");
        }
        // The QWebChannel + real Bridge are installed in
        // attachEmbeddedDocument(), once we have a DocumentData.
    }

    _webView->load(QUrl(resolveLandingUrl(serverUrl)));
}

void IntegratorFilePicker::attachEmbeddedDocument(
    const QString& wopiSrc, const QString& accessToken,
    const QString& coolServer, const QString& coolPath,
    QUrlQuery origQuery)
{
    LOG_TRC("IntegratorFilePicker::attachEmbeddedDocument: "
            << "wopiSrc=" << wopiSrc.toStdString()
            << " coolServer=" << coolServer.toStdString());

    coda::RemoteDownload dl = coda::downloadRemoteDocument(
        wopiSrc, accessToken, coolServer, coolPath);
    if (dl.localPath.isEmpty())
    {
        LOG_WRN("IntegratorFilePicker: document download failed");
        return;
    }

    _document = {
        ._fileURL = Poco::URI(
            Poco::Path(dl.localPath.toStdString())),
        ._fakeClientFd = fakeSocketSocket(),
        ._appDocId = coda::generateNewAppDocId(),
        ._remoteInfo = std::move(dl.remoteInfo),
    };

    _bridge = coda::attachRemoteBridge(
        _webView->page(), _document, this, _webView);
    coda::wireCollabMessagesToBridge(
        _bridge, _document._remoteInfo.get());

    // Build the URL we rewrite the iframe to: our local HTTP server's
    // /cool.html with CODA-local params added.  Preserve the UI
    // hints (lang, closebutton, revisionhistory) from the original
    // NC iframe URL, but drop WOPISrc and access_token: their
    // presence on the URL is what makes main.js set isWopi=true and
    // the JS render the integrator-iframe minimal chrome (no top
    // menu bar, smaller toolbar) instead of the local-edit chrome
    // we want here.  Both values are kept separately on
    // _document._remoteInfo for switchToServerMode to rebuild the
    // server-mode URL later.
    QUrlQuery embedQuery(origQuery);
    embedQuery.removeAllQueryItems("WOPISrc");
    embedQuery.removeAllQueryItems("access_token");
    QUrl target;
    target.setScheme("http");
    target.setHost("localhost");
    target.setPort(_embedPort);
    target.setPath("/cool.html");
    target.setQuery(embedQuery);
    coda::addRemoteCoolParams(target, _document);

    LOG_TRC("IntegratorFilePicker: navigating picker to "
            << target.toString().toStdString());
    _webView->setUrl(target);

    // Repurpose the picker dialog as the document viewer: set the
    // window title to <filename> - APP_NAME and resize to match the
    // non-embed document window.
    Poco::Path uriPath(_document._fileURL.getPath());
    QString fileName = QString::fromStdString(uriPath.getFileName());
    setWindowTitle(fileName + " - " APP_NAME);
    auto size = coda::documentWindowSize(false);
    resize(size.first, size.second);
}

void IntegratorFilePicker::extractAccessTokenAsync(
    std::function<void(const QString&)> then)
{
    // Try known integrator-specific extraction strategies.  Each
    // returns a non-empty string on success or empty string on
    // failure.  The JS tries them all and returns the first hit.
    _webView->page()->runJavaScript(
        "(() => {"
        "  var token = '';"
        // Nextcloud richdocuments: POST form hidden input
        "  var form = document.querySelector("
        "    'form[target] input[name=access_token]');"
        "  if (form && form.value) return form.value;"
        // Nextcloud richdocuments: JS object
        "  if (window.OCA && window.OCA.RichDocuments"
        "      && window.OCA.RichDocuments.token)"
        "    return window.OCA.RichDocuments.token;"
        // Generic: any hidden input named access_token
        "  var input = document.querySelector("
        "    'input[name=access_token]');"
        "  if (input && input.value) return input.value;"
        // Add more integrator strategies here as needed.
        "  return '';"
        "})()",
        [then = std::move(then)](const QVariant& result) {
            then(result.toString());
        });
}

void IntegratorFilePicker::extractAccessToken()
{
    extractAccessTokenAsync([this](const QString& val) {
        if (!val.isEmpty())
            _accessToken = val;
        accept();
    });
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
