#ifndef _CSD_ROCKSDB_FUNC_H
#define _CSD_ROCKSDB_FUNC_H

#include "csd_user_func.h"

#define kMaxEncodedLength 20
#define kBlockTrailerSize 5
#define kNumInternalBytes 8
#define kNewVersionsEncodedLength 53
#define kFooterPart2Size 40
#define FormatVersion 5
const uint64_t kBlockBasedTableMagicNumber = 0x88e241b785f4cff7ull;

#define ROCKSDB_BLOCK_SIZE 4096

bool kLittleEndian = true;

struct decode_info {
	uint32_t shared;
	uint32_t non_shared;
	uint32_t value_length;
	char key[34];
	char value[5000];
};

inline uint32_t Upper32of64(uint64_t v)
{
	return (uint32_t)(v >> 32);
}
inline uint32_t Lower32of64(uint64_t v)
{
	return (uint32_t)(v);
}
char *GetVarint64Ptr(char *p, char *limit, uint64_t *value)
{
	uint64_t result = 0;
	for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
		uint64_t byte = *(uint8_t *)p;
		p++;
		if (byte & 128) {
			// More bytes are present
			result |= ((byte & 127) << shift);
		} else {
			result |= (byte << shift);
			*value = result;
			return (char *)p;
		}
	}
	return NULL;
}

char *GetVarint32Ptr(char *p, char *limit, uint32_t *value)
{
	if (p < limit) {
		uint32_t result = *(uint8_t *)p;
		if ((result & 128) == 0) {
			*value = result;
			return p + 1;
		}
	}

	// GetVarint32PtrFallback
	uint32_t result = 0;
	for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
		uint32_t byte = *(uint8_t *)p;
		p++;
		if (byte & 128) {
			// More bytes are present
			result |= ((byte & 127) << shift);
		} else {
			result |= (byte << shift);
			*value = result;
			return (char *)p;
		}
	}
	return NULL;
}

uint32_t DecodeFixed32(char *ptr)
{
	const uint8_t *const buffer = (const uint8_t *)ptr;
	if (kLittleEndian) {
		// Load the raw bytes
		uint32_t result;
		memcpy(&result, ptr, sizeof(result)); // gcc optimizes this to a plain load
		return result;
	} else {
		// Recent clang and gcc optimize this to a single mov / ldr instruction.
		return ((uint32_t)(buffer[0])) | ((uint32_t)(buffer[1]) << 8) | ((uint32_t)(buffer[2]) << 16) |
			   ((uint32_t)(buffer[3]) << 24);
	}
}

uint64_t DecodeFixed64(const char *ptr)
{
	if (kLittleEndian) {
		// Load the raw bytes
		uint64_t result;
		memcpy(&result, ptr, sizeof(result)); // gcc optimizes this to a plain load
		return result;
	} else {
		uint64_t lo = DecodeFixed32((char *)ptr);
		uint64_t hi = DecodeFixed32((char *)ptr + 4);
		return (hi << 32) | lo;
	}
}

char *DecodeEntry(char *p, char *limit, uint32_t *shared, uint32_t *non_shared, uint32_t *value_length)
{
	if (limit - p < 3)
		return NULL;
	*shared = ((uint8_t *)p)[0];
	*non_shared = ((uint8_t *)p)[1];
	*value_length = ((uint8_t *)p)[2];
	if ((*shared | *non_shared | *value_length) < 128) {
		// Fast path: all three values are encoded in one byte each
		p += 3;
	} else {
		if ((p = GetVarint32Ptr(p, limit, shared)) == NULL)
			return NULL;
		if ((p = GetVarint32Ptr(p, limit, non_shared)) == NULL)
			return NULL;
		if ((p = GetVarint32Ptr(p, limit, value_length)) == NULL)
			return NULL;
	}

	if ((limit - p) < (*non_shared + *value_length)) {
		return NULL;
	}
	return p;
}

char *EncodeVarint64(char *dst, uint64_t v)
{
	static const int B = 128;
	uint8_t *ptr = (uint8_t *)dst;
	while (v >= B) {
		*(ptr++) = v | B;
		v >>= 7;
	}
	*(ptr++) = (uint8_t)v;
	return (char *)ptr;
}

char *PutVarint64(char *dst, uint64_t v)
{
	char buf[10];
	char *ptr = EncodeVarint64(buf, v);
	memcpy(dst, buf, ptr - buf);
	return dst + (ptr - buf);
}

char *EncodeVarint32(char *dst, uint32_t v)
{
	// Operate on characters as unsigneds
	uint8_t *ptr = (uint8_t *)dst;
	static const int B = 128;
	if (v < (1 << 7)) {
		*(ptr++) = v;
	} else if (v < (1 << 14)) {
		*(ptr++) = v | B;
		*(ptr++) = v >> 7;
	} else if (v < (1 << 21)) {
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = v >> 14;
	} else if (v < (1 << 28)) {
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = (v >> 14) | B;
		*(ptr++) = v >> 21;
	} else {
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = (v >> 14) | B;
		*(ptr++) = (v >> 21) | B;
		*(ptr++) = v >> 28;
	}
	return (char *)ptr;
}

char *PutVarint32(char *dst, uint32_t v)
{
	char buf[5];
	char *ptr = EncodeVarint32(buf, v);
	memcpy(dst, buf, ptr - buf);
	return dst + (ptr - buf);
}

void EncodeFixed32(char *dst, uint32_t value)
{
	uint8_t *buffer = (uint8_t *)dst;

	buffer[0] = value & 0xff;
	buffer[1] = (value >> 8) & 0xff;
	buffer[2] = (value >> 16) & 0xff;
	buffer[3] = (value >> 24) & 0xff;
}

char *PutFixed32(char *dst, uint32_t value)
{
	if (kLittleEndian) {
		memcpy(dst, &value, sizeof(value));
	} else {
		char buf[sizeof(value)];
		EncodeFixed32(buf, value);
		memcpy(dst, buf, sizeof(value));
	}
	return dst + sizeof(value);
}

void EncodeFixed64(char *dst, uint64_t value)
{
	if (kLittleEndian) {
		memcpy(dst, &value, sizeof(value));
	} else {
		dst[0] = value & 0xff;
		dst[1] = (value >> 8) & 0xff;
		dst[2] = (value >> 16) & 0xff;
		dst[3] = (value >> 24) & 0xff;
		dst[4] = (value >> 32) & 0xff;
		dst[5] = (value >> 40) & 0xff;
		dst[6] = (value >> 48) & 0xff;
		dst[7] = (value >> 56) & 0xff;
	}
}

#endif