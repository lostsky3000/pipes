#ifndef PPS_WORKER_H
#define PPS_WORKER_H

#include <cstdint>
#include <atomic>
#include "mq_mpmc.h"
#include "pool_linked.h"

#define WORKER_TMP_BUF_SIZE 4096

struct pipes;
struct pps_service;

struct worker_tmp_buf
{
	int32_t cap;
	char* buf;	
};

struct pps_worker
{
	uint32_t index;
	struct pipes* pipes;
	//
	struct mq_mpmc<int32_t>* queTask;
	//
	struct pool_linked<struct pps_service*>* plkSvcWaitFree;
	int32_t svcWaitFreeNum;
	//
	struct worker_tmp_buf tmpBuf;
	//
	char** arrPtrChar;
	int arrPtrCharSize;
	int* arrInt;
	int arrIntSize;
};

void worker_thread(struct pps_worker* wk);

int worker_add_task(struct pipes* pipes, struct pps_service*s, bool isNew, struct pps_worker* wkSelf=nullptr);

int worker_init_main_thread(struct pps_worker* wk, struct pipes* pipes, uint32_t index);
int worker_deinit_main_thread(struct pps_worker* wk);

int worker_shutdown(struct pipes* pipes);

//
struct exclusive_thread_ctx;
void worker_exclusive_thread(struct exclusive_thread_ctx* ctx);
int worker_exclusive_init(struct pps_worker* wk, struct pipes* pipes);
int worker_exclusive_deinit(struct pps_worker* wk);

#endif // !PPS_WORKER_H

