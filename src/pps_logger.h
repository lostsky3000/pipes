#ifndef PPS_LOGGER_H
#define PPS_LOGGER_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "mq_mpsc.h"
#include "mq_mpmc.h"
#include "pool_linked.h"

struct pipes;
struct logger_msg
{
	uint32_t id;
	uint32_t cap;
	int level;
	uint32_t msgLen;
	char* msg;
	int64_t when;
	int64_t who;
};

struct pps_logger
{
	std::atomic<bool> loop;
	std::atomic<bool> idle;
	std::atomic<uint32_t> unAddMsgNum;
	std::atomic<uint32_t> msgIdCnt;
	struct pipes* pipes;
	mq_mpmc<struct logger_msg*>* mqMsgPool;
	mq_mpsc<struct logger_msg*>* mqMsg;
	pool_linked<struct logger_msg*>* plkMsgBak;
	std::mutex mtxMsgBak;
	std::mutex mtxIdle;
	std::condition_variable condIdle;
};

void logger_thread(struct pps_logger* log);

//
struct logger_msg* logger_send_begin(uint32_t size, int64_t who, int level, struct pps_logger*logger);
int logger_send_end(const char* str, struct logger_msg* msg, struct pps_logger*logger);
//int logger_send(const char* str, uint32_t strLen, int64_t who, struct pps_logger*logger);

void logger_shutdown(struct pipes* pipes);

//
void logger_init_main_thread(struct pps_logger* log, struct pipes* pipes);
void logger_deinit_main_thread(struct pps_logger* log);

#endif // !PPS_LOGGER_H

