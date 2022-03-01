
#include "pps_sysapi.h"

uint64_t sysapi_clock_since_epoch(struct time_clock& tm)
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(tm.tm.time_since_epoch()).count();
}


uint64_t sysapi_system_now()
{
	using namespace std::chrono;
	auto dur = system_clock::now().time_since_epoch();
	return duration_cast<milliseconds>(dur).count();
}

/*
uint64_t sysapi_clock_now_micro()
{
using namespace std::chrono;
auto dur = steady_clock::now().time_since_epoch();
return duration_cast<microseconds>(dur).count();
}
*/

