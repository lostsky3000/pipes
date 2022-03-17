#ifndef TCP_DECODE_H
#define TCP_DECODE_H

#include <cstdint>
#include <cstring>

// return:  -1:not done   0:done(succ)  >0:done(errCode)
typedef int(*FN_DECODE_CONN_CHECK)(struct tcp_decode*d, char* buf, int bufSize, int& checkedBytes);

typedef int(*FN_DECODE_RSP_IMPL)(void* ud, const char* buf, int sz);
typedef int(*FN_DECODE_RSP)(struct tcp_decode*d, void* ud, FN_DECODE_RSP_IMPL impl);

// return:  -1:not done   0:done(succ)   >0:done(errCode)
typedef int(*FN_DECODE_PACK_HEAD)(struct tcp_decode*d, uint8_t* buf, int bufSize, int& checkedBytes);

// return:  -1:not done   0:done(succ)   >0:done(errCode)
typedef int(*FN_DECODE_PACK_BODY)(struct tcp_decode*d, uint8_t* buf, int bufSize, int& checkedBytes);

typedef int(*FN_DECODE_PACK_RESET)(struct tcp_decode*d);

#define TCPDEC_STATE_PACK_ERROR -1
#define TCPDEC_STATE_CONN_CHECKING 1
#define TCPDEC_STATE_CONN_DONE 2
#define TCPDEC_STATE_PACK_HEAD 3
#define TCPDEC_STATE_PACK_DEC_BODY 4
#define TCPDEC_STATE_PACK_READ_BODY 5
//
#define TCPDEC_STATE_INNER_MSG 6
//#define TCPDEC_STATE_CLOSE 6

struct protocol_cfg;
struct tcp_decode
{
	int8_t protocol;
	int8_t state;
	int8_t errCode;
	int8_t packNeedDecode;
	int32_t curPackBodyLen;
	int32_t curPackBodyReadCnt;
	int32_t innerMsg;
	FN_DECODE_CONN_CHECK cbConnCheck;
	FN_DECODE_RSP cbConnRsp;
	FN_DECODE_PACK_HEAD cbPackHead;
	FN_DECODE_PACK_BODY cbPackDecBody;
	FN_DECODE_PACK_RESET cbPackReset;
};

struct tcp_decode* decode_new(struct protocol_cfg* cfg);
void decode_reset(struct tcp_decode* d, struct protocol_cfg* cfg);
int decode_destroy(struct tcp_decode* d);

//
struct dec_sep
{
	int seekedTotal;
	int chMatchCnt;
	int sepLen;
	char* sep;
};
inline void decsep_reset(struct dec_sep* d, const char*sep, int sepLen)
{
	if(d->sep && d->sepLen < sepLen){
		delete[] d->sep;
		d->sep = nullptr;
	}
	if(d->sep == nullptr){
		d->sep = new char[sepLen];
		d->sepLen = sepLen;
	}
	memcpy(d->sep, sep, sepLen);
	d->chMatchCnt = 0;
	d->seekedTotal = 0;
}
inline struct dec_sep* decsep_new()
{
	struct dec_sep* d = new struct dec_sep;
	d->chMatchCnt = 0;
	d->seekedTotal = 0;
	d->sep = nullptr;
	d->sepLen = 0;
	return d;
}
int decsep_check(struct dec_sep* d, const char* buf, int bufSize, int& readBytes);

#endif // !PROTOCOL_DECODE_H

