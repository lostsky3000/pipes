#include "net_helper.h"
#include "pps_sysinfo.h"
#include <cstring>

struct wrap_buf
{
	char* buf;
	int cap;
	int widx;
};

static inline void wb_check_size(struct wrap_buf* b, int newCap)
{
	if(b->buf && b->cap < newCap){  // expand
		char* oldBuf = b->buf;
		char* newBuf = new char[newCap];
		if(b->widx > 0){  // has old data
			memcpy(newBuf, oldBuf, b->widx);
		}
		b->buf = newBuf;
		b->cap = newCap;
		delete[] oldBuf;
	}else if(b->buf == nullptr){
		b->buf = new char[newCap];
		b->cap = newCap;
		b->widx = 0;
	}
}
static inline struct wrap_buf* wb_new(int cap)
{
	struct wrap_buf* b = new struct wrap_buf;
	b->buf = nullptr;
	b->cap = 0;
	b->widx = 0;
	wb_check_size(b, cap);
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
				wb_check_size(b, totalLen);
			}
			((uint8_t*)b->buf)[1] = szData;  // no mask
			memcpy(b->buf + 2, data, szData);
		} else if (szData < 65536) {  //2bytes for len
			int totalLen = szData + 4;
			if (totalLen > b->cap) {
				wb_check_size(b, totalLen);
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
				wb_check_size(b, totalLen);
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

int nethp_wrap_rdssend_helper_init(struct net_helper* h)
{	
	h->bufRdsSend->widx = 0;
	return 1;
}
int nethp_wrap_rdssend(struct net_helper* h, struct nethp_rdssend_ctx* ctx, const char* item, int szItem)
{
	struct wrap_buf* b = h->bufRdsSend;
	int& widx = b->widx;
	if(ctx->itemCnt == 0){   // 1st item, write array head
		//widx = 0;
		// *x\r\n
		b->buf[widx++] = '*';
		int itemTotal = ctx->itemTotal;
		if(itemTotal < 10){
			b->buf[widx++] = (char)itemTotal + '0';
		}else if(itemTotal < 100){
			b->buf[widx++] = (char)(itemTotal / 10) + '0';
			b->buf[widx++] = (char)(itemTotal % 10) + '0';
		}else if(itemTotal < 1000){
			b->buf[widx++] = (char)(itemTotal / 100) + '0';
			b->buf[widx++] = (char)((itemTotal % 100) / 10) + '0';
			b->buf[widx++] = (char)(itemTotal % 10) + '0';
		}else{  // too many args
			return 0;
		}
		b->buf[widx++] = '\r';
		b->buf[widx++] = '\n';
	}
	//
	b->buf[widx++] = '$';
	//
	wb_check_size(b, widx + szItem + 64);  // check bufSize before write item
	if(item){  // bulk string
		if(szItem > 0){  //  4\r\nhehe
			if (szItem < 10) {
				b->buf[widx++] = (char)szItem + '0';
			} else if (szItem < 100) {
				b->buf[widx++] = (char)(szItem / 10) + '0';
				b->buf[widx++] = (char)(szItem % 10) + '0';
			} else if (szItem < 1000) {
				b->buf[widx++] = (char)(szItem / 100) + '0';
				b->buf[widx++] = (char)((szItem % 100) / 10) + '0';
				b->buf[widx++] = (char)(szItem % 10) + '0';
			} else if (szItem < 10000){
				b->buf[widx++] = (char)(szItem / 1000) + '0';
				b->buf[widx++] = (char)((szItem % 1000) / 100) + '0';
				b->buf[widx++] = (char)((szItem % 100) / 10) + '0';
				b->buf[widx++] = (char)(szItem % 10) + '0';
			}else if(szItem < 100000){
				b->buf[widx++] = (char)(szItem / 10000) + '0';
				b->buf[widx++] = (char)((szItem % 10000) / 1000) + '0';
				b->buf[widx++] = (char)((szItem % 1000) / 100) + '0';
				b->buf[widx++] = (char)((szItem % 100) / 10) + '0';
				b->buf[widx++] = (char)(szItem % 10) + '0';
			}else {  // too long
				return 0;
			}
			//
			b->buf[widx++] = '\r';
			b->buf[widx++] = '\n';
			memcpy(b->buf + widx, item, szItem);
			widx += szItem;
		}else{  // 0\r\n
			b->buf[widx++] = '0';
			b->buf[widx++] = '\r';
			b->buf[widx++] = '\n';
		}
	}else{  // nil
		// -1
		b->buf[widx++] = '-';
		b->buf[widx++] = '1';
	}
	b->buf[widx++] = '\r';
	b->buf[widx++] = '\n';
	//
	if(++ctx->itemCnt == ctx->itemTotal){   // last item
		ctx->szOut = b->widx;
		ctx->bufOut = b->buf;
	}
	return 1;
}
char* nethp_rdssendbuf(struct net_helper* h, int* szOut)
{
	*szOut = h->bufRdsSend->widx;
	return h->bufRdsSend->buf;
}

//
int nethp_init(struct net_helper* h)
{
	h->bufWsSend = wb_new(1024);
	//
	h->bufRdsSend = wb_new(1024);
	//
	return 1;
}
void nethp_deinit(struct net_helper* h)
{

}

