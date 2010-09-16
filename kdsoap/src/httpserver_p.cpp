#include "httpserver_p.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QDomDocument>
#include <QEventLoop>

// Helper for xmlBufferCompare
static bool textBufferCompare(
    const QByteArray& source, const QByteArray& dest,  // for the qDebug only
    QIODevice& sourceFile, QIODevice& destFile)
{
    int lineNumber = 1;
    while (!sourceFile.atEnd()) {
        if (destFile.atEnd())
            return false;
        QByteArray sourceLine = sourceFile.readLine();
        QByteArray destLine = destFile.readLine();
        if (sourceLine != destLine) {
            sourceLine.chop(1); // remove '\n'
            destLine.chop(1); // remove '\n'
            qDebug() << source << "and" << dest << "differ at line" << lineNumber;
            qDebug("got     : %s", sourceLine.constData());
            qDebug("expected: %s", destLine.constData());
            return false;
        }
        ++lineNumber;
    }
    return true;
}

// A tool for comparing XML documents and outputting something useful if they differ
bool KDSoapUnitTestHelpers::xmlBufferCompare(const QByteArray& source, const QByteArray& dest)
{
    QBuffer sourceFile;
    sourceFile.setData(source);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        qDebug() << "ERROR opening QIODevice";
        return false;
    }
    QBuffer destFile;
    destFile.setData(dest);
    if (!destFile.open(QIODevice::ReadOnly)) {
        qDebug() << "ERROR opening QIODevice";
        return false;
    }

    // Use QDomDocument to reformat the XML with newlines
    QDomDocument sourceDoc;
    if (!sourceDoc.setContent(&sourceFile)) {
        qDebug() << "ERROR parsing XML:" << source;
        return false;
    }
    QDomDocument destDoc;
    if (!destDoc.setContent(&destFile)) {
        qDebug() << "ERROR parsing XML:" << dest;
        return false;
    }

    const QByteArray sourceXml = sourceDoc.toByteArray();
    const QByteArray destXml = destDoc.toByteArray();
    sourceFile.close();
    destFile.close();

    QBuffer sourceBuffer;
    sourceBuffer.setData(sourceXml);
    sourceBuffer.open(QIODevice::ReadOnly);
    QBuffer destBuffer;
    destBuffer.setData(destXml);
    destBuffer.open(QIODevice::ReadOnly);

    return textBufferCompare(source, dest, sourceBuffer, destBuffer);
}

void KDSoapUnitTestHelpers::httpGet(const QUrl& url)
{
    QNetworkRequest request(url);
    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.get(request);
    //reply->ignoreSslErrors();

    QEventLoop ev;
    QObject::connect(reply, SIGNAL(finished()), &ev, SLOT(quit()));
    ev.exec();

    //QObject::connect(reply, SIGNAL(finished()), &QTestEventLoop::instance(), SLOT(exitLoop()));
    //QTestEventLoop::instance().enterLoop(11);

    delete reply;
}

#ifndef QT_NO_OPENSSL
static void setupSslServer(QSslSocket* serverSocket)
{
    serverSocket->setProtocol(QSsl::AnyProtocol);
    serverSocket->setLocalCertificate(QString::fromLatin1("certs/qt-test-server-cacert.pem"));
    serverSocket->setPrivateKey(QString::fromLatin1("certs/server.key"));
}
#endif

// A blocking http server (must be used in a thread) which supports SSL.
class BlockingHttpServer : public QTcpServer
{
    Q_OBJECT
public:
    BlockingHttpServer(bool ssl) : doSsl(ssl), sslSocket(0) {}
    ~BlockingHttpServer() {}

    QTcpSocket* waitForNextConnectionSocket() {
        if (!waitForNewConnection(10000)) // 2000 would be enough, except in valgrind
            return 0;
        if (doSsl) {
            Q_ASSERT(sslSocket);
            return sslSocket;
        } else {
            //qDebug() << "returning nextPendingConnection";
            return nextPendingConnection();
        }
    }
    virtual void incomingConnection(int socketDescriptor)
    {
#ifndef QT_NO_OPENSSL
        if (doSsl) {
            QSslSocket *serverSocket = new QSslSocket;
            serverSocket->setParent(this);
            serverSocket->setSocketDescriptor(socketDescriptor);
            connect(serverSocket, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(slotSslErrors(QList<QSslError>)));
            setupSslServer(serverSocket);
            qDebug() << "Created QSslSocket, starting server encryption";
            // ### fails in QSslSocketBackendPrivate::startServerEncryption
            serverSocket->startServerEncryption();
            sslSocket = serverSocket;
        } else
#endif
            QTcpServer::incomingConnection(socketDescriptor);
    }
private slots:
#ifndef QT_NO_OPENSSL
    void slotSslErrors(const QList<QSslError>& errors)
    {
        qDebug() << "slotSslErrors" << sslSocket->errorString() << errors;
    }
#endif
private:
    const bool doSsl;
    QTcpSocket* sslSocket;
};

static bool splitHeadersAndData(const QByteArray& request, QByteArray& header, QByteArray& data)
{
    const int sep = request.indexOf("\r\n\r\n");
    if (sep <= 0)
        return false;
    header = request.left(sep);
    data = request.mid(sep + 4);
    return true;
}

