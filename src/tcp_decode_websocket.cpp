#include "tcp_decode_websocket.h"
#include "tcp_protocol.h"
#include "util_crypt.h"

static const char* SEC_KEY_SALT = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const int SEC_KEY_SALT_LEN = 36;

struct tcp_decode_websocket* decws_new(struct protocol_cfg_websocket* cfg)
{
	struct tcp_decode_websocket* d = new struct tcp_decode_websocket;
	d->uri = nullptr;
	d->uriCap = 0;
	d->uriLen = 0;
	d->decHttpHead = nullptr;
	//
	d->bufSecKey = nullptr;
	d->bufSecKeyCap = 0;
	d->secKeySaltPos = 0;
	//
	d->bufSecKeyRsp = nullptr;
	d->bufSecKeyRspLen = 0;
	//
	decws_reset(d, cfg);
	return d;
}

void decws_reset(struct tcp_decode_websocket* d, struct protocol_cfg_websocket* cfg)
{
	if(d->uri && d->uriCap <= cfg->uriLen){
		delete[] d->uri;
		d->uri = nullptr;
	}
	if(d->uri == nullptr){
		int cap = cfg->uriLen + 1;
		d->uri = new char[cap];
		d->uriCap = cap;
	}
	int uriLen = cfg->uriLen;
	memcpy(d->uri, cfg->uri, uriLen);
	d->uri[uriLen] = '\0';
	d->uriLen = uriLen;
	// reset httpHeaderDec
	if(d->decHttpHead){
		dechh_reset(d->decHttpHead);
	}else{
		d->decHttpHead = dechh_new();
	}
}

void decws_destroy(struct tcp_decode_websocket* d)
{
	if(d->uri){
		delete[] d->uri;
		d->uri = nullptr;
	}
	if(d->decHttpHead){
		dechh_destroy(d->decHttpHead);
		d->decHttpHead = nullptr;
	}
	delete d;
}

#define STATE_OPCODE 1
#define STATE_LENGTH 2
#define STATE_MASK 3

static const int HEADER_KEY_NUM = 4;
static const char* HEADER_KEY[4] = { "Connection", "Upgrade",   "Sec-WebSocket-Version", "Sec-WebSocket-Key"};
static const char* HEADER_VAL[3] = { "Upgrade",    "websocket", "13" };
static const int HEADER_VAL_NUM = 3;

