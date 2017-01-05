/*
   Drawpile - a collaborative drawing program.

   Copyright (C) 2008-2014 Calle Laakkonen

   Drawpile is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Drawpile is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Drawpile.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "multiserver.h"
#include "initsys.h"
#include "sslserver.h"
#include "database.h"

#include "../shared/server/session.h"
#include "../shared/server/sessionserver.h"
#include "../shared/server/client.h"
#include "../shared/server/serverconfig.h"

#include "../shared/util/announcementapi.h"

#include <QTcpSocket>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QJsonObject>
#include <QJsonArray>

namespace server {

MultiServer::MultiServer(ServerConfig *config, QObject *parent)
	: QObject(parent),
	m_config(config),
	m_server(nullptr),
	m_state(NOT_STARTED),
	m_autoStop(false)
{
	m_sessions = new SessionServer(config, this);

	connect(m_sessions, &SessionServer::sessionCreated, this, &MultiServer::assignRecording);
	connect(m_sessions, &SessionServer::sessionEnded, this, &MultiServer::tryAutoStop);
	connect(m_sessions, &SessionServer::userLoggedIn, this, &MultiServer::printStatusUpdate);
	connect(m_sessions, &SessionServer::userDisconnected, [this]() {
		printStatusUpdate();
		// The server will be fully stopped after all users have disconnected
		if(m_state == STOPPING)
			stop();
		else
			tryAutoStop();
	});
}

void MultiServer::setMustSecure(bool secure)
{
	m_sessions->setMustSecure(secure);
}

#ifndef NDEBUG
void MultiServer::setRandomLag(uint lag)
{
	m_sessions->setRandomLag(lag);
}
#endif

/**
 * @brief Automatically stop server when last session is closed
 *
 * This is used in socket activation mode. The server will be restarted
 * by the system init daemon when needed again.
 * @param autostop
 */
void MultiServer::setAutoStop(bool autostop)
{
	m_autoStop = autostop;
}

void MultiServer::setAnnounceLocalAddr(const QString &addr)
{
	m_sessions->announcementApiClient()->setLocalAddress(addr);
}

void MultiServer::setRecordingPath(const QString &path)
{
	m_recordingPath = path;
}

bool MultiServer::createServer()
{
	if(!m_sslCertFile.isEmpty() && !m_sslKeyFile.isEmpty()) {
		SslServer *server = new SslServer(m_sslCertFile, m_sslKeyFile, this);
		if(!server->isValidCert())
			return false;
		m_server = server;

	} else {
		m_server = new QTcpServer(this);
	}

	connect(m_server, &QTcpServer::newConnection, this, &MultiServer::newClient);

	return true;
}

/**
 * @brief Start listening on the specified address.
 * @param port the port to listen on
 * @param address listening address
 * @return true on success
 */
bool MultiServer::start(quint16 port, const QHostAddress& address) {
	Q_ASSERT(m_state == NOT_STARTED);
	m_state = RUNNING;
	if(!createServer())
		return false;

	if(!m_server->listen(address, port)) {
		logger::error() << m_server->errorString();
		delete m_server;
		m_server = nullptr;
		m_state = NOT_STARTED;
		return false;
	}

	logger::info() << "Started listening on port" << port << "at address" << address.toString();
	return true;
}

/**
 * @brief Start listening on the given file descriptor
 * @param fd
 * @return true on success
 */
bool MultiServer::startFd(int fd)
{
	Q_ASSERT(m_state == NOT_STARTED);
	m_state = RUNNING;
	if(!createServer())
		return false;

	if(!m_server->setSocketDescriptor(fd)) {
		logger::error() << "Couldn't set server socket descriptor!";
		delete m_server;
		m_server = nullptr;
		m_state = NOT_STARTED;
		return false;
	}

	logger::info() << "Started listening on passed socket";
	return true;
}

/**
 * @brief Assign a recording file name to a new session
 *
 * The name is generated by replacing placeholders in the file name pattern.
 * If a file with the same name exists, a number is inserted just before the suffix.
 *
 * If the file name pattern points to a directory, the default pattern "%d %t session %i.dprec"
 * will be used.
 *
 * The following placeholders are supported:
 *
 *  ~/ - user's home directory (at the start of the pattern)
 *  %d - the current date (YYYY-MM-DD)
 *  %h - the current time (HH.MM.SS)
 *  %i - session ID
 *  %a - session alias (or ID if not assigned)
 *
 * @param session
 */
void MultiServer::assignRecording(Session *session)
{
	QString filename = m_recordingPath;

	if(filename.isEmpty())
		return;

	// Expand home directory
	if(filename.startsWith("~/")) {
		filename = QString(qgetenv("HOME")) + filename.mid(1);
	}

	// Use default file pattern if target is a directory
	QFileInfo fi(filename);
	if(fi.isDir()) {
		filename = QFileInfo(QDir(filename), "%d %t session %i.dprec").absoluteFilePath();
	}

	// Expand placeholders
	QDateTime now = QDateTime::currentDateTime();
	filename.replace("%d", now.toString("yyyy-MM-dd"));
	filename.replace("%t", now.toString("HH.mm.ss"));
	filename.replace("%i", session->idString());
	filename.replace("%a", session->idAlias().isEmpty() ? session->idString() : session->idAlias());

	fi = filename;

	session->setRecordingFile(fi.absoluteFilePath());
}

