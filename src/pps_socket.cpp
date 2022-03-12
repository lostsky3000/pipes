#include "pps_socket.h"

int32_t sock_slot_alloc(struct socket_mgr* mgr)
{
	if (mgr->ctxUsedNum.fetch_add(1, std::memory_order_relaxed) + 1 >= MAX_SOCK_CTX) {
		mgr->ctxUsedNum.fetch_sub(1, std::memory_order_relaxed);
		return -2;
	}
	uint32_t idx;
	if (mpmc_pop(mgr->queCtxFree, &idx)) {
		return idx;
	}
	if (mpmc_pop(mgr->queCtxOri, &idx)) {
		return idx;
	}
	// warning?
	mgr->ctxUsedNum.fetch_sub(1, std::memory_order_relaxed);
	return -1;
}
int sock_slot_free(struct socket_mgr* mgr, uint32_t idx)
{
	assert(mpmc_push(mgr->queCtxFree, idx));
	mgr->ctxUsedNum.fetch_sub(1, std::memory_order_relaxed);
	return 1;	
}

int sock_ctxsocks_init(struct socket_ctx* ctx, int sockCap)
{
	if (sockCap < 1) { // clear socks
		if(ctx->socks) {
			ctx->sockCap = 0;
			delete[] ctx->socks;
			ctx->socks = nullptr;
		}
		return 1;
	}
	if (ctx->socks && sockCap > ctx->sockCap) {
		ctx->sockCap = 0;
		delete[] ctx->socks;
		ctx->socks = nullptr;
	}
	if (ctx->socks == nullptr) {
		ctx->socks = new struct socket_sockctx[sockCap];
		ctx->sockCap = sockCap;
		for (int i = 0; i < sockCap; ++i) {
			ctx->socks[i].idx = i;
			ctx->socks[i].main = ctx;
		}
	}
	return 1;
}

//
static void dec_init(struct read_runtime* run, struct read_arg* arg, int decType)
{
	struct read_decode* d = run->arrDecode[decType];
	d->session = arg->session;
	d->type = decType;
	run->curDec = d;
}
int sockdec_init_now(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_NOW] == nullptr) {
		struct read_decode_now* d = new struct read_decode_now;
		run->arrDecode[DECTYPE_NOW] = (struct read_decode*)d;
	}
	dec_init(run, arg, DECTYPE_NOW);
	return 1;
}
int sockdec_init_len(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_LEN] == nullptr) {
		struct read_decode_len* d = new struct read_decode_len;
		run->arrDecode[DECTYPE_LEN] = (struct read_decode*)d;
	}
	dec_init(run, arg, DECTYPE_LEN);
	//
	struct dec_arg_len* da = (struct dec_arg_len*)arg->decArg;
	struct read_decode_len* d = (struct read_decode_len*)run->arrDecode[DECTYPE_LEN];
	d->readLen = da->readLen;
	return 1;
}
int sockdec_init_sep(struct read_runtime* run, struct read_arg* arg)
{
	if (run->arrDecode[DECTYPE_SEP] == nullptr) {
		struct read_decode_sep* d = new struct read_decode_sep;
		run->arrDecode[DECTYPE_SEP] = (struct read_decode*)d;
		d->sep = nullptr;
		d->sepCap = 0;
	}
	dec_init(run, arg, DECTYPE_SEP);
	//
	struct dec_arg_sep* da = (struct dec_arg_sep*)arg->decArg;
	struct read_decode_sep* d = (struct read_decode_sep*)run->arrDecode[DECTYPE_SEP];
	if (d->sepCap > 0 && da->sepLen > d->sepCap) {
		pps_free(d->sep);
		d->sep = nullptr;
		d->sepCap = 0;
	}
	if (d->sep == nullptr) {
		d->sepCap = da->sepLen;
		d->sep = (char*)pps_malloc(d->sepCap);
	}
	d->seekedBytes = 0;
	d->sepCharSeek = 0;
	d->sepLen = da->sepLen;
	//
	d->seekBuf = run->curReadBuf;
	d->seekBufPos = run->curReadBuf->size4Read;
	memcpy(d->sep, da->sep, d->sepLen);
	return 1;
}


