/***************************************************************************
 * 
 * Copyright (c) 2011 Baidu.com, Inc. All Rights Reserved
 * $Id$ 
 * 
 **************************************************************************/
 
 
 
/**
 * ssps.cc ~ 2011/03/28 23:24:34
 * @author leeight(liyubei@baidu.com)
 * @version $Revision$ 
 * @description 
 * server side page speed
 **/

#include <stdio.h>

#include <fstream>
#include <vector>

#include "base/at_exit.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/stl_util-inl.h"
#include "base/string_util.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "pagespeed/core/engine.h"
#include "pagespeed/core/pagespeed_init.h"
#include "pagespeed/core/pagespeed_input.h"
#include "pagespeed/core/resource.h"
#include "pagespeed/formatters/json_formatter.h"
#include "pagespeed/formatters/proto_formatter.h"
#include "pagespeed/formatters/text_formatter.h"
#include "pagespeed/har/http_archive.h"
#include "pagespeed/image_compression/image_attributes_factory.h"
#include "pagespeed/proto/pagespeed_output.pb.h"
#include "pagespeed/rules/rule_provider.h"

#include "ssps.h"
#include "ssps.moc"

namespace {

NetworkReplyProxy::NetworkReplyProxy(QObject* parent, QNetworkReply* reply)
    : QNetworkReply(parent), 
      m_reply(reply) {

    qWarning("Starting network job: %p %s", this, qPrintable(m_reply->url().toString()));
    // apply attributes...
    setOperation(m_reply->operation());
    setRequest(m_reply->request());
    setUrl(m_reply->url());

    // handle these to forward
    connect(m_reply, SIGNAL(metaDataChanged()), SLOT(applyMetaData()));
    connect(m_reply, SIGNAL(readyRead()), SLOT(readInternal()));
    connect(m_reply, SIGNAL(error(QNetworkReply::NetworkError)), SLOT(errorInternal(QNetworkReply::NetworkError)));

    // forward signals
    connect(m_reply, SIGNAL(finished()), SIGNAL(finished()));
    connect(m_reply, SIGNAL(uploadProgress(qint64,qint64)), SIGNAL(uploadProgress(qint64,qint64)));
    connect(m_reply, SIGNAL(downloadProgress(qint64,qint64)), SIGNAL(downloadProgress(qint64,qint64)));

    // for the data proxy...
    setOpenMode(ReadOnly);
}

void NetworkReplyProxy::finishReply() {
    emit finished();
}

const char * NetworkReplyProxy::requestMethod() {
  switch(m_reply->operation()) {
    case QNetworkAccessManager::HeadOperation:
      return "HEAD";
    case QNetworkAccessManager::GetOperation:
      return "GET";
    case QNetworkAccessManager::PutOperation:
      return "PUT";
    case QNetworkAccessManager::PostOperation:
      return "POST";
    default:
      return "?";
  }
}

const QByteArray& NetworkReplyProxy::data() {
    return m_data;
}

NetworkReplyProxy::~NetworkReplyProxy() {
    if (m_reply->url().scheme() != "data")
        writeData();
    delete m_reply;
}

qint64 NetworkReplyProxy::readData(char* data, qint64 maxlen) {
    qint64 size = qMin(maxlen, qint64(m_buffer.size()));
    memcpy(data, m_buffer.constData(), size);
    m_buffer.remove(0, size);
    return size;
}

void NetworkReplyProxy::applyMetaData() {
    QList<QByteArray> headers = m_reply->rawHeaderList();
    foreach(QByteArray header, headers)
        setRawHeader(header, m_reply->rawHeader(header));

    setHeader(QNetworkRequest::ContentTypeHeader, m_reply->header(QNetworkRequest::ContentTypeHeader));
    setHeader(QNetworkRequest::ContentLengthHeader, m_reply->header(QNetworkRequest::ContentLengthHeader));
    setHeader(QNetworkRequest::LocationHeader, m_reply->header(QNetworkRequest::LocationHeader));
    setHeader(QNetworkRequest::LastModifiedHeader, m_reply->header(QNetworkRequest::LastModifiedHeader));
    setHeader(QNetworkRequest::SetCookieHeader, m_reply->header(QNetworkRequest::SetCookieHeader));

    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute));
    setAttribute(QNetworkRequest::HttpReasonPhraseAttribute, m_reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute));
    setAttribute(QNetworkRequest::RedirectionTargetAttribute, m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute));
    setAttribute(QNetworkRequest::ConnectionEncryptedAttribute, m_reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute));
    setAttribute(QNetworkRequest::CacheLoadControlAttribute, m_reply->attribute(QNetworkRequest::CacheLoadControlAttribute));
    setAttribute(QNetworkRequest::CacheSaveControlAttribute, m_reply->attribute(QNetworkRequest::CacheSaveControlAttribute));
    setAttribute(QNetworkRequest::SourceIsFromCacheAttribute, m_reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute));
    setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, m_reply->attribute(QNetworkRequest::DoNotBufferUploadDataAttribute));
    emit metaDataChanged();
}

