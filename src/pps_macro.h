#ifndef PPS_MACRO_H
#define PPS_MACRO_H
#include <cstdint>

#ifdef __linux
#define PLAT_IS_LINUX
#elif defined(WIN32) || defined(_WIN32)
#define PLAT_IS_WIN
#endif 


#define LPPS_PIPES_NAME "LPIPES_PIPES"

//
#define SVC_MQIN_CAP_DEFAULT 1024
#define SVC_MQ_WAITSEND_CAP 128
#define SVC_MQ_WAITSEND_MAX INT32_MAX
#define SVC_STEP_PROCMSG_MAX 128
#define SVC_MQTIMER_LEN 32
#define SVC_MQNET_LEN 256



//
#define MAX_NET 16
#define MAX_TIMER 16
#define MAX_WORKER 256
#define SVC_SLOT_NUM 65536
#define MAX_SERVICE 60000

//
#define CB_STATUS_CREATE 1
#define CB_STATUS_START 2
#define CB_STATUS_DESTROY 3


#endif // !PPS_MACRO_H