//
static int read_now_inner(struct read_runtime* run, int readableOri, int* waitReadable, int* trunc)
{
	if (readableOri < 1) {   // no readable data
		*waitReadable = 1;
		return 0;
	}
	if (readableOri > run->readPackMax) {   // need trunc
		readableOri = run->readPackMax;
		*trunc = 1;
	} else {
		*trunc = 0;
	}
	//uint32_t fillBufIdx = run->fillBufIdx.load(std::memory_order_acquire);
	struct read_buf_block* buf = run->curReadBuf;
	int writeCnt = 0;
	int readable = readableOri;
	while (readable > 0) {
		int bufLeft = buf->cap - buf->size4Read;
		if (bufLeft < 1) {  // curReadBuf read done, change to next buf
			buf = buf->next;
			run->curReadBuf = buf;
			buf->size4Read = 0;
			bufLeft = buf->cap;
			run->readBufIdx.fetch_add(1, std::memory_order_release);
		}
		if (readable < bufLeft) {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, readable, run->udRead);
			buf->size4Read += readable;
			writeCnt += readable;
			readable = 0;
		} else {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, bufLeft, run->udRead);
			buf->size4Read = buf->cap;
			writeCnt += bufLeft;
			readable -= bufLeft;
		}
	}
	run->readable.fetch_sub(writeCnt, std::memory_order_release);
	return 1;
}
int sockdec_read_now(struct read_runtime* run, int* waitReadable, int*readableUsed, int* trunc)
{
	int readableOri = run->readable.load(std::memory_order_acquire);
	*readableUsed = readableOri;
	return read_now_inner(run, readableOri, waitReadable, trunc);
}
int sockdec_read_len(struct read_runtime* run, int* waitReadable, int*readableUsed, int* trunc)
{
	struct read_decode_len* d = (struct read_decode_len*)run->curDec;
	int readableOri = run->readable.load(std::memory_order_acquire);
	*readableUsed = readableOri;
	if (readableOri < d->readLen) {  // no readable data
		*waitReadable = d->readLen;
		return 0;
	}
	*trunc = 0;
	readableOri = d->readLen;
	struct read_buf_block* buf = run->curReadBuf;
	int writeCnt = 0;
	int readable = readableOri;
	while (readable > 0) {
		int bufLeft = buf->cap - buf->size4Read;
		if (bufLeft < 1) {   // curReadBuf read done, change to next buf
			buf = buf->next;
			run->curReadBuf = buf;
			buf->size4Read = 0;
			bufLeft = buf->cap;
			run->readBufIdx.fetch_add(1, std::memory_order_release);
		}
		if (readable < bufLeft) {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, readable, run->udRead);
			buf->size4Read += readable;
			writeCnt += readable;
			readable = 0;
		} else {
			run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, bufLeft, run->udRead);
			buf->size4Read = buf->cap;
			writeCnt += bufLeft;
			readable -= bufLeft;
		}
	}
	run->readable.fetch_sub(writeCnt, std::memory_order_release);
	return 1;
}

bool sockdec_sep_seek(struct read_decode_sep* d, int readableBytes)
{
	int& sepCharSeek = d->sepCharSeek;
	int& sepLen = d->sepLen;
	if(sepCharSeek == sepLen){  // already match
		return true;
	}
	int& seekedBytes = d->seekedBytes;
	int canSeekBytes = readableBytes - seekedBytes;
	if (canSeekBytes < sepLen - sepCharSeek) {
		return false;
	}
	int& seekBufPos = d->seekBufPos;
	char* sep = d->sep;
	struct read_buf_block* buf = d->seekBuf;
	while (--canSeekBytes >= 0) {
		if (seekBufPos >= buf->cap) {  // seek next buf
			buf = buf->next;
			d->seekBuf = buf;
			seekBufPos = 0;
		}
		char ch1 = buf->buf[seekBufPos++];
		char ch2 = sep[sepCharSeek];
		if (ch1 == ch2) {
			++sepCharSeek;
		} else {
			sepCharSeek = 0;
		}
		++seekedBytes;
		if (sepCharSeek == sepLen) {   // match found
			return true;
		}
	}
	return false;
}
static int read_sep_inner(struct read_runtime* run, int* waitReadable, int readableOri)
{
	struct read_decode_sep* d = (struct read_decode_sep*)run->curDec;
	//seek
	if (!sockdec_sep_seek(d, readableOri)) {  // match not found
		*waitReadable = readableOri + d->sepLen - d->sepCharSeek;
		return 0;
	}
	if (d->seekedBytes > run->readPackMax) {  // need trunc
		return 0;
	}
	// consume data
	readableOri = d->seekedBytes - d->sepLen;
	struct read_buf_block* buf = run->curReadBuf;
	int readable = readableOri;
	if (readable > 0) {
		int writeCnt = 0;
		while (readable > 0) {
			int bufLeft = buf->cap - buf->size4Read;
			if (bufLeft < 1) { // curReadBuf read done, change to next buf
				buf = buf->next;
				run->curReadBuf = buf;
				buf->size4Read = 0;
				bufLeft = buf->cap;
				run->readBufIdx.fetch_add(1, std::memory_order_release);
			}
			if (readable < bufLeft) {
				run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, readable, run->udRead);
				buf->size4Read += readable;
				writeCnt += readable;
				readable = 0;
			} else {
				run->cbRead(readableOri, writeCnt, buf->buf + buf->size4Read, bufLeft, run->udRead);
				buf->size4Read = buf->cap;
				writeCnt += bufLeft;
				readable -= bufLeft;
			}
		}
	} else {    // empty string
		run->cbRead(0, 0, nullptr, 0, run->udRead);
	}
	// consume sep
	buf = run->curReadBuf;
	readable = d->sepLen;
	while (readable > 0) {
		int bufLeft = buf->cap - buf->size4Read;
		if (bufLeft < 1) {  // curReadBuf read done, change to next buf
			buf = buf->next;
			run->curReadBuf = buf;
			buf->size4Read = 0;
			bufLeft = buf->cap;
			run->readBufIdx.fetch_add(1, std::memory_order_release);
		}
		if (readable < bufLeft) {
			buf->size4Read += readable;
			readable = 0;
		} else {
			buf->size4Read = buf->cap;
			readable -= bufLeft;
		}
	}
	run->readable.fetch_sub(d->seekedBytes, std::memory_order_release);
	//d->sepCharSeek = 0;
	//d->seekedBytes = 0;
	return 1;
}
int sockdec_read_sep(struct read_runtime* run, int* waitReadable, int*readableUsed, int* trunc)
{
	int readableOri = run->readable.load(std::memory_order_acquire);
	*readableUsed = readableOri;
	if (read_sep_inner(run, waitReadable, readableOri)) { // read succ & no trunc
		*trunc = 0;   // no trunc
		return 1;
	}
	// read nothing, check trunc
	if (readableOri > run->readPackMax) {   // can trunc
		assert(read_now_inner(run, readableOri, waitReadable, trunc));
		assert(*trunc == 1);  //  debug
		return 1;
	}
	return 0;
}



