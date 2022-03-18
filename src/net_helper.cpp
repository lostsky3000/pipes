#include "net_helper.h"
#include "pps_sysinfo.h"
#include <cstring>

struct wrap_buf
{
	char* buf;
	int cap;
};

static inline void wb_reset(struct wrap_buf* b, int newCap)
{
	if(b->buf && b->cap < newCap){  // expand
		delete[] b->buf;
		b->buf = nullptr;
	}
	if(b->buf == nullptr){
		b->buf = new char[newCap];
		b->cap = newCap;
	}
}
static inline struct wrap_buf* wb_new(int cap)
{
	struct wrap_buf* b = new struct wrap_buf;
	b->buf = nullptr;
	wb_reset(b, cap);
	return b;
}

char* nethp_wrap_wssend(struct net_helper* h, int op, const char* data, uint32_t szData, int* szOut)
{
	struct wrap_buf* b = h->bufWsSend;
	if(op == 1 || op == 2){   // is payload frame
		int totalLen;
		if (szData < 126) {   // 1byte for len
			totalLen = szData + 2;
			if(totalLen > b->cap){
				wb_reset(b, totalLen);
			}
			((uint8_t*)b->buf)[1] = szData;  // no mask
			memcpy(b->buf + 2, data, szData);
		} else if (szData < 65536) {  //2bytes for len
			int totalLen = szData + 4;
			if (totalLen > b->cap) {
				wb_reset(b, totalLen);
			}
			((uint8_t*)b->buf)[1] = 126;   // no mask
			if(IS_BIGENDIAN){
				*((uint16_t*)(b->buf + 2)) = szData;
			}else{
				uint8_t* pLen = (uint8_t*)(b->buf + 2);
				pLen[0] = szData & 0xff;
				pLen[1] = szData >> 8;
			}
			memcpy(b->buf + 4, data, szData);
		} else {   //8bytes for len
			int totalLen = szData + 8;
			if (totalLen > b->cap) {
				wb_reset(b, totalLen);
			}
			((uint8_t*)b->buf)[1] = 127;   // no mask
			if(IS_BIGENDIAN){
				*((uint64_t*)(b->buf + 2)) = szData;
			}else{
				uint8_t* pLen = (uint8_t*)(b->buf + 2);
				int8_t cnt = 0;
				uint32_t sz = szData;
				while(cnt < 8){
					if(sz > 0){
						pLen[cnt++] = sz & 0xff;
						sz = sz >> 8;
					}else{
						pLen[cnt++] = 0;
					}
				}
			}
			memcpy(b->buf + 8, data, szData);
		}
		((uint8_t*)b->buf)[0] = 128 | op;  // 10000000 | op
		*szOut = totalLen;
		return b->buf;
	}else{   // control frame
		if(op == 9){
			((uint8_t*)b->buf)[0] = 128 | op;
			((uint8_t*)b->buf)[1] = 0;
			*szOut = 2;
			return b->buf;
		}
	}
	return nullptr;
}


int nethp_init(struct net_helper* h)
{
	h->bufWsSend = wb_new(1024);
	return 1;
}
void nethp_deinit(struct net_helper* h)
{

}

