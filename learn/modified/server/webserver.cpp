#include "webserver.h"

WebServer::WebServer(int port, int trigMode, int timeoutMS, bool optLinger, int threadNum, bool openLog, int logLevel, int logQueSize, int actor) : 
	port_(port), openLinger_(optLinger), timeoutMS_(timeoutMS), isClose_(false), timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller()) {
		// get current working directory
		srcDir_ = getcwd(nullptr, 256);
		assert(srcDir_);
		// concatenate
		strncat(srcDir_, "/resource/", 16);
		HttpConn::userCount = 0;
		HttpConn::srcDir = srcDir_;

		// sequence
		if (!InitSocket_()) {
			isClose_ = true;
		}

		// sequence
		InitEventMode_(trigMode);
		if (openLog) {
			Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
			if (isClose_) {
				LOG_ERROR("============== Server init error ================");
			}
			else {
				LOG_INFO("=============== Server init ======================");
				LOG_INFO("Port:%d, OpenLinger:%s", port_, optLinger ? "true" : "false");
				LOG_INFO("Listen Mode:%s, OpenConn Mode:%s", (listenEvent_ & EPOLLET ? "ET" : "LT"), (connEvent_ & EPOLLET ? "ET" : "LT"));
				LOG_INFO("LogSys level:%d", logLevel);
				LOG_INFO("srcDir:%s", HttpConn::srcDir);
				LOG_INFO("ThreadPool num:%d", threadNum);
			}
		}
		actor_ = actor;

}

WebServer::~WebServer() {
	close(listenFd_);
	isClose_ = true;
	free(srcDir_);
}

void WebServer::InitEventMode_(int trigMode) {
	listenEvent_ = EPOLLRDHUP;
	connEvent_ = EPOLLONESHOT | EPOLLRDHUP;
	switch (trigMode) {
		case 0:
			break;
		case 1:
			connEvent_ |= EPOLLET;
			break;
		case 2:
			listenEvent_ |= EPOLLET;
			break;
		case 3:
			listenEvent_ |= EPOLLET;
			connEvent_ |= EPOLLET;
			break;
		default:
			listenEvent_ |= EPOLLET;
			connEvent_ |= EPOLLET;
			break;
	}
	HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start() {
	int timeMS = -1;
	if (!isClose_) {
		LOG_INFO("================= Server start ==============");
	}
	while (!isClose_) {
		if (timeoutMS_ > 0) {
			// get the remain time if > 0
			timeMS = timer_->GetNextTick();
		}
		int eventCnt = epoller_->Wait(timeMS);
		for (int i = 0; i < eventCnt; ++i) {
			// fd events
			int fd = epoller_->GetEventFd(i);
			uint32_t events = epoller_->GetEvents(i);
			if (fd == listenFd_) {
				DealListen_();
			}
			else if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
				assert(users_.count(fd) > 0);
				CloseConn_(&users_[fd]);
			}
			else if (events & EPOLLIN) {
				assert(users_.count(fd) > 0);
				DealRead_(&users_[fd]);
			}
			else if (events & EPOLLOUT) {
				assert(users_.count(fd) > 0);
				DealWrite_(&users_[fd]);
			}
			else {
				LOG_ERROR("Unexpected event.");
			}
		}
	}
}

void WebServer::SendError_(int fd, const char *info) {
	assert(fd > 0);
	int ret = send(fd, info, strlen(info), 0);
	if (ret < 0) {
		LOG_WARN("send error to client[%d] error!", fd);
	}
	close(fd);
}

