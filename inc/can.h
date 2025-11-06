#ifndef CAN_H
#define CAN_H

#include <bits/stdint-uintn.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <linux/can.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/can/raw.h>
#include <vector>
#include "MThread.h"


class Can : public MThread
{
public:
    Can(char *deviceName);
    ~Can();
	
	int init();
	int destroy();

	void startAutoRead();
	void stopAutoRead();
	virtual void run() override;
	void setOnCanReceiveDataCallback(std::function<void (struct can_frame &&)> callback);
	
	int transmit(struct can_frame *frame);

private:
    int m_sd;
	std::string m_deviceName;
	struct ifreq ifr;
	struct sockaddr_can addr;
	bool isAutoRead;
	std::function<void (struct can_frame &&)> onCanReceiveDataCallback;
};

#endif
