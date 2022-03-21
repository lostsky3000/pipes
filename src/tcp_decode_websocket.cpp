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
	d->head.packNeedDecode = 1;
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

#define STATE_ERR -1
#define STATE_PACK_HEAD 1
#define STATE_PACK_BODY 2

static const int HEADER_KEY_NUM = 4;
static const char* HEADER_KEY[4] = { "Connection", "Upgrade",   "Sec-WebSocket-Version", "Sec-WebSocket-Key"};
static const char* HEADER_VAL[3] = { "Upgrade",    "websocket", "13" };
static const int HEADER_VAL_NUM = 3;

// -1: not done   0:has done(succ)  >0: has done(errCode)
int decws_conn_check(struct tcp_decode*dec, char* buf, int bufSize, int& readBytes)
{
	readBytes = 0;
	int8_t& decState = dec->state;
	if(decState == TCPDEC_STATE_CONN_DONE){  // conn has done
		return dec->errCode;
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	struct dec_http_head* decHead = d->decHttpHead;
	int ret = dechh_tick(decHead, buf, bufSize, readBytes);
	if (ret == 0) {  // http header check done, check ws info
		if(dechh_method(decHead) != DECHH_METHOD_GET){   // method is not GET
			decState = TCPDEC_STATE_CONN_DONE;
			dec->errCode = 400;  // means response status code(bad request)
			return dec->errCode;
		}
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
								decState = TCPDEC_STATE_CONN_DONE;
								dec->errCode = 400;  // means response status code(bad request)
								return dec->errCode;
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
			decState = TCPDEC_STATE_CONN_DONE;
			dec->errCode = 400;  // means response status code(bad request)
			return dec->errCode;
		}
		if(noOrigin){
			decState = TCPDEC_STATE_CONN_DONE;
			dec->errCode = 403;  // response status code
			return dec->errCode;
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
		decState = TCPDEC_STATE_CONN_DONE;
		dec->errCode = 0;

		return 0;
	}
	if (ret > 0) {  // conn error
		decState = TCPDEC_STATE_CONN_DONE;
		dec->errCode = 400;   // means response status code(bad request)
		return dec->errCode;
	}
	return -1;
}


// =======================begin, do not modify !!!
static const char* RSP_LINE_SUCC = 
"HTTP/1.1 101 Switching Protocols\r\nServer: pipes\r\nConnection: upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: ";
static const int RSP_LINE_SUCC_LEN = 112;
static const char* RSP_LINE_403 =
"HTTP/1.1 403 Forbidden\r\n\r\n";
static const int RSP_LINE_403_LEN = 26;
static const char* RSP_LINE_400 = 
"HTTP/1.1 400 Bad Request\r\n\r\n";
static const int RSP_LINE_400_LEN = 28;
// =======================end, do not modify !!!


int decws_conn_rsp(struct tcp_decode*dec, void* ud, FN_DECODE_RSP_IMPL impl)
{
	if(dec->state == TCPDEC_STATE_CONN_CHECKING ){   // error, assert?
		return 0;
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	//int len = strlen(RSP_LINE_400);   // debug
	//dec->connRet = 400;  // debug
	if(dec->errCode == 0){   // conn succ
		if(!impl(ud, RSP_LINE_SUCC, RSP_LINE_SUCC_LEN)){  // rsp failed
			return 0;
		}
		if(!impl(ud, d->bufSecKeyRsp, d->bufSecKeyRspLen + 4)){  //rsp failed
			return 0;
		}
	}else if(dec->errCode == 403){   // no origin
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


static const uint8_t ENDIAN_CHECK[2] = { 0, 1 };
static const bool IS_BIGENDIAN = *((uint16_t*)ENDIAN_CHECK) == 1;

#define MASK_OPCODE 15 // 1111
#define MASK_LEN 127   // 0111 1111

static inline int on_error(struct tcp_decode*dec, int err)
{
	dec->errCode = err;
	return err;
}
int decws_pack_reset(struct tcp_decode*dec)
{
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	dec->state = TCPDEC_STATE_PACK_HEAD;
	d->packMetaRcvd = 0;
	memset(d->packMeta, 0, sizeof(d->packMeta) / sizeof(uint8_t));
	return 1;
}
// return:  -1:not done   0:done(succ)   >0:done(errCode)
int decws_pack_head(struct tcp_decode*dec, uint8_t* buf, int bufSize, int& checkedBytes)
{
	checkedBytes = 0;
	int8_t& state = dec->state;
	if(state != TCPDEC_STATE_PACK_HEAD){
		return on_error(dec, 1);
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	uint8_t& metaRcvd = d->packMetaRcvd;
	uint8_t* bufMeta = d->packMeta;
	int seek = 0;
	while (seek < bufSize) {
		if (metaRcvd == 1) {  // 1st of len, check len
			uint8_t ch = buf[seek++];
			uint8_t len = ch & MASK_LEN;
			bufMeta[metaRcvd] = ch;
			if (len < 126) {
				metaRcvd += 9;
			} else if (len == 126) {
				metaRcvd += 7;
			} else if (len == 127) {
				++metaRcvd;
			} else if (len > 126) {  // invalid
				checkedBytes += seek;
				return on_error(dec, 2);
			}
		} else if (metaRcvd > 1 && metaRcvd < 10) {   // reading len
			if (IS_BIGENDIAN) {
				bufMeta[metaRcvd++] = buf[seek++];
			} else {
				bufMeta[11 - metaRcvd] = buf[seek++];
				++metaRcvd;
			}
		} else {
			bufMeta[metaRcvd] = buf[seek++];
			if (++metaRcvd == 14) {   // read pack-head done
				if (!(bufMeta[1] >> 7)) {  // no mask from client, error
					checkedBytes += seek;
					return on_error(dec, 3);
				}
				d->curOpCode = bufMeta[0] & MASK_OPCODE;
				if(d->curOpCode == 1 || d->curOpCode == 2){  // has payload data
					uint8_t lenFlag = bufMeta[1] & MASK_LEN;
					if (lenFlag < 126) {
						dec->curPackBodyLen = lenFlag;
					} else if (lenFlag == 126) {
						dec->curPackBodyLen = *(uint16_t*)(bufMeta + 6);
					} else {
						dec->curPackBodyLen = *(uint64_t*)(bufMeta + 2);
					}
					// change state
					state = TCPDEC_STATE_PACK_DEC_BODY;
					d->bodyDecCnt = 0;
				}else {  // control frame, close, pong etc..
					state = TCPDEC_STATE_INNER_MSG;
					dec->innerMsg = d->curOpCode;
				}
				checkedBytes += seek;
				return 0;
			}
		}
	}
	checkedBytes += seek;
	return -1;
}

int decws_pack_dec_body(struct tcp_decode*dec, uint8_t* buf, int bufSize, int& checkedBytes)
{
	checkedBytes = 0;
	int8_t& state = dec->state;
	if(state != TCPDEC_STATE_PACK_DEC_BODY){
		return 1;
	}
	struct tcp_decode_websocket* d = (struct tcp_decode_websocket*)dec;
	uint32_t& byteCnt = d->bodyDecCnt;
	uint32_t bodyLen = dec->curPackBodyLen;
	uint8_t* bufMeta = d->packMeta;
	int seek = 0;
	while(seek < bufSize){
		buf[seek] = (bufMeta[10 + byteCnt % 4]) ^ buf[seek];
		++seek;
		if(++byteCnt == bodyLen){  // done
			checkedBytes += seek;
			state = TCPDEC_STATE_PACK_READ_BODY;
			dec->curPackBodyReadCnt = 0;
			return 0;
		}
	}
	checkedBytes += seek;
	return -1;
}