/**
 * @brief Accept or reject new client connection
 */
void MultiServer::newClient()
{
	QTcpSocket *socket = m_server->nextPendingConnection();

	logger::info() << "Accepted new client from address" << socket->peerAddress();

	auto *client = new Client(socket);

	if(m_config->isAddressBanned(socket->peerAddress())) {
		logger::info() << "Kicking banned client from address" << socket->peerAddress() << "straight away";
		client->disconnectKick("BANNED");

	} else {
		m_sessions->addClient(client);
		printStatusUpdate();
	}
}


void MultiServer::printStatusUpdate()
{
	initsys::notifyStatus(QString("%1 users and %2 sessions")
		.arg(m_sessions->totalUsers())
		.arg(m_sessions->sessionCount())
	);
}

/**
 * @brief Stop the server if vacant (and autostop is enabled)
 */
void MultiServer::tryAutoStop()
{
	if(m_state == RUNNING && m_autoStop && m_sessions->sessionCount() == 0 && m_sessions->totalUsers() == 0) {
		logger::info() << "Autostopping due to lack of sessions";
		stop();
	}
}

/**
 * Disconnect all clients and stop listening.
 */
void MultiServer::stop() {
	if(m_state == RUNNING) {
		logger::info() << "Stopping server and kicking out" << m_sessions->totalUsers() << "users...";
		m_state = STOPPING;
		m_server->close();

		m_sessions->stopAll();
	}

	if(m_state == STOPPING) {
		if(m_sessions->totalUsers() == 0) {
			m_state = STOPPED;
			logger::info() << "Server stopped.";
			emit serverStopped();
		}
	}
}

JsonApiResult MultiServer::callJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	QString head;
	QStringList tail;
	std::tie(head, tail) = popApiPath(path);

	if(head == "server")
		return serverJsonApi(method, tail, request);
	else if(head == "sessions")
		return m_sessions->callJsonApi(method, tail, request);
	else if(head == "banlist")
		return banlistJsonApi(method, tail, request);

	return JsonApiNotFound();
}

/**
 * @brief Serverwide settings
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::serverJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method != JsonApiMethod::Get && method != JsonApiMethod::Update)
		return JsonApiBadMethod();

	const ConfigKey settings[] = {
		config::ClientTimeout,
		config::SessionSizeLimit,
		config::SessionCountLimit,
		config::EnablePersistence,
		config::IdleTimeLimit,
		config::ServerTitle,
		config::WelcomeMessage,
		config::AnnounceWhiteList,
		config::LocalAddress,
		config::PrivateUserList,
		config::AllowGuests
	};
	const int settingCount = sizeof(settings) / sizeof(settings[0]);

	if(method==JsonApiMethod::Update) {
		for(int i=0;i<settingCount;++i) {
			if(request.contains(settings[i].name)) {
				m_config->setConfigString(settings[i], request[settings[i].name].toVariant().toString());
			}
		}
	}

	QJsonObject result;
	for(int i=0;i<settingCount;++i) {
		result[settings[i].name] = QJsonValue::fromVariant(m_config->getConfigVariant(settings[i]));
	}

	return JsonApiResult { JsonApiResult::Ok, QJsonDocument(result) };
}

/**
 * @brief View and modify the serverwide banlist
 *
 * @param method
 * @param path
 * @param request
 * @return
 */
JsonApiResult MultiServer::banlistJsonApi(JsonApiMethod method, const QStringList &path, const QJsonObject &request)
{
	// Database is needed to manipulate the banlist
	Database *db = qobject_cast<Database*>(m_config);
	if(!db)
		return JsonApiNotFound();

	if(path.size()==1) {
		if(method != JsonApiMethod::Delete)
			return JsonApiBadMethod();
		if(db->deleteBan(path.at(0).toInt()))
			return JsonApiResult {JsonApiResult::Ok, QJsonDocument()};
		else
			return JsonApiNotFound();
	}

	if(!path.isEmpty())
		return JsonApiNotFound();

	if(method == JsonApiMethod::Get) {
		return JsonApiResult { JsonApiResult::Ok, QJsonDocument(db->getBanlist()) };

	} else if(method == JsonApiMethod::Create) {
		QHostAddress ip { request["ip"].toString() };
		if(ip.isNull())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Valid IP address required");
		int subnet = request["subnet"].toInt();
		QDateTime expiration = QDateTime::fromString(request["expiration"].toString(), "yyyy-MM-dd HH:mm:ss");
		if(expiration.isNull())
			return JsonApiErrorResult(JsonApiResult::BadRequest, "Valid expiration time required");
		QString comment = request["comment"].toString();

		return JsonApiResult { JsonApiResult::Ok, QJsonDocument(db->addBan(ip, subnet, expiration, comment)) };

	} else
		return JsonApiBadMethod();
}

}
