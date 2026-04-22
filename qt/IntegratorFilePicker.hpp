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

#pragma once

#include <qt/Document.hpp>

#include <QDialog>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

class Bridge;
class QWebEngineView;

/// Shows an integrator's web UI in a QWebEngineView.  When the user
/// opens a document, the integrator creates an iframe pointing to the
/// COOL server; we intercept that navigation to extract the WOPI
/// parameters (WOPISrc, access_token, COOL server URL).
///
/// The WOPISrc and COOL server URL are extracted generically from the
/// iframe URL (all integrators put WOPISrc there).  The access_token
/// extraction is integrator-specific; when the token is not in the
/// URL, known extraction strategies are tried in order.
class IntegratorFilePicker : public QDialog
{
    Q_OBJECT
public:
    explicit IntegratorFilePicker(const QString& serverUrl,
                                  QWidget* parent = nullptr);

    QString wopiSrc() const { return _wopiSrc; }
    QString accessToken() const { return _accessToken; }
    QString coolServer() const { return _coolServer; }
    QString coolPath() const { return _coolPath; }

    // InterceptPage needs access to set these
    friend class InterceptPage;

private:
    void extractAccessToken();
    /// Run integrator-specific JS to locate the WOPI access_token in
    /// the current page DOM.  `then` is called with the extracted
    /// value (empty string on failure).
    void extractAccessTokenAsync(
        std::function<void(const QString&)> then);
    /// Embed-mode download-and-attach: fetches the document via
    /// /co/collab, populates _document, attaches a Bridge to the
    /// picker page, and navigates the picker to the local-server
    /// cool.html.  origQuery carries through the UI hints NC set on
    /// the intercepted iframe URL (lang, closebutton, ...).
    void attachEmbeddedDocument(const QString& wopiSrc,
                                const QString& accessToken,
                                const QString& coolServer,
                                const QString& coolPath,
                                QUrlQuery origQuery);

    QWebEngineView* _webView;
    QString _wopiSrc;
    QString _accessToken;
    QString _coolServer;
    QString _coolPath;
    /// Port of the embed-mode HTTP server (0 when embed mode is off).
    quint16 _embedPort = 0;
    /// Embed-mode: populated document + Bridge, owned by the picker
    /// for the lifetime of the dialog.
    coda::DocumentData _document;
    Bridge* _bridge = nullptr;
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