void NetworkReplyProxy::errorInternal(QNetworkReply::NetworkError _error) {
    setError(_error, errorString());
    emit error(_error);
}

void NetworkReplyProxy::readInternal() {
    QByteArray data = m_reply->readAll();
    m_data += data;
    m_buffer += data;
    emit readyRead();
}

void NetworkReplyProxy::writeData() {
    QByteArray httpHeader;
    QList<QByteArray> headers = rawHeaderList();
    foreach(QByteArray header, headers) {
        if (header.toLower() == "content-encoding"
            || header.toLower() == "transfer-encoding"
            || header.toLower() == "content-length"
            || header.toLower() == "connection")
            continue;

        // special case for cookies.... we need to generate separate lines
        // QNetworkCookie::toRawForm is a bit broken and we have to do this
        // ourselves... some simple heuristic here..
        if (header.toLower() == "set-cookie") {
            QList<QNetworkCookie> cookies = QNetworkCookie::parseCookies(rawHeader(header));
            foreach (QNetworkCookie cookie, cookies) {
                httpHeader += "set-cookie: " + cookie.toRawForm() + "\r\n";
            }
        } else {
            httpHeader += header + ": " + rawHeader(header) + "\r\n";
        }
    }
    httpHeader += "content-length: " + QByteArray::number(m_data.size()) + "\r\n";
    httpHeader += "\r\n";

    if(m_reply->error() != QNetworkReply::NoError) {
        qWarning() << "\tError with: " << this << url() << error();
        return;
    }

    const QByteArray origUrl = m_reply->url().toEncoded();
    const QByteArray strippedUrl = m_reply->url().toEncoded(QUrl::RemoveFragment | QUrl::RemoveQuery);
    NetworkReplyProxy::writeData(origUrl, m_data, httpHeader, operation(), attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    if (origUrl != strippedUrl) {
        NetworkReplyProxy::writeData(strippedUrl, m_data, httpHeader, operation(), attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    }
}

void NetworkReplyProxy::writeData(const QByteArray& url, 
                                  const QByteArray& data, 
                                  const QByteArray& header, 
                                  int operation, 
                                  int response) {
    // IGNORE
}

Tracker::Tracker(QObject *parent)
    : QNetworkAccessManager(parent), 
      m_replyCounter(1), 
      m_sequence(1) {

  m_ticker.start();
  pagespeed::Init();
}

Tracker::~Tracker() {
  QTimer::singleShot(0, QApplication::instance(), SLOT(quit()));
}

QNetworkReply* Tracker::createRequest(Operation op, 
                                      const QNetworkRequest &request,
                                      QIODevice *outgoingData) {
  QNetworkReply *reply = QNetworkAccessManager::createRequest(op, request, outgoingData);
  NetworkReplyProxy *proxy = new NetworkReplyProxy(this, reply);
  connect(proxy, SIGNAL(finished()), SLOT(finishReply()));
  return proxy;
}

pagespeed::Resource* Tracker::toPageSpeedResource(NetworkReplyProxy *reply) {
  const QNetworkRequest& request = reply->request();
  int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  pagespeed::Resource *r = new pagespeed::Resource();
  r->SetRequestUrl(qPrintable(reply->url().toString()));
  r->SetRequestMethod(reply->requestMethod());
  r->SetResponseStatusCode(status);

  const QList<QByteArray>& rawReqHeaderList = request.rawHeaderList();
  for(int i = 0; i < rawReqHeaderList.size(); i ++) {
    r->AddRequestHeader(rawReqHeaderList[i].data(),
                        request.rawHeader(rawReqHeaderList[i]).data());
  }

  const QList<QByteArray>& rawResHeaderList = reply->rawHeaderList();
  for(int i = 0; i < rawResHeaderList.size(); i ++) {
    r->AddResponseHeader(rawResHeaderList[i].data(),
                         reply->rawHeader(rawResHeaderList[i]).data());
  }

  r->SetResponseBody(reply->data().data());

  // TODO
  // r->SetCookies();
  // r->SetLazyLoaded();
  // r->SetResourceType();

  return r;
}

void Tracker::finishReply() {
  NetworkReplyProxy *reply = qobject_cast<NetworkReplyProxy*>(sender());
#if 0
  qDebug() << "finishReply = [" << reply->data().size() << "]";
  qDebug() << "reply.url = [" << reply->url().toString() << "]";
  qDebug() << reply->data();
#endif
  m_input.AddResource(toPageSpeedResource(reply));
}

void Tracker::finalize() {
  // init
  if (m_input.primary_resource_url().empty() && m_input.num_resources() > 0) {
    m_input.SetPrimaryResourceUrl(m_input.GetResource(0).GetRequestUrl());
  }
  m_input.AcquireImageAttributesFactory(
    new pagespeed::image_compression::ImageAttributesFactory());
  m_input.Freeze();

  // run
  std::vector<pagespeed::Rule*> rules;
  bool save_optimized_content = true;
  pagespeed::rule_provider::AppendAllRules(save_optimized_content, &rules);
  pagespeed::formatters::TextFormatter formatter(&std::cout);
  pagespeed::Engine engine(&rules);
  engine.Init();
  engine.ComputeAndFormatResults(m_input, &formatter);
  
  // reset
  pagespeed::ShutDown();

  // extra delay for potential last-minute async requests
  QTimer::singleShot(3000, this, SLOT(deleteLater()));
}


}  // namespace

int main(int argc, char** argv) {
  // Some of our code uses Singleton<>s, which require an
  // AtExitManager to schedule their destruction.
  base::AtExitManager at_exit_manager;

  if (argc < 2) {
      std::cout << "tracenet URL" << std::endl << std::endl;
      return 0;
  }

#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  QUrl url = QUrl::fromUserInput(argv[1]);
#else
  QUrl url = QString::fromLatin1(argv[1]);
  if (!url.isValid())
      url = url.toString().prepend("http://");
#endif
  if (!url.isValid()) {
      std::cerr << "Invalid URL: " << argv[1] << std::endl << std::endl;
      return 0;
  }

  QApplication app(argc, argv);

  QWebSettings * settings = QWebSettings::globalSettings();
  settings->setAttribute(QWebSettings::AutoLoadImages, false);
  settings->setAttribute(QWebSettings::JavascriptEnabled, true);
  settings->setAttribute(QWebSettings::JavaEnabled, false);
  settings->setAttribute(QWebSettings::PluginsEnabled, false);
  settings->setAttribute(QWebSettings::PrivateBrowsingEnabled, false);

  Tracker *tracker = new Tracker;
  WebPage page;
  page.setNetworkAccessManager(tracker);
  QObject::connect(&page, SIGNAL(loadFinished(bool)), tracker, SLOT(finalize()));
  page.mainFrame()->load(url);

  return app.exec();
}



















/* vim: set ts=4 sw=4 sts=4 tw=100 noet: */
