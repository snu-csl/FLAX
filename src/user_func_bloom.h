#ifndef _CSD_BLOOM_FUNC_H
#define _CSD_BLOOM_FUNC_H

#include "xxph3.h"
#include "user_func_rocksdb.h"

#define kMetadatalen 5

const char *kFilterBlockName = "fullfilter.rocksdb.BuiltinBloomFilter";

// FastRange32: Map a 32-bit hash value down to an arbitrary uint32_t range
// See https://github.com/lemire/fastrange
static inline uint32_t FastRange32(uint32_t hash, uint32_t range)
{
	uint64_t product = (uint64_t)range * hash;
	return (uint32_t)(product >> 32);
}

// Prepare hash by computing the byte offset within the filter data
// and prefetching the relevant cache line
static inline void PrepareHash(uint32_t h1, uint32_t len_bytes,
                               const char *data, uint32_t *byte_offset)
{
	uint32_t bytes_to_cache_line = FastRange32(h1, len_bytes >> 6) << 6;
	// __builtin_prefetch(data + bytes_to_cache_line, 0, 1);
	// __builtin_prefetch(data + bytes_to_cache_line + 63, 0, 1);
	*byte_offset = bytes_to_cache_line;
}

// Add hash to the bloom filter at the prepared cache line location
static inline void AddHashPrepared(uint32_t h2, int num_probes,
                                   char *data_at_cache_line)
{
	uint32_t h = h2;
	for (int i = 0; i < num_probes; ++i, h *= (uint32_t)0x9e3779b9) {
		// 9-bit address within 512 bit cache line
		int bitpos = h >> (32 - 9);
		data_at_cache_line[bitpos >> 3] |= ((uint8_t)1 << (bitpos & 7));
	}
}

// Check if a hash value may match in the bloom filter at the prepared cache line
// Returns true if all probed bits are set (may match), false otherwise (definitely no match)
static inline bool HashMayMatchPrepared(uint32_t h2, int num_probes,
                                        const char *data_at_cache_line)
{
	uint32_t h = h2;
	for (int i = 0; i < num_probes; ++i, h *= (uint32_t)0x9e3779b9) {
		// 9-bit address within 512 bit cache line
		int bitpos = h >> (32 - 9);
		if ((data_at_cache_line[bitpos >> 3] & ((uint8_t)1 << (bitpos & 7))) == 0) {
			return false;
		}
	}
	return true;
}

// Combined MayMatch: hash key and check bloom filter
// Returns true if key may exist, false if definitely not present
static inline bool BloomMayMatch(const char *key, int key_len,
                                 const char *filter_data, uint32_t filter_size)
{
	// Get num_probes from filter metadata (bottom 5 bits of byte at offset -3)
	int num_probes = filter_data[filter_size - 3] & 31;
	if (num_probes < 1 || num_probes > 30) {
		return true;  // Invalid, assume may match
	}

	// Filter data length excluding metadata
	uint32_t len = filter_size - kMetadatalen;

	// Hash the key
	uint64_t h = XXPH3_64bits(key, key_len);

	// Prepare: compute byte offset to cache line
	uint32_t byte_offset;
	PrepareHash(Lower32of64(h), len, filter_data, &byte_offset);

	// Check if hash may match
	return HashMayMatchPrepared(Upper32of64(h), num_probes, filter_data + byte_offset);
}

static inline uint64_t CalculateSpace(uint64_t num_entries, int bits_per_key)
{
	int millibits_per_key = (int)(bits_per_key * 1000);
	
	uint64_t raw_target_len = (uint64_t)((num_entries * millibits_per_key + 7999) / 8000);

	if (raw_target_len >= (uint64_t)(0xffffffc0)) {
		raw_target_len = (uint64_t)(0xffffffc0);
	}

	uint64_t tmp =(raw_target_len + 63) & ~((uint64_t)63);
	return ((raw_target_len + 63) & ~((uint64_t)63)) + kMetadatalen;
}

static inline int GetNumProbes(uint64_t num_entries, uint64_t len_with_metadata)
{
	uint64_t millibits = (uint64_t)(len_with_metadata - kMetadatalen) * 8000;
	int actual_millibits_per_key = (int)(millibits / num_entries);

	if (actual_millibits_per_key <= 2080) {
		return 1;
	} else if (actual_millibits_per_key <= 3580) {
		return 2;
	} else if (actual_millibits_per_key <= 5100) {
		return 3;
	} else if (actual_millibits_per_key <= 6640) {
		return 4;
	} else if (actual_millibits_per_key <= 8300) {
		return 5;
	} else if (actual_millibits_per_key <= 10070) {
		return 6;
	} else if (actual_millibits_per_key <= 11720) {
		return 7;
	} else if (actual_millibits_per_key <= 14001) {
		// Would be something like <= 13800 but sacrificing *slightly* for
		// more settings using <= 8 probes.
		return 8;
	} else if (actual_millibits_per_key <= 16050) {
		return 9;
	} else if (actual_millibits_per_key <= 18300) {
		return 10;
	} else if (actual_millibits_per_key <= 22001) {
		return 11;
	} else if (actual_millibits_per_key <= 25501) {
		return 12;
	} else if (actual_millibits_per_key > 50000) {
		// Top out at 24 probes (three sets of 8)
		return 24;
	} else {
		// Roughly optimal choices for remaining range
		// e.g.
		// 28000 -> 12, 28001 -> 13
		// 50000 -> 23, 50001 -> 24
		return (actual_millibits_per_key - 1) / 2000 - 1;
	}
}

void AddAllEntries(char *data, uint32_t len, int num_probes, uint64_t num_entries, uint64_t *hash_entry_ptr)
{
	const size_t kBufferMask = 7;

	uint32_t hashes[8];
	uint32_t byte_offsets[8];

	// Prime the buffer
	size_t i = 0;
	uint64_t *hash_entries_it = hash_entry_ptr;
	for (; i <= kBufferMask && i < num_entries; ++i) {
		uint64_t h = *hash_entries_it;
		PrepareHash(Lower32of64(h), len, data, /*out*/ &byte_offsets[i]);
		hashes[i] = Upper32of64(h);
		++hash_entries_it;
	}

	// Process and buffer
	for (; i < num_entries; ++i) {
		uint32_t *hash_ref = &hashes[i & kBufferMask];
		uint32_t *byte_offset_ref = &byte_offsets[i & kBufferMask];
		// Process (add)
		AddHashPrepared(*hash_ref, num_probes, data + *byte_offset_ref);
		// And buffer
		uint64_t h = *hash_entries_it;
		PrepareHash(Lower32of64(h), len, data, /*out*/ byte_offset_ref);
		*hash_ref = Upper32of64(h);
		++hash_entries_it;
	}

	// Finish processing
	for (i = 0; i <= kBufferMask && i < num_entries; ++i) {
		AddHashPrepared(hashes[i], num_probes, data + byte_offsets[i]);
	}
}

#endif
