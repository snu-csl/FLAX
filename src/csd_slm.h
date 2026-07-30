#ifndef _NVMEVIRT_CSD_SLM_H
#define _NVMEVIRT_CSD_SLM_H

#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "buddy.h"

#define ALIGNED_DOWN(value, align) ((value) & ~((align)-1))
#ifndef DIVIDE_UP
#define DIVIDE_UP(value, divisor) (((value) + (divisor)-1) / (divisor))
#endif
#define _SLM_DEBUG_ (0)
static const unsigned int SLM_PAGE_SIZE = 4 * 1024; // tunable.
#define SLM_PAGE_SHIFT 12

/*
 * Head/tail offset dependency tracking — the SOLE readiness mechanism.
 *
 * Every region tracks two region-relative byte offsets in its slm_lba_info:
 *   ht_head : contiguous "produced" frontier (loaded / written)
 *   ht_tail : "consumed" frontier (advanced by the compute consumer)
 * Magic/circular rings keep these in MONOTONIC LOGICAL bytes; the magic-read
 * demand path layers a per-extent extents[].ready on top once a region flips
 * to extent_mapped.
 *
 * USE_HEAD_TAIL_DEP is retained only to scope the head/tail-specific
 * declarations below. 0 is unsupported (it would leave regions with no
 * readiness mechanism and hang); enforced below.
 */
#define USE_HEAD_TAIL_DEP (1)
#if (USE_HEAD_TAIL_DEP != 1)
#error "USE_HEAD_TAIL_DEP must be 1: head/tail is the only readiness mechanism."
#endif
/* Out-of-order completed intervals awaiting coalesce into the head frontier.
 * Bounded in practice by the loader's look-ahead throttle window (a few
 * in-flight IO_REQUEST_SIZE chunks); sized generously so overflow is an
 * invariant violation, not a config-dependent panic. */
#define HT_MAX_PENDING 256

struct ht_interval {
	size_t start;
	size_t end;
};

// Circular buffer constants for magic compaction limited memory operation
// MAGIC_BUFFER_SIZE must be a power of 2 for efficient bitmasking
#define MAGIC_BUFFER_SIZE (SLM_PAGE_SIZE * 32768)  // 128MB = 4KB * 32768 (power of 2)
#define MAGIC_BUFFER_MASK (MAGIC_BUFFER_SIZE - 1) // Bitmask for fast modulo: offset & MASK
#define MAGIC_BUFFER_PAGES (MAGIC_BUFFER_SIZE >> SLM_PAGE_SHIFT)  // Number of pages in buffer
/*
 * Producer->consumer notification grain for circular (magic) rings, decoupled
 * from SLM_PAGE_SIZE. Ring producers publish ht_head and ring consumers
 * advance ht_tail once per GRAIN instead of once per SLM page. Each publish
 * costs ~1-2us on the producer's hot path (shared-cacheline RFO against the
 * spinning downstream consumer + notify bookkeeping), so a 4K SLM page paid
 * that cost 4x more often than a 16K page and slowed entry-bound (small-value)
 * compactions by ~25%. Readiness itself stays page-granular and byte-exact at
 * finalize; a coarser grain only delays mid-stream visibility by < GRAIN
 * bytes, which the 128MB rings absorb.
 * Must be a power of 2 and a multiple of SLM_PAGE_SIZE.
 */
#define HT_NOTIFY_GRAIN (16 * 1024)
/* 
 * Ghost page for wrap-around reads.
 * A 4K ghost page suffices for most LSM-KVS workloads, but since we
 * evaluate 4K values (which can span beyond a 4K ghost page) we set 
 * the ghost page size to 8K. Note that 4K is enough for our 128B 
 * evaluations.
 */
#define GHOST_PAGE_SIZE (8 * 1024)
/* Must cover the largest contiguous access that can straddle the ring wrap:
 * the finalized data block (block_size + last entry + 8B restart interval)
 * read whole by the CRC path. Runtime-checked via WARN_GHOST_OVERRUN at the
 * wrap-straddling read sites in csd_user_func.c. */
#define WARN_GHOST_OVERRUN(len) \
	do { \
		if (unlikely((size_t)(len) > (size_t)GHOST_PAGE_SIZE)) \
			printk(KERN_WARNING "[GHOST] contiguous span %zu exceeds GHOST_PAGE_SIZE %d: wrap-straddling read is corrupt\n", \
			       (size_t)(len), GHOST_PAGE_SIZE); \
	} while (0)