//
void sockmgr_init_main_thread(struct socket_mgr* mgr, struct pipes* pipes)
{
	mgr->pipes = pipes;
	mgr->ctxPoolCap = SOCK_CTX_SLOT_NUM;
	mgr->ctxPool = new struct socket_ctx[mgr->ctxPoolCap];
	mgr->queCtxFree = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(mgr->ctxPoolCap));
	mgr->queCtxOri = const_cast<struct mq_mpmc<uint32_t>*>(mpmc_create<uint32_t>(mgr->ctxPoolCap));
	for (uint32_t i = 0; i < mgr->ctxPoolCap; ++i) {
		struct socket_ctx* ctx = &mgr->ctxPool[i];
		//SOCK_RT_BEGIN(ctx);
		ctx->socks = nullptr;
		ctx->sockCap = 0;
		ctx->sockNum = 0;
		sock_ctxsocks_init(ctx, 0);
		ctx->parentIdx = -1;
		ctx->parentCnt = 0;
		// ctx init stuff
		ctx->idx = i;
		ctx->cnt.store(0);
		SOCK_RT_END(ctx);
		//
		//SOCK_RDRT_BEGIN(ctx);
		sock_read_initlock(ctx);
		struct read_runtime* run = &ctx->readRuntime;
		rdbuf_clear(&run->queReading);
		rdbuf_clear(&run->queFree);
		run->curDec = nullptr;
		for (int i = 0; i < TCP_DEC_NUM; ++i) {
			run->arrDecode[i] = nullptr;
		}
		run->tcpDec = nullptr;
		sock_read_atom_release(ctx);
		//
		struct send_runtime* srun = &ctx->sendRuntime;
		sock_send_initlock(ctx);
		assert(sock_send_trylock(ctx));
		sdbuf_clear(&srun->queUnSend);
		sdbuf_clear(&srun->queFree);
		sock_send_unlock(ctx);
		//
		mpmc_push(mgr->queCtxOri, i);
	}
	mgr->ctxUsedNum.store(0);
}

void sockmgr_deinit_main_thread(struct socket_mgr* mgr)
{
	for (uint32_t i = 0; i < mgr->ctxPoolCap; ++i) {
		struct socket_ctx* ctx = &mgr->ctxPool[i];
		// ctx deinit stuff
		SOCK_RT_BEGIN(ctx);
		sock_ctxsocks_init(ctx, 0);
		SOCK_RT_END(ctx);
	}
	delete[] mgr->ctxPool;
	//
	mpmc_destroy(mgr->queCtxFree);
	mpmc_destroy(mgr->queCtxOri);
}

