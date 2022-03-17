#ifndef PPS_SYSINFO_H
#define PPS_SYSINFO_H
#include <cstdint>

uint32_t sysinfo_core_num();

const uint8_t ENDIAN_CHECK[2] = { 0, 1 };
const bool IS_BIGENDIAN = *((uint16_t*)ENDIAN_CHECK) == 1;

#endif // !PPS_SYSINFO_H

