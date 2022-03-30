#ifndef UTIL_CRYPT_H
#define UTIL_CRYPT_H

#include <cstddef>
#include <cstdint>

#define SHA1_DIGEST_SIZE 20
int ucrypt_sha1(uint8_t* data, size_t szData, uint8_t* digest);

inline int ucrypt_b64encode_calcsz(int szData)
{
	return (szData + 2) / 3 * 4;
}
int ucrypt_b64encode(uint8_t* data, int szData, char*bufOut);

#endif // !UTIL_SHA1_H

