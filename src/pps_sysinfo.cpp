
#include "pps_sysinfo.h"

#include <thread>


uint32_t sysinfo_core_num()
{
	return std::thread::hardware_concurrency();
}


/*

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#define _WIN32_WINNT 0x0601

#include <windows.h>

uint32_t
sysinfo_ncore() {
	// see https://devblogs.microsoft.com/oldnewthing/20200824-00/?p=104116
	return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

#elif defined(__APPLE__)

#include "unistd.h"

uint32_t
sysinfo_ncore() {
	return sysconf(_SC_NPROCESSORS_ONLN);
}

#else

#include <sys/sysinfo.h>

uint32_t
sysinfo_ncore() {
	return get_nprocs();
}

#endif

*/


