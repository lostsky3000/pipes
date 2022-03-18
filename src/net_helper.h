#ifndef NET_HELPER_H
#define NET_HELPER_H

#include <cstdint>


struct net_helper
{
	struct wrap_buf* bufWsSend;
};


char* nethp_wrap_wssend(struct net_helper* h, int op, const char* data, uint32_t szData, int* szOut);

//
int nethp_init(struct net_helper* h);
void nethp_deinit(struct net_helper* h);



#endif // !NET_HELPER_H