void WebServer::CloseConn_(HttpConn *client) {
	assert(client);
	LOG_INFO("Client [%d] quit!", client->GetFd());
	epoller_->DelFd(client->GetFd());
	client->Close();
}
void WebServer::AddClient_(int fd, sockaddr_in addr) {
	assert(fd > 0);
	users_[fd].init(fd, addr);
	if (timeoutMS_ > 0) {
		timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
	}
	epoller_->AddFd(fd, EPOLLIN | connEvent_);
	SetFdNonblock(fd);
	LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

void WebServer::DealListen_() {
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	do {
		int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
		if (fd <= 0) {
			return;
		}
		else if (HttpConn::userCount >= MAX_FD) {
			SendError_(fd, "Server busy!");
			LOG_WARN("Client is full!");
			return;
		}
		AddClient_(fd, addr);
	} while (listenEvent_ & EPOLLET);
}

void WebServer::DealRead_(HttpConn *client) {
	assert(client);
	ExtentTime_(client);
	// actor_mode
	// reactor
	if (actor_ == 1) {
		threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
	}
	//proactor
	else {
		assert(client);
		int ret = -1;
		int readErrno = 0;
		ret = client->read(&readErrno);
		//Read_(client);
		threadpool_->AddTask(std::bind(&WebServer::ProRead_, this, client, ret, readErrno));
	}

	//

	//threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}
void WebServer::DealWrite_(HttpConn *client) {
	assert(client);
	ExtentTime_(client);

	// reactor
	if (actor_ == 1) {
		threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
	}
	//proactor
	else {
		assert(client);
		int ret = -1;
		int writeErrno = 0;
		ret = client->write(&writeErrno);

		threadpool_->AddTask(std::bind(&WebServer::ProWrite_, this, client, ret, writeErrno));
	}
	//
	//threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn *client) {
	assert(client);
	if (timeoutMS_ > 0) {
		timer_->adjust(client->GetFd(), timeoutMS_);
	}
}
/*
void WebServer::OnRead_(HttpConn *client) {
ient->write(&writeErrno))sseet(client);
	int ret = -1;
	int readErrno = 0;
	ret = client->read(&readErrno);
	if (ret <= 0 && readErrno != EAGAIN) {
		CloseConn_(client);
		return;
	}
	OnProcess_(client);
}
*/


void WebServer::OnRead_(HttpConn *client) {
	assert(client);
	int ret = -1;
	int readErrno = 0;
	ret = client->read(&readErrno);
	if (ret <= 0 && readErrno != EAGAIN) {
		CloseConn_(client);
		return;
	}

	OnProcess_(client);
}

// proactor_read
void WebServer::ProRead_(HttpConn *client, int ret, int readErrno) {
	// worker
	if (ret <= 0 && readErrno != EAGAIN) {
		CloseConn_(client);
		return;
	}
	OnProcess_(client);
}
// proactor_write
void WebServer::ProWrite_(HttpConn *client, int ret, int writeErrno) {
	// worker
	if (client->ToWriteBytes() == 0) {
		if (client->IsKeepAlive()) {
			OnProcess_(client);
			return;
		}
	}
	else if (ret < 0) {
		if (writeErrno == EAGAIN) {
			// continue trans
			epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
			return;
		}
	}
	CloseConn_(client);
}

/*
void WebServer::Write_(HttpConn *client) {
	assert(client);
	int ret = -1;
	int writeErrno = 0;
	ret = client->write(&writeErrno);
}
*/

void WebServer::OnWrite_(HttpConn *client) {
	assert(client);
	int ret = -1;
	int writeErrno = 0;
	ret = client->write(&writeErrno);
	if (client->ToWriteBytes() == 0) {
		if (client->IsKeepAlive()) {
			OnProcess_(client);
			return;
		}
	}
	else if (ret < 0) {
		if (writeErrno == EAGAIN) {
			// continue trans
			epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
			return;
		}
	}
	CloseConn_(client);
}


/*
void WebServer::OnWrite_(HttpConn *client) {
	assert(client);
	int ret = -1;
	int writeErrno = 0;
	ret = client->write(&writeErrno);
	if (client->ToWriteBytes() == 0) {
		if (client->IsKeepAlive()) {
			OnProcess_(client);
			return;
		}
	}
	else if (ret < 0) {
		if (writeErrno == EAGAIN) {
			// continue trans
			epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
			return;
		}
	}
	CloseConn_(client);
}
*/

// modified

void WebServer::OnProcess_(HttpConn *client) {
	if (client->process()) {
		epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
	}
	else {
		epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
	}
}


/*
void WebServer::OnProcess_(HttpConn *client) {
	if (client->process()) {
		epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
	}
	else {
		epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
	}
}
*/


bool WebServer::InitSocket_() {
	int ret;
	struct sockaddr_in addr;
	if (port_ > 65535 || port_ < 1024) {
		LOG_ERROR("Port:%d error!", port_);
		return false;
	}
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port_);
	// close:--> elegant
	struct linger optLinger = {0};
	if (openLinger_) {
		// close until data finished or timeouted
		optLinger.l_onoff = 1;
		optLinger.l_linger = 1;
	}
	listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd_ < 0) {
		LOG_ERROR("Create socket error!", port_);
		return false;
	}

	ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
	if (ret < 0) {
		close(listenFd_);
		LOG_ERROR("Init linger error!", port_);
		return false;
	}

	int optval = 1;
	// reuse the port
	ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));
	if (ret == -1) {
		LOG_ERROR("set socket error!");
		close(listenFd_);
		return false;
	}

	ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERROR("Bind port:%d error!", port_);
		close(listenFd_);
		return false;
	}

	ret = listen(listenFd_, 8);
	if (ret < 0) {
		LOG_ERROR("Listen port:%d error!", port_);
		close(listenFd_);
		return false;
	}

	ret = epoller_->AddFd(listenFd_, listenEvent_ | EPOLLIN);
	if (ret == false) {
		LOG_ERROR("Add listen error!");
		close(listenFd_);
		return false;
	}
	SetFdNonblock(listenFd_);
	LOG_INFO("Server port:%d", port_);
	return true;
}

int WebServer::SetFdNonblock(int fd) {
	assert(fd > 0);
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}