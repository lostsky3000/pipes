#ifndef PPS_WORKER_H
#define PPS_WORKER_H

#include <cstdint>
#include <atomic>
#include "mq_mpmc.h"

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

#endif // !PPS_WORKER_H