typedef QMap<QByteArray, QByteArray> HeadersMap;
static HeadersMap parseHeaders(const QByteArray& headerData) {
    HeadersMap headersMap;
    QBuffer sourceBuffer;
    sourceBuffer.setData(headerData);
    sourceBuffer.open(QIODevice::ReadOnly);
    // The first line is special, it's the GET or POST line
    const QList<QByteArray> firstLine = sourceBuffer.readLine().split(' ');
    if (firstLine.count() < 3) {
        qDebug() << "Malformed HTTP request:" << firstLine;
        return headersMap;
    }
    const QByteArray request = firstLine[0];
    const QByteArray path = firstLine[1];
    const QByteArray httpVersion = firstLine[2];
    if (request != "GET" && request != "POST") {
        qDebug() << "Unknown HTTP request:" << firstLine;
        return headersMap;
    }
    headersMap.insert("_path", path);
    headersMap.insert("_httpVersion", httpVersion);

    while (!sourceBuffer.atEnd()) {
        const QByteArray line = sourceBuffer.readLine();
        const int pos = line.indexOf(':');
        if (pos == -1)
            qDebug() << "Malformed HTTP header:" << line;
        const QByteArray header = line.left(pos);
        const QByteArray value = line.mid(pos+1).trimmed(); // remove space before and \r\n after
        //qDebug() << "HEADER" << header << "VALUE" << value;
        headersMap.insert(header, value);
    }
    return headersMap;
}

void HttpServerThread::run()
{
    BlockingHttpServer server(m_features & Ssl);
    server.listen();
    m_port = server.serverPort();
    m_ready.release();

    const bool doDebug = qgetenv("KDSOAP_DEBUG").toInt();

    if (doDebug)
        qDebug() << "HttpServerThread listening on port" << m_port;

    // Wait for first connection (we'll wait for further ones inside the loop)
    QTcpSocket *clientSocket = server.waitForNextConnectionSocket();
    Q_ASSERT(clientSocket);

    Q_FOREVER {
        // get the "request" packet
        if (doDebug) {
            qDebug() << "HttpServerThread: waiting for read";
        }
        if (clientSocket->state() == QAbstractSocket::UnconnectedState ||
            !clientSocket->waitForReadyRead(2000)) {
            if (clientSocket->state() == QAbstractSocket::UnconnectedState) {
                delete clientSocket;
                if (doDebug) {
                    qDebug() << "Waiting for next connection...";
                }
                clientSocket = server.waitForNextConnectionSocket();
                Q_ASSERT(clientSocket);
                continue; // go to "waitForReadyRead"
            } else {
                qDebug() << "HttpServerThread:" << clientSocket->error() << "waiting for \"request\" packet";
                break;
            }
        }
        const QByteArray request = clientSocket->readAll();
        if (doDebug) {
            qDebug() << "HttpServerThread: request:" << request;
        }
        // Split headers and request xml
        const bool splitOK = splitHeadersAndData(request, m_receivedHeaders, m_receivedData);
        Q_ASSERT(splitOK);
        Q_UNUSED(splitOK); // To avoid a warning if Q_ASSERT doesn't expand to anything.
        m_headers = parseHeaders(m_receivedHeaders);

        if (m_headers.value("_path").endsWith("terminateThread")) // we're asked to exit
            break; // normal exit

        // TODO compared with expected SoapAction
        QList<QByteArray> contentTypes = m_headers.value("Content-Type").split(';');
        if (contentTypes[0] == "text/xml" && m_headers.value("SoapAction").isEmpty()) {
            qDebug() << "ERROR: no SoapAction set for Soap 1.1";
            break;
        }else if( contentTypes[0] == "application/soap+xml" && !contentTypes[2].startsWith("action")){
            qDebug() << "ERROR: no SoapAction set for Soap 1.2";
            break;
        }

        //qDebug() << "headers received:" << m_receivedHeaders;
        //qDebug() << headers;
        //qDebug() << "data received:" << m_receivedData;


        if (m_features & BasicAuth) {
            QByteArray authValue = m_headers.value("Authorization");
            if (authValue.isEmpty())
                authValue = m_headers.value("authorization"); // as sent by Qt-4.5
            bool authOk = false;
            if (!authValue.isEmpty()) {
                //qDebug() << "got authValue=" << authValue; // looks like "Basic <base64 of user:pass>"
                Method method;
                QString headerVal;
                parseAuthLine(QString::fromLatin1(authValue), &method, &headerVal);
                //qDebug() << "method=" << method << "headerVal=" << headerVal;
                switch (method) {
                case None: // we want auth, so reject "None"
                    break;
                case Basic:
                    {
                        const QByteArray userPass = QByteArray::fromBase64(headerVal.toLatin1());
                        //qDebug() << userPass;
                        // TODO if (validateAuth(userPass)) {
                        if (userPass == ("kdab:testpass")) {
                            authOk = true;
                        }
                        break;
                    }
                default:
                    qWarning("Unsupported authentication mechanism %s", authValue.constData());
                }
            }

            if (!authOk) {
                // send auth request (Qt supports basic, ntlm and digest)
                const QByteArray unauthorized = "HTTP/1.1 401 Authorization Required\r\nWWW-Authenticate: Basic realm=\"example\"\r\nContent-Length: 0\r\n\r\n";
                clientSocket->write( unauthorized );
                if (!clientSocket->waitForBytesWritten(2000)) {
                    qDebug() << "HttpServerThread:" << clientSocket->error() << "writing auth request";
                    break;
                }
                continue;
            }
        }

        // send response
        QByteArray response = makeHttpResponse(m_dataToSend);
        if (doDebug) {
            qDebug() << "HttpServerThread: writing" << response;
        }
        clientSocket->write(response);

        clientSocket->flush();
    }
    // all done...
    delete clientSocket;
    if (doDebug) {
        qDebug() << "HttpServerThread terminated";
    }
}

#include "moc_httpserver_p.cpp"
#include "httpserver_p.moc"