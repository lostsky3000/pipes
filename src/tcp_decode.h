#ifndef TCP_DECODE_H
#define TCP_DECODE_H

#include <cstdint>
#include <cstring>

// return:  -1: not done   0:has done(succ)  >0: has done(errCode)
typedef int(*FN_DECODE_CONN_CHECK)(struct tcp_decode*d, char* buf, int bufSize, int& readBytes);

struct tcp_decode
{
	int16_t protocol;
	int16_t connDone;
	int connRet;
	FN_DECODE_CONN_CHECK cbConnCheck;
};

struct protocol_cfg;
struct tcp_decode* decode_new(struct protocol_cfg* cfg);
void decode_reset(struct tcp_decode* d, struct protocol_cfg* cfg);


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

