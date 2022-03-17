#ifndef PPS_SYSAPI_H
#define PPS_SYSAPI_H

#include <cstdint>
#include <chrono>

struct time_clock
{
	std::chrono::time_point<std::chrono::steady_clock> tm;
	//
	uint64_t operator-(const struct time_clock& o) {
		using namespace std::chrono;
		return duration_cast<milliseconds>(tm - o.tm).count();
	}
};

inline void sysapi_clock_now(struct time_clock& tm)
{
	using namespace std::chrono;
	tm.tm = steady_clock::now();
}

inline int64_t sysapi_clock_now_ms(struct time_clock& tmBegin)
{
	using namespace std::chrono;
	time_point<steady_clock> tmNow = steady_clock::now();
	return duration_cast<milliseconds>(tmNow - tmBegin.tm).count();
}

uint64_t sysapi_clock_since_epoch(struct time_clock& tm);
uint64_t sysapi_system_now();


#endif // !PPS_SYSAPI_H

