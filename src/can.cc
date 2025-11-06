#include "can.h"
#include "log.h"
#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <vector>

Can::Can(char *deviceName) :
	m_sd(-1),
	m_deviceName(deviceName)
{

}

Can::~Can()
{
	destroy();
}

int Can::init() 
{
	isAutoRead = false;
	m_sd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (m_sd == -1) {
		LOG(ERROR, "create socket failed.");
		return -1;
	} 

	strcpy(ifr.ifr_name, m_deviceName.c_str());
	if (ioctl(m_sd, SIOCGIFINDEX, &ifr) == -1) {
		LOG(ERROR, "ioctl error %s", m_deviceName.c_str());
		return -1;
	}
	
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;


	if (bind(m_sd, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
		LOG(ERROR, "bind error");
		close(m_sd);
		return -1;
	}

	LOG(INFO, "Successed:%s has interface index %d.", m_deviceName.c_str(), ifr.ifr_ifindex);
	return 0;
}

void Can::startAutoRead()
{
	this->start();
	this->detach();
	isAutoRead = true;
}

void Can::stopAutoRead()
{
	this->stop();
	isAutoRead = false;
}

void Can::run()
{
	struct pollfd poll_set[1];
	poll_set[0].fd = m_sd;
	poll_set[0].events = POLLIN;
	poll_set[0].revents = 0;
	int timeout = 1000;

	struct can_frame rx_frame;

	while(!this->isStoped()) {
		switch (poll(poll_set, 1, timeout)) {
		case -1:
			perror("poll");
			exit(1);
			break;
		case 0:
			break;
		default:
			int len = read(m_sd, &rx_frame, CAN_MTU);
			if (len > 0) 
				onCanReceiveDataCallback(std::move(rx_frame));
			break;
		}
	}
}

int Can::destroy()
{
	if(isAutoRead) 
		stopAutoRead();
	return close(m_sd);
}

int Can::transmit(struct can_frame *frame)
{
	int ret = write(m_sd, frame, sizeof(*frame));
	if (ret != sizeof (*frame))
		return -1;
	return 0;
}

void Can::setOnCanReceiveDataCallback(std::function<void (struct can_frame &&)> callback)
{
	onCanReceiveDataCallback = std::move(callback);
}
