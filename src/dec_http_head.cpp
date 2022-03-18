#include "dec_http_head.h"

static const char* DECHH_VER_LINE = "HTTP/1.1\r\n";
static const int DECHH_VER_LINE_LEN = 10;

static inline int on_error(struct dec_http_head*d, int err)
{
	d->state = DECHH_STATE_ERROR;
	d->errCode = err;
	return err;
}
static inline bool url_ch_valid(char ch)
{
	return ch >= 33 && ch <= 126;
}
static inline bool header_ch_valid(char ch)
{
	return ch >= 32 && ch <= 126;
}

static inline void expand_buf(struct dec_http_head*d)
{
	char* oldBuf = d->buf;
	int oldBufSize = d->szBuf;
	int newSize = oldBufSize + 256;
	char* newBuf = new char[newSize];
	if(d->widxBuf > 0){   // has old data, cp
		memcpy(newBuf, oldBuf, d->widxBuf);
	}
	delete[] oldBuf;
	d->szBuf = newSize;
	d->buf = newBuf;
}


// return:  -1: not done   0:has done(succ)  >0: has done(errCode)
int dechh_tick(struct dec_http_head*d, const char* buf, int bufSize, int& readBytes)
{
	readBytes = 0;
	int8_t& state = d->state;
	if (state == DECHH_STATE_DONE) {
		return 0;
	} 
	if (state == DECHH_STATE_ERROR) {
		return d->errCode;
	}
	// check tmpBuf size
	char ch;
	int seek = 0;
	int& fieldLenCnt = d->fieldLenCnt;
	if(state == DECHH_STATE_METHOD){
		while(seek < bufSize){
			ch = buf[seek++];
			if(fieldLenCnt > 0){   // not 1st ch
				if (ch != ' ') {
					if (fieldLenCnt >= d->methodPtrnLen) {
						readBytes += seek;
						return on_error(d, DECHH_ERR_METHOD_INVALID);
					}
					if (ch == d->methodPtrn[fieldLenCnt]) {
						d->methodStr[fieldLenCnt] = ch;
					} else {
						readBytes += seek;
						return on_error(d, DECHH_ERR_METHOD_INVALID);
					}
				} else {
					if (fieldLenCnt != d->methodPtrnLen) {
						readBytes += seek;
						return on_error(d, DECHH_ERR_METHOD_INVALID);
					}
					// method done
					state = DECHH_STATE_URL;
					fieldLenCnt = 0;
					break;
				}
				++fieldLenCnt;
			}else{  // 1st ch
				for (int i = 0; i < sizeof(DECHH_METHOD) / sizeof(char*); ++i) {
					const char* ptrn = DECHH_METHOD[i];
					if (ch == ptrn[0]) {  // 1st ch match
						d->methodPtrn = (char*)ptrn;
						d->methodPtrnLen = DECHH_METHOD_LEN[i];
						d->method = (int16_t)i;
						break;
					}
				}
				if (!d->methodPtrn) {  // pattern not found
					readBytes += seek;
					return on_error(d, DECHH_ERR_METHOD_INVALID);
				}
				d->methodStr[fieldLenCnt++] = ch;
			}
		}
	}
	int& idxWrite = d->widxBuf;
	if(state == DECHH_STATE_URL){
		while(seek < bufSize){
			ch = buf[seek++];
			if(ch != ' '){
				if(!url_ch_valid(ch)){
					readBytes += seek;
					return on_error(d, DECHH_ERR_URL_INVALID);
				}
				d->buf[idxWrite] = ch;
			}else{
				if(idxWrite == 0){   // 1st ch invalid
					readBytes += seek;
					return on_error(d, DECHH_ERR_URL_INVALID);
				}
				// url done
				d->urlLen = idxWrite;
				d->buf[idxWrite++] = '\0';
				state = DECHH_STATE_VER;
				fieldLenCnt = 0;
				break;
			}
			if(++idxWrite > DECHH_URL_LEN_MAX){
				readBytes += seek;
				return on_error(d, DECHH_ERR_URL_INVALID);
			}
		}
	}
	if(state == DECHH_STATE_VER){
		while(seek < bufSize){
			if(buf[seek++] != DECHH_VER_LINE[fieldLenCnt]){
				readBytes += seek;
				return on_error(d, DECHH_ERR_VER_INVALID);
			}
			if(++fieldLenCnt == DECHH_VER_LINE_LEN){
				state = DECHH_STATE_HEADER;
				//
				d->headState = 2;  // means check header end
				fieldLenCnt = 1;
				break;
			}
		}
	}
	if(state == DECHH_STATE_HEADER){
		int8_t& headState = d->headState;
		int& headLenCnt = d->headLenCnt;
		int& szBuf = d->szBuf;
		while(seek < bufSize){
			ch = buf[seek++];
			// 0: reading key  1: reading val  2: checking header end
			if(headState == 0){  // reading key
				if (ch != ':') {  // ch of key
					if (!header_ch_valid(ch)) {
						readBytes += seek;
						return on_error(d, DECHH_ERR_HEADER_INVALID);
					}
					if(fieldLenCnt == 0){  // 1st ch of key
						d->tmpPos = idxWrite;
						idxWrite += 2;     // 2bytes for keylen
					}
					d->buf[idxWrite++] = ch;
					++fieldLenCnt;
				} else {   // reading key done
					if(fieldLenCnt == 0){  // key is empty
						readBytes += seek;
						return on_error(d, DECHH_ERR_HEADER_INVALID);
					}
					d->buf[idxWrite++] = '\0';
					// set key len
					*((uint16_t*)(d->buf + d->tmpPos)) = fieldLenCnt;
					// change to reading val
					headState = 1;  
					fieldLenCnt = 0;
				}
			}else if(headState == 1){  // reading val
				if(ch != '\r'){  // val body
					if (!header_ch_valid(ch)) {
						readBytes += seek;
						return on_error(d, DECHH_ERR_HEADER_INVALID);
					}
					if(fieldLenCnt != 0 || ch != ' '){  
						if (fieldLenCnt == 0) {  // 1st valid ch of val
							d->tmpPos = idxWrite;
							idxWrite += 2;     // 2bytes for vallen
						}
						d->buf[idxWrite++] = ch;
						++fieldLenCnt;
					}else{  // skip leadingspace
						
					}
				}else{  
					if (fieldLenCnt == 0) {  // val is empty
						readBytes += seek;
						return on_error(d, DECHH_ERR_HEADER_INVALID);
					}
					d->buf[idxWrite++] = '\0';
					//set val len
					*((uint16_t*)(d->buf + d->tmpPos)) = fieldLenCnt;
					// incr headerNum
					++d->headerNum;
					// change to checking headerline end
					headState = 2;
					fieldLenCnt = 0;
				}
			}else{   // checking header end
				if(fieldLenCnt == 0){
					if (ch != '\n') {   // headerline end, ch must be \n
						readBytes += seek;
						return on_error(d, DECHH_ERR_HEADER_INVALID);
					}
					++fieldLenCnt;
				}else if(fieldLenCnt == 1){
					if(ch != '\r'){    // change to reading key
						--headLenCnt;
						--seek;
						headState = 0;
						fieldLenCnt = 0;
					}else{   // all headers end signal
						++fieldLenCnt;
					}
				}else{  // all headers end
					if(ch != '\n'){    // must be \n
						readBytes += seek;
						return on_error(d, DECHH_ERR_HEADER_INVALID);
					}
					// all done
					++headLenCnt;
					state = DECHH_STATE_DONE;
					readBytes += seek;
					return 0;
				}
			}
			if(++headLenCnt > DECHH_HEADER_LEN_MAX){  // headers too long
				readBytes += seek;
				return on_error(d, DECHH_ERR_HEADER_INVALID);
			}
			if(idxWrite >= szBuf){   // need expand buf
				expand_buf(d);
			}
		}
	}
	readBytes += seek;
	return -1;
}