// Extent-based remapping for magic read (on-demand block loading)
#define MAX_EXTENT_ENTRIES 16
#define MAGIC_READ_BUFFER_PAGES (4096) // 16MB = 4KB * 4096
#define MAGIC_READ_BUFFER_SIZE (SLM_PAGE_SIZE * MAGIC_READ_BUFFER_PAGES)

struct extent_map_entry {
	size_t file_offset;   /* Offset within the logical file */
	size_t file_size;     /* Size of the mapped region */
	size_t slm_offset;    /* Offset within the SLM buffer where this extent lives */
	bool ready;           /* true once the whole extent's demand IO has landed
	                       * (per-extent readiness for the head/tail->demand
	                       * path) */
};

typedef int(init_fn)(size_t size);
typedef size_t(allocate_fn)(size_t length, void *args);
typedef int(deallocate_fn)(size_t mem_offset);
typedef size_t(size_fn)(size_t mem_offset);
typedef void(status_fn)(void);
typedef void(kill_fn)(void);

struct allocator_ops {
	init_fn *init;
	allocate_fn *allocate;
	deallocate_fn *deallocate;
	size_fn *size;
	size_fn *act_size;
	status_fn *status;
	kill_fn *kill;
};

struct slm_physical_resource {
	uint64_t start_addr;
	struct allocator_ops allocator;
};

bool check_allocated_slm_range(size_t addr, size_t size);
size_t alloc_slm_range(size_t size);
uint32_t get_task_id_from_slm_lba_info(size_t addr);
size_t get_slm_range_size(size_t addr);
void free_slm_range(size_t addr);

void print_slm_info(void);

void copy_to_slm(size_t dest, void *src, size_t size);
void copy_from_slm(void *dest, size_t src, size_t size);

void init_slm_memory(void *start, size_t slm_size);
void final_slm_memory(void);

size_t get_slm_offset(size_t addr);
size_t get_slm_addr(size_t offset);

// Async Computational Commands
struct slm_lba_info {
	uint64_t start_addr;       // physical SLM start address
	size_t len;                // for circular: physical buffer size, otherwise: logical size
	int nentry;
	struct source_range_entry *sre;
	uint64_t demand_read_offset;
	size_t demand_read_size;
	bool request_done;
	bool random_access;
	uint32_t task_id;
	uint64_t next_addr;
	bool is_output;
	uint64_t final_offset;
	size_t final_leftovers;
	bool stream_access;

	// Circular buffer support for magic compaction (INPUT)
	bool is_circular;              // true if this is a circular buffer
	size_t logical_total_size;     // total logical size of the input data (original file size)
	bool load_complete;            // true when all data has been loaded
	size_t compute_logical_offset; // logical offset where computation has reached (for throttling)

	// Circular OUTPUT buffer support
	bool output_is_circular;              // true if output uses circular buffer
	size_t host_consumed_logical_offset;  // how much host has read (logical offset)
	size_t output_logical_total_size;     // final output size (set when finalized)

	// Extent-based remapping for magic read (entered from head/tail on a
	// scattered probe; see slm_request_demand_read_info).
	bool extent_mapped;                   // true if extent remapping is actively in use
	int extent_count;                     // number of active extent entries
	int extent_next_free_page;            // next free page index in the SLM buffer
	struct extent_map_entry extents[MAX_EXTENT_ENTRIES];
	size_t demand_read_slm_target;        // SLM target addr for demand read IO

	// Head/tail offset dependency tracking (the readiness mechanism for every
	// region). All offsets are region-relative bytes.
	bool use_head_tail;                   // set on every load/output region
	size_t ht_head;                       // contiguous produced frontier
	size_t ht_tail;                       // consumed frontier
	bool ht_producer_done;                // producer finished; output_logical_total_size is final
	int ht_pending_cnt;                   // out-of-order intervals awaiting coalesce
	struct ht_interval ht_pending[HT_MAX_PENDING];
};

struct circular_output_buf_ctx {
	size_t base_addr;              /* Physical base address of circular buffer */
	size_t base_page;              /* Base page index (precomputed for fast access) */
	size_t logical_write_offset;   /* Current logical write position (monotonic) */
	size_t prev_logical_offset;    /* Previous offset for page notification tracking */
	struct slm_lba_info *lba_info; /* For host consumption tracking */
	const char *debug_tag;         /* Debug tag for differentiating contexts (e.g., "COMPACT", "CRC") */
};

