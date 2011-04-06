/***************************************************************************
 * 
 * Copyright (c) 2011 Baidu.com, Inc. All Rights Reserved
 * $Id$ 
 * 
 **************************************************************************/
 
 
 
/**
 * ssps.h ~ 2011/03/28 23:26:13
 * @author leeight(liyubei@baidu.com)
 * @version $Revision$ 
 * @description 
 *  
 **/



#ifndef  __SSPS_H_
#define  __SSPS_H_


#include <QtGui>
#include <QtNetwork>
#include <QtWebKit>

#include <iostream>

#include "pagespeed/core/pagespeed_input.h"
#include "pagespeed/core/resource.h"

namespace {
class WebPage : public QWebPage {
    Q_OBJECT
public Q_SLOTS:
    bool shouldInterruptJavaScript() {
        return true;
    }

public:
    void javaScriptAlert(QWebFrame *, const QString& msg) {
        std::cout << "javaScriptAlert:msg = [" << msg.toStdString()  << "]" << std::endl;
    }

    bool javaScriptConfirm(QWebFrame *, const QString& msg) {
        std::cout << "javaScriptConfirm:msg = [" << msg.toStdString() << "]" << std::endl;
        return false;
    }

    bool javaScriptPrompt(QWebFrame *, const QString& msg, const QString& defaultValue, QString* ) {
        std::cout << "javaScriptPrompt:msg = [" << msg.toStdString() << "], defaultValue = ["
                  << defaultValue.toStdString() << "]" << std::endl;
        return false;
    }

    void javaScriptConsoleMessage(const QString& message, int lineNumber, const QString& sourceID) {
        std::cout << "javaScriptConsoleMessage:message = [" << message.toStdString() 
                  << "], lineNumber = [" << lineNumber << "], sourceID = [" 
                  << sourceID.toStdString() << "]" << std::endl;
    }
};

// NetworkReplyProxy
class NetworkReplyProxy : public QNetworkReply {
    Q_OBJECT

public:
    NetworkReplyProxy(QObject* parent, QNetworkReply* reply);
    virtual ~NetworkReplyProxy();

    // virtual  methids
    void abort() { m_reply->abort(); }
    void close() { m_reply->close(); }
    bool isSequential() const { return m_reply->isSequential(); }

    // not possible...
    void setReadBufferSize(qint64 size) { QNetworkReply::setReadBufferSize(size); m_reply->setReadBufferSize(size); }

    // ssl magic is not done....
    // isFinished()/isRunning can not be done *sigh*

    // QIODevice proxy...
    virtual qint64 bytesAvailable() const
    {
        return m_buffer.size() + QIODevice::bytesAvailable();
    }

    virtual qint64 bytesToWrite() const { return -1; }
    virtual bool canReadLine() const { qFatal("not implemented"); return false; }

    virtual bool waitForReadyRead(int) { qFatal("not implemented"); return false; }
    virtual bool waitForBytesWritten(int) { qFatal("not implemented"); return false; }

    virtual qint64 readData(char* data, qint64 maxlen);
    const QByteArray& data();
    const char * requestMethod();

public Q_SLOTS:
    void ignoreSslErrors() { m_reply->ignoreSslErrors(); }
    void applyMetaData();
    void errorInternal(QNetworkReply::NetworkError _error);
    void readInternal();
    void finishReply();

private:
    void writeData();
    static void writeData(const QByteArray& url, const QByteArray& data, const QByteArray& header, int operation, int response);

    QNetworkReply* m_reply;
    QByteArray m_data;
    QByteArray m_buffer;
};

// Tracker
class Tracker: public QNetworkAccessManager {
    Q_OBJECT

private:
    int m_replyCounter;
    QHash<QNetworkReply*, int> m_replyHash;
    QHash<QNetworkReply*, const char *> m_requestMethod;
    QHash<QNetworkReply*, QString> m_statusTextHash;
    QTime m_ticker;
    pagespeed::PagespeedInput m_input;
    int m_sequence;

public:
    Tracker(QObject *parent = 0);
    virtual ~Tracker();

protected:
    // NETWORK_RESOURCE_START
    virtual QNetworkReply *createRequest(Operation op, 
                                         const QNetworkRequest &request,
                                         QIODevice *outgoingData);
public slots:
    void finalize();

private slots:
    // NETWORK_RESOURCE_FINISH
    void finishReply();

private:
    // toPageSpeedResource
    pagespeed::Resource* toPageSpeedResource(NetworkReplyProxy *proxy);
};

} // end namespace












#endif  //__SSPS_H_

/* vim: set ts=4 sw=4 sts=4 tw=100 noet: */
