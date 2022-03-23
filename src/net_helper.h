#ifndef NET_HELPER_H
#define NET_HELPER_H

#include <cstdint>


struct net_helper
{
	struct wrap_buf* bufWsSend;
	struct wrap_buf* bufRdsSend;
};

struct nethp_rdssend_ctx
{
	int itemTotal;
	int itemCnt;
	int szOut;
	char* bufOut;
};

char* nethp_wrap_wssend(struct net_helper* h, int op, const char* data, uint32_t szData, int* szOut);

//
inline int nethp_wrap_rdssend_ctx_init(struct nethp_rdssend_ctx* ctx, int itemTotal)
{
	ctx->itemTotal = itemTotal;
	ctx->itemCnt = 0;
	ctx->szOut = 0;
	ctx->bufOut = nullptr;
	return 1;
}
int nethp_wrap_rdssend_helper_init(struct net_helper* h);
int nethp_wrap_rdssend(struct net_helper* h, struct nethp_rdssend_ctx* ctx, const char* item, int szItem);
char* nethp_rdssendbuf(struct net_helper* h, int* szOut);
//
int nethp_init(struct net_helper* h);
void nethp_deinit(struct net_helper* h);



#endif // !NET_HELPER_H



