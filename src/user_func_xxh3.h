#ifndef _CSD_XXH3_FUNC_H
#define _CSD_XXH3_FUNC_H

#define XXH_NO_STDLIB
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION

#include "xxhash.h"
#include "user_func_rocksdb.h"

uint32_t ModifyChecksumForLastByte(uint32_t checksum, char last_byte)
{
	// This strategy bears some resemblance to extending a CRC checksum by one
	// more byte, except we don't need to re-mix the input checksum as long as
	// we do this step only once (per checksum).
	const uint32_t kRandomPrime = 0x6b9083d9;
	return checksum ^ (uint8_t)(last_byte)*kRandomPrime;
}

uint32_t ChecksumModifierForContext(uint32_t base_context_checksum, uint64_t offset)
{
	uint32_t all_or_nothing = (uint32_t)(0) - (base_context_checksum != 0);

	uint32_t modifier = base_context_checksum ^ (Lower32of64(offset) + Upper32of64(offset));

	return modifier & all_or_nothing;
}

void set_xxh3_to_block(char *output, size_t size, size_t offset, char *trailer)
{
	memset(trailer, 0, kBlockTrailerSize);
	trailer[0] = 0; // Type KNoCompression
	uint32_t v = Lower32of64(XXH3_64bits(output, size));
	uint32_t checksum = ModifyChecksumForLastByte(v, 0);
	checksum += ChecksumModifierForContext(0, offset);
	EncodeFixed32(trailer + 1, checksum);
}

#endif
