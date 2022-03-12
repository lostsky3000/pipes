#ifndef DEC_HTTP_HEAD_H
#define DEC_HTTP_HEAD_H

#include <cstdint>
#include <cstring>
#include "tcp_decode.h"

#define DECHH_STATE_METHOD 0
#define DECHH_STATE_URL 1
#define DECHH_STATE_VER 2
#define DECHH_STATE_HEADER 3
#define DECHH_STATE_DONE 4
#define DECHH_STATE_ERROR -1

//
#define DECHH_ERR_METHOD_INVALID 1
#define DECHH_ERR_URL_INVALID 2
#define DECHH_ERR_VER_INVALID 3
#define DECHH_ERR_HEADER_INVALID 4

//
#define DECHH_URL_LEN_MAX 1024
#define DECHH_HEADER_LEN_MAX 2048

static const char* DECHH_VER = "HTTP/1.1";
static const int DECHH_VER_LEN = 8;

struct dec_http_head
{
	int8_t state;
	int8_t errCode;
	//
	int8_t headState;   // 0:reading key  1: reading val  2: checking header end
	int8_t methodPtrnLen;
	//
	int fieldLenCnt;
	int urlLen;
	//
	int widxBuf;
	int szBuf;
	char* buf;
	//
	int tmpPos;
	int headerNum;
	int headLenCnt;
	//
	char* methodPtrn;
	char method[8];
};

inline void dechh_reset(struct dec_http_head* d)
{
	char* preBuf = d->buf;
	int preSzBuf = d->szBuf;
	memset(d, 0, sizeof(struct dec_http_head));
	d->buf = preBuf;
	d->szBuf = preSzBuf;
}
inline void dechh_destroy(struct dec_http_head* d)
{
	if(d){
		if (d->buf) {
			delete[] d->buf;
			d->buf = nullptr;
		}
		delete d;
	}
}

inline struct dec_http_head* dechh_new()
{
	struct dec_http_head* d = new struct dec_http_head;
	d->buf = new char[DECHH_URL_LEN_MAX + 256];
	d->szBuf = DECHH_URL_LEN_MAX + 256;
	dechh_reset(d);
	return d;
}

inline const char* dechh_method(struct dec_http_head* d, int* len)
{
	if(d->state == DECHH_STATE_DONE){
		*len = d->methodPtrnLen;
		return d->method;
	}
	return nullptr;
}
inline const char* dechh_ver(struct dec_http_head* d, int* len)
{
	if(d->state == DECHH_STATE_DONE){
		*len = DECHH_VER_LEN;
		return DECHH_VER;
	}
	return nullptr;
}
inline const char* dechh_url(struct dec_http_head* d, int* len)
{
	if (d->state == DECHH_STATE_DONE) {
		*len = d->urlLen;
		return d->buf;
	}
	return nullptr;
}
inline int dechh_header_num(struct dec_http_head* d)
{
	if(d->state == DECHH_STATE_DONE){
		return d->headerNum;
	}
	return 0;
}
inline int dechh_headerit_init(struct dec_http_head* d)
{
	if (d->state == DECHH_STATE_DONE) {
		d->headLenCnt = 0;  // use as headerItemCnt
		d->fieldLenCnt = d->urlLen + 1; // use as headerReadIdx
		return 1;
	}
	return 0;
}
inline char* dechh_headerit_next(struct dec_http_head* d, int* pKeyLen, char** ptrVal, int* pValLen)
{
	if (d->state == DECHH_STATE_DONE) {
		if(++d->headLenCnt <= d->headerNum){
			int keyLen = *((uint16_t*)(d->buf + d->fieldLenCnt));
			char* key = d->buf + d->fieldLenCnt + 2;
			d->fieldLenCnt += 2 + keyLen + 1;
			*pKeyLen = keyLen;
			//
			int valLen = *((uint16_t*)(d->buf + d->fieldLenCnt));
			*ptrVal = d->buf + d->fieldLenCnt + 2;
			d->fieldLenCnt += 2 + valLen + 1;
			*pValLen = valLen;
			return key;
		}
	}
	return nullptr;
}

// return:  -1: not done   0:has done(succ)  >0: has done(errCode)
int dechh_tick(struct dec_http_head*d, const char* buf, int bufSize, int& readBytes);

#endif // !DEC_HTTP_HEAD_H