struct circular_buf_ctx {
	size_t base_addr;           /* Physical base address of circular buffer */
	size_t logical_offset;      /* Current logical offset into data */
	size_t logical_total_size;  /* Total logical size of data */
	size_t prev_logical_offset; /* Previous offset for compute_ready notification */
	struct slm_lba_info *lba_info; /* For checking if all data is loaded */
	const char *debug_tag;      /* Debug tag for differentiating contexts (e.g., "COMPACT", "CRC") */
};


/* Page -> owning slm_lba_info back-pointer map. */
struct slm_data_ready_info {
	struct slm_lba_info *slm_lba_info;
};

extern struct slm_data_ready_info *slm_data_ready_info;

void reserve_slm_memory(uint64_t start_addr, size_t len, struct slm_lba_info *slm_lba_info_ptr);
void set_slm_ready_for_csd(uint64_t start_addr, size_t len, struct slm_lba_info *info);
bool check_slm_data_ready(uint64_t start_sddr, size_t len, bool log);
bool check_slm_data_ready_info(uint64_t start_addr, size_t len, bool log, struct slm_lba_info *info);
bool check_output_slm_data_ready(uint64_t start_sddr, size_t *cmd_len);
bool check_slm_output_data_finalized(uint64_t start_addr, size_t len);
bool check_slm_output_data_finalized_info(uint64_t start_addr, size_t len, struct slm_lba_info *info);
size_t get_slm_last_page_leftover(uint64_t start_addr);
void notify_slm_data_ready(uint64_t start_sddr, size_t len);
void notify_misaligned_slm_data_ready(uint64_t start_sddr, size_t len);
void notify_if_slm_data_ready(uint64_t start_sddr, size_t len);
void notify_compute_ready(uint64_t start_addr);
void notify_compute_ready_circular(uint64_t start_addr);
#if (USE_HEAD_TAIL_DEP)
/* Circular (magic) rings: producers publish head/tail in LOGICAL bytes
 * (physical addresses are ambiguous in a ring). */
void notify_circular_load_ready(struct slm_lba_info *info, size_t logical, size_t len);
void notify_circular_output_produced(struct slm_lba_info *info, size_t logical_write_end);
#endif
void notify_output_consumed(uint64_t start_addr, size_t len);
size_t get_latest_compute_ready(uint64_t start_addr, size_t total_size);
void update_circular_compute_offset(uint64_t start_addr, size_t logical_offset);
struct slm_lba_info *get_slm_lba_info_from_addr(uint64_t addr);
void finalize_slm_data_ready(uint64_t start_addr, size_t output_len);

struct slm_lba_info *alloc_slm_lba_info(uint64_t start_addr, uint64_t start_lba, size_t len, uint32_t task_id,
										struct source_range_entry *sre, int nentry);
void free_slm_lba_info(size_t slm_offset, size_t slm_size);
void release_slm_memory(uint64_t start_addr, size_t len, struct slm_lba_info *slm_lba_info_ptr, uint32_t task_id);

void slm_request_demand_read(size_t addr, size_t size);
void slm_request_demand_read_info(size_t addr, size_t size, struct slm_lba_info *info);
size_t slm_get_demand_read_offset(size_t addr);
void slm_detect_sequential(size_t addr);
void complete_slm_demand_read(size_t addr, size_t len);
void notify_slm_demand_read_requested(size_t addr);
uint32_t get_io_task_from_slm_addr(size_t addr);
size_t get_continuous_size(size_t addr, size_t len);

void debug_slm_memory(uint64_t start_addr, size_t len);

/* Check if a logical file address is mapped in the extent map */
static inline bool extent_is_mapped(struct slm_lba_info *info, size_t logical_addr, size_t len)
{
	size_t file_offset = logical_addr - info->start_addr;
	int i;
	for (i = 0; i < info->extent_count; i++) {
		if (file_offset >= info->extents[i].file_offset &&
		    file_offset + len <= info->extents[i].file_offset + info->extents[i].file_size) {
			return true;
		}
	}
	return false;
}

/* Translate a logical file address to an SLM address using extent map */
static inline size_t extent_translate_addr(struct slm_lba_info *info, size_t logical_addr)
{
	size_t file_offset = logical_addr - info->start_addr;
	int i;
	for (i = 0; i < info->extent_count; i++) {
		if (file_offset >= info->extents[i].file_offset &&
		    file_offset < info->extents[i].file_offset + info->extents[i].file_size) {
			size_t within = file_offset - info->extents[i].file_offset;
			return info->start_addr + info->extents[i].slm_offset + within;
		}
	}
	return logical_addr; /* fallback: not yet mapped */
}
#endif