// -1: not done   0:has done(succ)  >0: has done(errCode)
int decws_conn_check(struct tcp_decode*dec, char* buf, int bufSize, int& readBytes)
{
	readBytes = 0;
	if(dec->connDone){  // conn has done
		return dec->connRet;
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	struct dec_http_head* decHead = d->decHttpHead;
	int ret = dechh_tick(decHead, buf, bufSize, readBytes);
	if (ret == 0) {  // http header check done, check ws info
		dechh_headerit_init(decHead);
		char* key = nullptr;
		char* val = nullptr;
		int keyLen, valLen;
		bool headUnCheck[HEADER_KEY_NUM] = { 1,1,1,1 };
		int validHeaderCnt = 0;
		char* secKeyVal = nullptr;
		int secKeyLen = 0;
		bool noOrigin = true;
		bool headerNotMatch;
		while( (key = dechh_headerit_next(decHead, &keyLen, &val, &valLen)) ){
			headerNotMatch = true;
			for (int i = 0; i < HEADER_KEY_NUM; ++i) {
				if(headUnCheck[i]){
					if(strcmp(key, HEADER_KEY[i]) == 0){   // key match
						if(i < HEADER_VAL_NUM){   // val must match
							if(strcmp(val, HEADER_VAL[i]) != 0){   //val not match
								dec->connDone = 1;
								dec->connRet = 400;  // means response status code(bad request)
								return dec->connRet;
							}
						}
						if(i == 3){  // Sec-WebSocket-Key, mark val
							secKeyVal = val;
							secKeyLen = valLen;
						}
						headUnCheck[i] = false;
						++validHeaderCnt;
						headerNotMatch = false;
						break;
					}
				}
			}
			if(headerNotMatch && noOrigin){  // check origin
				if(strcmp("Origin", key) == 0){
					noOrigin = false;
				}
			}
			if(validHeaderCnt >= HEADER_KEY_NUM && !noOrigin){   // all need headers found
				break;
			}
		}
		if(validHeaderCnt < HEADER_KEY_NUM){
			dec->connDone = 1;
			dec->connRet = 400;  // means response status code(bad request)
			return dec->connRet;
		}
		if(noOrigin){
			dec->connDone = 1;
			dec->connRet = 403;  // response status code
			return dec->connRet;
		}
		// check uri?

		// calc secKey
		if(d->bufSecKey && secKeyLen > d->secKeySaltPos){  // need expand buf
			delete[] d->bufSecKey;
			d->bufSecKey = nullptr;
		}
		if(d->bufSecKey == nullptr){
			d->bufSecKeyCap = secKeyLen + SEC_KEY_SALT_LEN;
			d->bufSecKey = new char[d->bufSecKeyCap];
			d->secKeySaltPos = secKeyLen;
			memcpy(d->bufSecKey, secKeyVal, secKeyLen);
			memcpy(d->bufSecKey + secKeyLen, SEC_KEY_SALT, SEC_KEY_SALT_LEN);
		}else{  // just cp secKey
			memcpy(d->bufSecKey + d->secKeySaltPos - secKeyLen, secKeyVal, secKeyLen);
		}
		uint8_t digest[SHA1_DIGEST_SIZE];
		ucrypt_sha1((uint8_t*)(d->bufSecKey + d->secKeySaltPos - secKeyLen), secKeyLen + SEC_KEY_SALT_LEN, digest);
		if (d->bufSecKeyRsp == nullptr) {
			int rspLen = ucrypt_b64encode_calcsz(SHA1_DIGEST_SIZE);
			d->bufSecKeyRspLen = rspLen;
			d->bufSecKeyRsp = new char[rspLen + 4];   // set as last header of response
			d->bufSecKeyRsp[rspLen] = '\r';
			d->bufSecKeyRsp[rspLen + 1] = '\n';
			d->bufSecKeyRsp[rspLen + 2] = '\r';
			d->bufSecKeyRsp[rspLen + 3] = '\n';
		}
		ucrypt_b64encode(digest, SHA1_DIGEST_SIZE, d->bufSecKeyRsp);
		//
		dec->connDone = 1;
		// init pack dec state
		d->state = STATE_OPCODE;
		return 0;
	}
	if (ret > 0) {  // conn error
		dec->connDone = 1;
		dec->connRet = 400;   // means response status code(bad request)
		return ret;
	}
	return -1;
}

static const char* RSP_LINE_SUCC = 
"HTTP/1.1 101 Switching Protocols\r\nServer: pipes\r\nConnection: upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: ";
static const int RSP_LINE_SUCC_LEN = 112;
static const char* RSP_LINE_403 =
"HTTP/1.1 403 Forbidden\r\n\r\n";
static const int RSP_LINE_403_LEN = 26;

static const char* RSP_LINE_400 = 
"HTTP/1.1 400 Bad Request\r\n\r\n";
static const int RSP_LINE_400_LEN = 28;

int decws_conn_rsp(struct tcp_decode*dec, void* ud, FN_DECODE_CONN_RSP_IMPL impl)
{
	if(!dec->connDone){   // error, assert?
		return 0;
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	//int len = strlen(RSP_LINE_400);   // debug
	//dec->connRet = 400;  // debug
	if(dec->connRet == 0){   // conn succ
		if(!impl(ud, RSP_LINE_SUCC, RSP_LINE_SUCC_LEN)){  // rsp failed
			return 0;
		}
		if(!impl(ud, d->bufSecKeyRsp, d->bufSecKeyRspLen + 4)){  //rsp failed
			return 0;
		}
	}else if(dec->connRet == 403){   // no origin
		if(!impl(ud, RSP_LINE_403, RSP_LINE_403_LEN)){
			return 0;
		}
	}else{
		if (!impl(ud, RSP_LINE_400, RSP_LINE_400_LEN)) {
			return 0;
		}
	}
	return 1;
}

int decws_pack_check(struct tcp_decode*dec, uint8_t* buf, int bufSize, int& checkedBytes)
{
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	int& state = d->state;
	int seek = 0;
	uint8_t ch;
	if(state == STATE_OPCODE){
		ch = buf[seek++];
		state == STATE_LENGTH;
	}
	if(state == STATE_LENGTH){
		ch = buf[seek++];
	}

	checkedBytes += seek;
	return 0;
}

