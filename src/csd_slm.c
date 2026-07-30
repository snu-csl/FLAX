
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "nvmev.h"
#include "nvme_csd.h"
#include "csd_slm.h"
#include "csd_dispatcher.h"
#include "csd_user_func.h"

#if (_SLM_DEBUG_ == 1)
#define NVMEV_SLM_INFO(string, args...) printk("[SLM_DEBUG] - %s : " string, __func__, ##args)
#else
#define NVMEV_SLM_INFO(string, args...)
#endif
#define DIVIDE_UP(value, divisor) (((value) + (divisor)-1) / (divisor))

static struct slm_physical_resource slm_physical_manager;
struct slm_data_ready_info *slm_data_ready_info;

static const struct allocator_ops buddy_allocator_ops = {
	.init = buddy_init,
	.allocate = buddy_alloc,
	.deallocate = buddy_free,
	.size = buddy_size,
	.act_size = buddy_act_size,
	.status = buddy_print,
	.kill = buddy_kill,
};

bool check_allocated_slm_range(size_t addr, size_t size)
{
	bool ret = true;
	size_t slm_offset = 0;

	slm_offset = get_slm_offset(addr);
	size_t slm_size = slm_physical_manager.allocator.act_size(slm_offset);

	NVMEV_SLM_INFO("Check SLM - SLM offset: %lu, SLM size: %lu\n, Check size: %lu", slm_offset, slm_size, size);

	ret = (size <= slm_size) ? true : false;
	return ret;
}

size_t alloc_slm_range(size_t size)
{
	unsigned int i = 0;
	size_t slm_offset = -1;

	slm_offset = slm_physical_manager.allocator.allocate(size, NULL);
	if (slm_offset == -1) {
		return -1;
	}

	// Since the ready bit map is managed in page units, we require page-aligned allocation to simplify management.
	// But this is not guaranteed by the allocator interface, so we add an additional check here.
	// If this check fails frequently, we may need to modify the allocator interface to guarantee page-aligned allocation.
	if (slm_offset % SLM_PAGE_SIZE != 0) {
		NVMEV_ERROR("Allocated SLM offset is not aligned to page size\n");
		BUG();
	}

	NVMEV_SLM_INFO("Alloc SLM - SLM offset: %lu (%lu), SLM PageOffset: %lu(Count:%lu)", slm_offset,
				   get_slm_addr(slm_offset), slm_offset / SLM_PAGE_SIZE, size / SLM_PAGE_SIZE);
	return get_slm_addr(slm_offset);
}

uint32_t get_task_id_from_slm_lba_info(size_t addr)
{
	size_t slm_offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	uint32_t task_id = TASK_ID_NONE;

	if (slm_data_ready_info[slm_offset].slm_lba_info != NULL) {
		task_id = ((struct slm_lba_info *)slm_data_ready_info[slm_offset].slm_lba_info)->task_id;
	}

	return  task_id;
}

size_t get_slm_range_size(size_t addr)
{
	size_t slm_offset = get_slm_offset(addr);

	return slm_physical_manager.allocator.size(slm_offset);
}

void free_slm_range(size_t addr)
{
	unsigned int ret = 0;
	size_t slm_offset = 0;

	slm_offset = get_slm_offset(addr);
	size_t slm_size = slm_physical_manager.allocator.act_size(slm_offset);

	free_slm_lba_info(slm_offset, slm_size);

	NVMEV_SLM_INFO("Free SLM - SLM offset: %lu, SLM size: %lu\n", slm_offset, slm_size);

	if (slm_physical_manager.allocator.deallocate(slm_offset) != 0) {
		NVMEV_ERROR("Error when freeing SLM page %lu, %lu\n", slm_offset, slm_size);
		BUG_ON(1);
	}
}

void print_slm_info(void)
{
	// slm_physical_manager.allocator.status();
}

void copy_to_slm(size_t dest, void *src, size_t size)
{
	if (dest < slm_physical_manager.start_addr) {
		NVMEV_ERROR("[%s] INVALID SLM ADDR:%lu", __FUNCTION__, dest);
	}

	// TODO: additional checking of range
	memcpy((void *)dest, src, size);
}

void copy_from_slm(void *dest, size_t src, size_t size)
{
	if (src < slm_physical_manager.start_addr) {
		NVMEV_ERROR("[%s] INVALID SLM ADDR:%lu", __FUNCTION__, src);
	}

	// TODO: additional checking of range
	memcpy(dest, (void *)src, size);
}

size_t get_slm_offset(size_t addr)
{
	return (addr - slm_physical_manager.start_addr);
}
size_t get_slm_addr(size_t offset)
{
	return (slm_physical_manager.start_addr + offset);
}

void init_slm_memory(void *start, size_t slm_size)
{
	size_t i = 0;
	size_t page_count = slm_size / SLM_PAGE_SIZE;

	NVMEV_SLM_INFO("SLM Init Info : start:%llu, size:%lu, Page:%lu", (uint64_t)start, slm_size, page_count);

	// slm_data_ready_info = kzalloc(sizeof(struct slm_data_ready_info) * page_count, GFP_KERNEL);
	slm_data_ready_info = vmalloc_node(sizeof(struct slm_data_ready_info) * page_count, 1);
	if (slm_data_ready_info == NULL) {
		NVMEV_ERROR("slm_data_ready_info alloc fail");
		BUG();
	}
	for (i = 0; i < page_count; i++) {
		slm_data_ready_info[i].slm_lba_info = NULL;
	}

	slm_physical_manager.start_addr = (uint64_t)start;
	slm_physical_manager.allocator = buddy_allocator_ops;
	if (slm_physical_manager.allocator.init(slm_size) != 0) {
		NVMEV_ERROR("Allocator init failed!\n");
	}
}

void final_slm_memory(void)
{
	// kfree(slm_data_ready_info);
	vfree(slm_data_ready_info);
	slm_physical_manager.allocator.kill();
}

void reserve_slm_memory(uint64_t start_addr, size_t len, struct slm_lba_info *slm_lba_info_ptr)
{
	size_t i = 0;
	size_t page_count = DIVIDE_UP(len, SLM_PAGE_SIZE);
	uint64_t start_offset = get_slm_offset(start_addr);
	start_offset = start_offset / SLM_PAGE_SIZE;

	for (i = start_offset; i < start_offset + page_count; i++) {
		slm_data_ready_info[i].slm_lba_info = slm_lba_info_ptr;
	}

	/* Reset head/tail frontiers (reserve always follows a fresh alloc_slm_lba_info,
	 * which also zeroes the extent/demand fields). */
	slm_lba_info_ptr->ht_head = 0;
	slm_lba_info_ptr->ht_tail = 0;
	slm_lba_info_ptr->ht_producer_done = false;
	slm_lba_info_ptr->ht_pending_cnt = 0;

	NVMEV_SLM_INFO("Reserved SLM Page - start:%llu(offset:%llu), count:%lu)", start_addr, start_offset, page_count);
}

/* Host-managed (synchronous) CSD input readiness.
 *
 * In host-managed CSD the host copies the entire input region into SLM with the
 * memory_copy command and only issues the compute command afterwards, so the
 * whole region is valid the moment the memory_copy command is scheduled. The
 * async demand-loader path (which would otherwise install the page->info
 * back-pointer and advance ht_head incrementally) is compiled out, so publish
 * the region in one shot here: install the page->slm_lba_info back-pointer (so
 * check_slm_data_ready can locate the region) and set the head frontier to cover
 * all of len. ht_producer_done makes any later loader-completion notify a no-op. */
void set_slm_ready_for_csd(uint64_t start_addr, size_t len, struct slm_lba_info *info)
{
	size_t i = 0;
	size_t page_count = DIVIDE_UP(len, SLM_PAGE_SIZE);
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;

	for (i = start_offset; i < start_offset + page_count; i++) {
		slm_data_ready_info[i].slm_lba_info = info;
	}

	info->ht_head = len;
	info->ht_tail = 0;
	info->ht_producer_done = true;
	info->ht_pending_cnt = 0;
}

void release_slm_memory(uint64_t start_addr, size_t len, struct slm_lba_info *slm_lba_info_ptr, uint32_t task_id)
{
	size_t i = 0;
	size_t page_count = DIVIDE_UP(len, SLM_PAGE_SIZE);
	uint64_t start_offset = get_slm_offset(start_addr);
	start_offset = start_offset / SLM_PAGE_SIZE;

	for (i = start_offset; i < start_offset + page_count; i++) {
		if (slm_data_ready_info[i].slm_lba_info != NULL) {
			if (slm_data_ready_info[i].slm_lba_info->task_id == task_id) {
				slm_data_ready_info[i].slm_lba_info = NULL;
			}
		}
	}
	// kfree(slm_lba_info_ptr);
	// if (slm_lba_info_ptr->sre != NULL) { // sre free is handled in csd_dispatcher
	//     vfree(slm_lba_info_ptr->sre);
	// }
	vfree(slm_lba_info_ptr);
}

/* ============================================================================
 *  Head/tail offset dependency tracking
 *
 *  Each region tracks two monotonic, region-relative byte frontiers
 *  in its slm_lba_info:
 *      ht_head : PRODUCED frontier  (loader / compute writer advances it)
 *      ht_tail : CONSUMED frontier  (compute reader / host advances it)
 *  Readiness    = "is the probe's last page below DIVIDE_UP(ht_head, PAGE)?"
 *  Back-pressure= "how far has the consumer gotten?" = ht_tail.
 *
 *  Three roles (who calls what):
 *    PRODUCER (NAND->SLM load, compute->SLM output) advances ht_head via
 *        notify_slm_data_ready / notify_misaligned_slm_data_ready /
 *        notify_if_slm_data_ready / finalize_slm_data_ready, and for rings
 *        notify_circular_load_ready / notify_circular_output_produced.
 *    CONSUMER (compute reads its input) checks readiness with
 *        check_slm_data_ready[_info] and advances ht_tail via
 *        notify_compute_ready / update_circular_compute_offset.
 *    HOST (SLM->host readout) checks with check_output_slm_data_ready and
 *        advances ht_tail (rings) via notify_output_consumed.
 *
 *  Two sub-modes layered on the frontier:
 *    DEMAND/EXTENT - a scattered (random-access) reader flips one-way to
 *        extent_mapped; readiness becomes per-extent (extents[].ready),
 *        published by complete_slm_demand_read. See slm_request_demand_read[_info].
 *    CIRCULAR RING - compound buffers keep ht_head/ht_tail in MONOTONIC LOGICAL
 *        bytes (physical = base + (logical & MAGIC_BUFFER_MASK)); the wrappers
 *        below feed the helpers logical offsets so a wrap is unambiguous.
 *
 *  Concurrency: single producer / single consumer per frontier, lock-free;
 *  producers smp_wmb() before publishing, consumers smp_rmb() before reading.
 * ==========================================================================*/

/* Advance the contiguous produced frontier, absorbing a completed byte range
 * [start,end) that may arrive out of order (load IO completion reordering). */
static void ht_advance_head(struct slm_lba_info *info, size_t start, size_t end)
{
	int i;
	bool progressed;

	if (end <= start)
		return;

	if (start <= info->ht_head) {
		/* Range extends the contiguous frontier directly. */
		if (end > info->ht_head)
			info->ht_head = end;
	} else {
		/* Out-of-order: stash the gap, coalescing with an overlapping entry. */
		for (i = 0; i < info->ht_pending_cnt; i++) {
			if (start <= info->ht_pending[i].end &&
			    end >= info->ht_pending[i].start) {
				if (start < info->ht_pending[i].start)
					info->ht_pending[i].start = start;
				if (end > info->ht_pending[i].end)
					info->ht_pending[i].end = end;
				break;
			}
		}
		if (i == info->ht_pending_cnt) {
			if (info->ht_pending_cnt >= HT_MAX_PENDING) {
				NVMEV_ERROR("[SLM] ht pending overflow: head=%lu range=[%lu,%lu)\n",
					    info->ht_head, start, end);
				BUG();
			}
			info->ht_pending[info->ht_pending_cnt].start = start;
			info->ht_pending[info->ht_pending_cnt].end = end;
			info->ht_pending_cnt++;
		}
	}

	/* Absorb stashed intervals now contiguous with the frontier. */
	do {
		progressed = false;
		for (i = 0; i < info->ht_pending_cnt; i++) {
			if (info->ht_pending[i].start <= info->ht_head) {
				if (info->ht_pending[i].end > info->ht_head)
					info->ht_head = info->ht_pending[i].end;
				info->ht_pending[i] = info->ht_pending[info->ht_pending_cnt - 1];
				info->ht_pending_cnt--;
				progressed = true;
				break;
			}
		}
	} while (progressed);

	smp_wmb();
}

/* Page-granular in-order frontier advance (sequential single-writer output). */
static inline void ht_advance_head_page(struct slm_lba_info *info, size_t rel_byte_end)
{
	size_t rel_page_end = ALIGNED_DOWN(rel_byte_end, SLM_PAGE_SIZE);

	if (rel_page_end > info->ht_head) {
		info->ht_head = rel_page_end;
		smp_wmb();
	}
}

/* Advance the consumed frontier (monotonic). */
static inline void ht_set_tail(struct slm_lba_info *info, size_t rel_end)
{
	if (rel_end > info->ht_tail) {
		info->ht_tail = rel_end;
		smp_wmb();
	}
}

#if (USE_HEAD_TAIL_DEP)
/* Circular (magic) rings keep head/tail in MONOTONIC LOGICAL bytes
 * (physical = base + (logical & MAGIC_BUFFER_MASK)), so there is no
 * wrap-epoch ambiguity on ring reuse. The generic notify/check entry points
 * only see physical addresses — ambiguous in a ring — so circular callers
 * feed the helpers logical offsets through these wrappers instead. */

/* Loader IO completion for a circular input ring: byte-exact, out-of-order
 * tolerated via ht_pending coalescing (logical offsets never wrap). */
void notify_circular_load_ready(struct slm_lba_info *info, size_t logical, size_t len)
{
	if (!info->ht_producer_done)
		ht_advance_head(info, logical, logical + len);
}

/* Compute writer progress on a circular output ring: publish the produced
 * frontier at HT_NOTIFY_GRAIN granularity (not per SLM page) so the store to
 * the ht_head cacheline - contended with the spinning downstream consumer -
 * stays off the producer's per-page hot path. The final partial grain is
 * published byte-exact by circular_output_finalize. */
void notify_circular_output_produced(struct slm_lba_info *info, size_t logical_write_end)
{
	/* Never publish below page granularity: the page-rounded consumer check
	 * (DIVIDE_UP) would otherwise see a partially written page as ready. */
	size_t grain = (HT_NOTIFY_GRAIN > SLM_PAGE_SIZE) ? HT_NOTIFY_GRAIN : SLM_PAGE_SIZE;
	size_t rel_grain_end = ALIGNED_DOWN(logical_write_end, grain);

	if (!info->ht_producer_done && rel_grain_end > info->ht_head) {
		info->ht_head = rel_grain_end;
		smp_wmb();
	}
}
#endif

/* CONSUMER CHECK (compute input; non-compound loads + compound-read type 1).
 * use_head_tail branch -> ht_head page compare; extent_mapped branch -> covering
 * extents[].ready. Circular inputs are checked via circular_buf_check_data_at. */
bool check_slm_data_ready(uint64_t start_addr, size_t len, bool log)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	if (info != NULL && info->use_head_tail && !info->extent_mapped &&
	    !info->is_circular && !info->output_is_circular) {
		/* Head/tail readiness is PAGE-granular: a page is ready once any of its
		 * bytes are within the produced frontier (DIVIDE_UP), so the final
		 * partial page counts as a whole (published via notify_misaligned/finalize).
		 * Reproduce the len==SLM_PAGE_SIZE special case (check only the start
		 * page) used for compaction/crc probes.
		 * !extent_mapped: an extent-mapped region (e.g. magic-read type-0 after the
		 * flip, which keeps use_head_tail set) must resolve via extents[].ready
		 * below, not against the frozen ht_head — matches check_slm_data_ready_info.
		 * !is_circular: rel here is PHYSICAL — wrong for a wrapped ring; ring
		 * consumers probe via the logical branch in circular_buf_check_data_at. */
		size_t rel_start = start_addr - info->start_addr;
		size_t last_page = (len == SLM_PAGE_SIZE)
					   ? (rel_start / SLM_PAGE_SIZE)
					   : ((rel_start + len - 1) / SLM_PAGE_SIZE);
		smp_rmb();
		if (last_page < DIVIDE_UP(info->ht_head, SLM_PAGE_SIZE)) {
			if (info->is_output == false)
				info->next_addr = rel_start + len;
			return true;
		}
		return false;
	}

#if (USE_HEAD_TAIL_DEP)
	/* One-way demand/extent mode (entered from head/tail stream on a
	 * non-contiguous probe; identity-mapped). Readiness is per-extent: a
	 * covered+ready extent is ready even while another extent's demand is in
	 * flight (the flag is published only after the whole extent lands), so we
	 * check it BEFORE the blanket demand-pending gate below. An uncovered probe
	 * returns false to trigger demand allocation in slm_request_demand_read. */
	if (info != NULL && info->extent_mapped) {
		size_t rel = start_addr - info->start_addr;
		int j;
		smp_rmb();
		for (j = 0; j < info->extent_count; j++) {
			if (rel >= info->extents[j].file_offset &&
			    rel + len <= info->extents[j].file_offset + info->extents[j].file_size) {
				if (info->extents[j].ready) {
					if (info->is_output == false)
						info->next_addr = rel + len;
					return true;
				}
				return false;
			}
		}
		return false;
	}
#endif

	/* No live region reaches here: every non-circular input is use_head_tail or
	 * extent_mapped (above); circular inputs are checked via circular_buf_check_data_at.
	 * Fail safe: report not-ready and warn, so a coverage miss surfaces as a logged
	 * stall, never a stale-data read. */
	WARN_ONCE(1, "check_slm_data_ready: addr=%llu len=%lu has no readiness flag\n", start_addr, len);
	return false;
}

/* CONSUMER CHECK for compound-read type 0. Takes info explicitly: the packed SLM
 * address can't recover the region through the page back-pointer. extent_mapped
 * is checked BEFORE use_head_tail, so once the region flips it resolves purely
 * via extents[].ready (the pre-flip page-0 phase uses the head/tail branch). */
bool check_slm_data_ready_info(uint64_t start_addr, size_t len, bool log, struct slm_lba_info *info)
{
	if (info != NULL && info->is_output == false) {
		if (info->random_access && info->demand_read_offset != -1) {
			return false;
		}
	}

	if (info != NULL && info->extent_mapped) {
		/* Per-extent readiness: the demand IO publishes extents[j].ready on its
		 * last chunk (complete_slm_demand_read), keyed on the LOGICAL file_offset
		 * — so the decision needs no translate (extent_translate_addr stays in the
		 * CALLER, __magic_rocksdb_read, for the data access after this returns true).
		 * Magic-read demands are serialized, so the demand-pending gate above
		 * already covers the one in-flight extent. */
		size_t rel = start_addr - info->start_addr;
		int j;
		smp_rmb();
		for (j = 0; j < info->extent_count; j++) {
			if (rel >= info->extents[j].file_offset &&
			    rel + len <= info->extents[j].file_offset + info->extents[j].file_size) {
				if (info->extents[j].ready) {
					if (info->is_output == false)
						info->next_addr = rel + len;
					return true;
				}
				return false;
			}
		}
		return false; /* uncovered probe -> triggers demand alloc */
	} else if (info != NULL && info->use_head_tail && !info->is_circular && !info->output_is_circular) {
		/* Initial sequential phase (pre-extent_mapped): head/tail frontier. A probe
		 * past ht_head returns false -> check_data_using_ptr_info calls
		 * slm_request_demand_read_info, which flips the region to extent_mapped
		 * (COMPACT packing); thereafter the extent_mapped branch above serves reads
		 * via extents[].ready and this branch is no longer taken. */
		size_t rel_start = start_addr - info->start_addr;
		size_t last_page = (len == SLM_PAGE_SIZE)
					   ? (rel_start / SLM_PAGE_SIZE)
					   : ((rel_start + len - 1) / SLM_PAGE_SIZE);
		smp_rmb();
		if (last_page < DIVIDE_UP(info->ht_head, SLM_PAGE_SIZE)) {
			if (info->is_output == false)
				info->next_addr = rel_start + len;
			return true;
		}
		return false;
	}

	/* Fail safe: every _info caller (magic-read type-0) is use_head_tail pre-flip
	 * or extent_mapped post-flip. */
	WARN_ONCE(1, "check_slm_data_ready_info: addr=%llu len=%lu has no readiness flag\n", start_addr, len);
	return false;
}

bool check_slm_output_data_finalized(uint64_t start_addr, size_t len)
{
	bool ret = false;
	size_t i = 0;
	size_t page_count = DIVIDE_UP(len, SLM_PAGE_SIZE);
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	if (info != NULL && info->is_output == true && info->final_offset != -1) {
		if (start_offset > info->final_offset) {
			// printk("start: %llu final: %llu\n", start_offset, info->final_offset);
			return true;
		}
	}

	return ret;
}

bool check_slm_output_data_finalized_info(uint64_t start_addr, size_t len, struct slm_lba_info *info)
{
	bool ret = false;
	size_t i = 0;
	size_t page_count = DIVIDE_UP(len, SLM_PAGE_SIZE);
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;

	if (info != NULL && info->is_output == true && info->final_offset != -1) {
		if (start_offset > info->final_offset) {
			// printk("start: %llu final: %llu\n", start_offset, info->final_offset);
			return true;
		}
	}

	return ret;
}

size_t get_slm_last_page_leftover(uint64_t start_addr)
{
	bool ret = false;
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	if (info != NULL && info->is_output == true && info->final_offset != -1) {
		if (start_offset == info->final_offset) {
			// printk("%llu %llu, leftover is %llu\n", start_offset, info->final_offset, info->final_leftovers);
			return info->final_leftovers;
		} else {
			printk("WRONG LAST PAGE ADDRESS %llu %llu\n", start_offset, info->final_offset);
			BUG();
		}
	}

	return ret;
}

/* HOST CHECK: is the requested output range ready to read back to the host?
 * Linear outputs compare against the ht_head frontier; circular output rings
 * compare the host-consumed logical offset against ht_head. *cmd_len is clamped
 * to the finalized output length (output_logical_total_size). */
bool check_output_slm_data_ready(uint64_t start_addr, size_t *cmd_len)
{
	size_t len = *cmd_len;
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	/* Head/tail output (linear): PAGE-granular readiness via ht_head, end-of-data
	 * clamp via producer_done/output_logical_total_size.
	 * !output_is_circular: rel here is PHYSICAL — the circular host ring is
	 * handled logically in the output_is_circular branch below. */
	if (info != NULL && info->use_head_tail && !info->output_is_circular) {
		size_t rel_start = start_addr - info->start_addr;
		size_t rel_end = rel_start + len;
		size_t last_page;
		smp_rmb();
		if (info->ht_producer_done) {
			size_t produced = info->output_logical_total_size;
			if (rel_start >= produced) {
				*cmd_len = 0;
				return true;
			}
			if (rel_end > produced) {
				*cmd_len = produced - rel_start;
				rel_end = produced;
			}
		}
		last_page = (rel_end - 1) / SLM_PAGE_SIZE;
		return (last_page < DIVIDE_UP(info->ht_head, SLM_PAGE_SIZE));
	}

	/* Circular output ring (output_is_circular implies use_head_tail): logical
	 * byte compare against the produced frontier. Mid-stream the head is
	 * page-aligned (full pages only); after finalize the end-clamp bounds len
	 * and the head is byte-exact. */
	if (info != NULL && info->is_output && info->output_is_circular) {
		size_t requested_logical_start = info->host_consumed_logical_offset;

		/* Check end-of-data using output_logical_total_size */
		if (info->output_logical_total_size > 0) {
			if (requested_logical_start >= info->output_logical_total_size) {
				*cmd_len = 0;
				return true;
			}
			size_t remaining = info->output_logical_total_size - requested_logical_start;
			if (remaining < len) {
				*cmd_len = remaining;
				len = remaining;
			}
		}

		smp_rmb();
		return (requested_logical_start + len <= info->ht_head);
	}

	/* Fail safe: every output region is use_head_tail (linear) or output_is_circular. */
	WARN_ONCE(1, "check_output_slm_data_ready: addr=%llu has no output readiness flag\n", start_addr);
	*cmd_len = 0;
	return false;
}

/* PRODUCER: a load / compute-output IO of [start,len) landed -> advance ht_head.
 * No-op once extent_mapped: demand completions land at the PACKED target and
 * publish per-extent readiness via complete_slm_demand_read, so their physical
 * address must never reach ht_advance_head. */
void notify_slm_data_ready(uint64_t start_addr, size_t len)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info;

	/* Read info once; after the head/tail publish below the host may free the
	   buffer, so do all info accesses up front. */
	info = READ_ONCE(slm_data_ready_info[start_offset].slm_lba_info);

	if (info == NULL) {
		// The buffer might have already been released
		return;
	}

	if (info->use_head_tail && !info->extent_mapped && !info->is_circular && !info->output_is_circular) {
		/* Advance the contiguous produced frontier (reorder-tolerant). Once
		 * finalized, head is authoritative and page-aligned; ignore any trailing
		 * notify so it cannot push head past the published end.
		 * !extent_mapped: a demand-phase completion lands at the PACKED
		 * demand_read_slm_target; its readiness is published per-extent by
		 * complete_slm_demand_read, NOT here — feeding ht_advance_head the packed
		 * physical delta would corrupt ht_head.
		 * !is_circular: rel here is PHYSICAL — ring producers publish logically
		 * via notify_circular_load_ready/_produced. */
		size_t rel = start_addr - info->start_addr;
		if (!info->ht_producer_done) {
			ht_advance_head(info, rel, rel + len);
		}
		return;
	}

	/* Demand-phase (extent_mapped) completion: readiness is published by
	 * complete_slm_demand_read via extents[].ready. Nothing to publish here. */
}

/* CONSUMER: the page at start_addr has been fully consumed -> advance ht_tail.
 * This is the back-pressure signal the loader throttle reads (get_latest_compute_ready). */
void notify_compute_ready(uint64_t start_addr)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;

	/* Head/tail: the page containing start_addr is fully consumed; advance the
	 * tail to the end of that page. (Every consumer region is use_head_tail.) */
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;
	if (info != NULL && info->use_head_tail) {
		size_t rel_page_end =
			ALIGNED_DOWN(start_addr - info->start_addr, SLM_PAGE_SIZE) + SLM_PAGE_SIZE;
		ht_set_tail(info, rel_page_end);
	}
}

/* For circular (head/tail) rings this is a no-op: readiness is a logical head
 * compare and the tail advances via update_circular_compute_offset. */
void notify_compute_ready_circular(uint64_t start_addr)
{
	uint64_t start_offset = get_slm_offset(start_addr);
	start_offset = start_offset / SLM_PAGE_SIZE;

#if (USE_HEAD_TAIL_DEP)
	/* Head/tail ring: per-page state has no reader (readiness is a logical
	 * head compare; the tail advances via update_circular_compute_offset at
	 * the same call site). Keep callers untouched, no-op here. */
	{
		struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;
		if (info != NULL && info->use_head_tail)
			return;
	}
#endif

	NVMEV_SLM_INFO("Notify Compute Ready Circular - start:%llu, page_offset:%llu", start_addr, start_offset);
}

/* Called after host completes reading from circular output buffer.
 * Mirrors the host's consumed frontier into ht_tail for the writer's space wait. */
void notify_output_consumed(uint64_t start_addr, size_t len)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	if (info == NULL || !info->output_is_circular)
		return;

	/* output_is_circular implies use_head_tail: advance the consumed frontier. */
	info->host_consumed_logical_offset += len;
	smp_wmb();
	ht_set_tail(info, info->host_consumed_logical_offset);
}

/* Update the compute logical offset for circular buffer throttling */
void update_circular_compute_offset(uint64_t start_addr, size_t logical_offset)
{
	uint64_t start_offset = get_slm_offset(start_addr);
	start_offset = start_offset / SLM_PAGE_SIZE;

	struct slm_lba_info *lba_info = slm_data_ready_info[start_offset].slm_lba_info;
	if (lba_info->is_circular) {
#if (USE_HEAD_TAIL_DEP)
		/* Head/tail ring: ht_tail is the single consumed frontier — it feeds
		 * both the loader throttle (input rings) and the writer's space wait
		 * (intermediate rings, where the CRC consumer runs this path). */
		if (lba_info->use_head_tail) {
			ht_set_tail(lba_info, logical_offset);
			return;
		}
#endif
		lba_info->compute_logical_offset = logical_offset;
		smp_wmb();
	}
}

/* Get slm_lba_info from an SLM address */
struct slm_lba_info *get_slm_lba_info_from_addr(uint64_t addr)
{
	uint64_t offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	return slm_data_ready_info[offset].slm_lba_info;
}

/* BACK-PRESSURE: the first not-yet-consumed page of the region (= base page +
 * ht_tail/PAGE). The loader throttle keeps the load a bounded window ahead of this. */
size_t get_latest_compute_ready(uint64_t start_addr, size_t total_size)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	/* Head/tail: first not-yet-consumed page = base + (ht_tail / page). Every
	 * region reaching the loader throttle is use_head_tail. */
	if (info != NULL && info->use_head_tail) {
		smp_rmb();
		return start_offset + (info->ht_tail / SLM_PAGE_SIZE);
	}

	if (info->random_access) {
		return start_offset;
	}

	/* Fail safe: report no progress (base page) so the throttle waits, logged. */
	WARN_ONCE(1, "get_latest_compute_ready: addr=%llu not use_head_tail\n", start_offset);
	return start_offset;
}

/* PRODUCER (sub-page tail): like notify_slm_data_ready, but for a load whose
 * final chunk is not page-aligned -> advance ht_head by the exact byte range
 * (the page-granular check rounds the partial final page up to ready). */
void notify_misaligned_slm_data_ready(uint64_t start_addr, size_t len)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	/* Head/tail: advance the produced frontier by the exact byte range (incl. a
	 * final sub-page chunk); the page-granular readiness check (DIVIDE_UP) makes
	 * the partial final page fully ready. Ignore once finalized so the head stays
	 * page-aligned. !extent_mapped: demand completions publish per-extent (see
	 * notify_slm_data_ready). !is_circular: rel is PHYSICAL. */
	if (info != NULL && info->use_head_tail && !info->extent_mapped && !info->is_circular && !info->output_is_circular) {
		size_t rel = start_addr - info->start_addr;
		if (!info->ht_producer_done)
			ht_advance_head(info, rel, rel + len);
	}
}

/* PRODUCER (compute output): advance ht_head to the last FULLY written page
 * (the partial final page is published later by finalize_slm_data_ready). */
void notify_if_slm_data_ready(uint64_t start_addr, size_t len)
{
	uint64_t start_offset = get_slm_offset(start_addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	/* Head/tail output: advance the produced frontier to the last fully written
	 * page (sequential single-writer; the partial final page is published by
	 * finalize_slm_data_ready). !extent_mapped/!is_circular as above. */
	if (info != NULL && info->use_head_tail && !info->extent_mapped && !info->is_circular && !info->output_is_circular) {
		ht_advance_head_page(info, (start_addr - info->start_addr) + len);
	}
}

/* PRODUCER done: record the byte-exact output total + set ht_producer_done, and
 * bump ht_head to include the final (possibly partial) page. After this, host
 * reads clamp to output_logical_total_size and late notifies are ignored. */
void finalize_slm_data_ready(uint64_t start_addr, size_t output_len)
{
	uint64_t start_offset = get_slm_offset(start_addr);
	start_offset = start_offset / SLM_PAGE_SIZE;
	uint64_t remain = output_len - (output_len / SLM_PAGE_SIZE) * SLM_PAGE_SIZE;

	if (slm_data_ready_info[start_offset].slm_lba_info != NULL) {
		struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

		info->final_offset = start_offset;
		info->final_leftovers = remain;

		if (info->use_head_tail) {
			/* Mark the producer done and publish end-of-output. Keep the
			 * byte-precise total for host-readout clamping, but advance the
			 * page-granular head to include the whole final data page (page
			 * output_len/PAGE). (output_len/PAGE + 1) covers both the
			 * partial-final-page and the page-aligned (empty trailing page)
			 * cases. start_addr == region_base + output_len, so output_len is
			 * the region-relative end. */
			info->output_logical_total_size = output_len;
			info->ht_producer_done = true;
			{
				size_t head_pages = (output_len / SLM_PAGE_SIZE) + 1;
				if (head_pages * SLM_PAGE_SIZE > info->ht_head)
					info->ht_head = head_pages * SLM_PAGE_SIZE;
			}
			smp_wmb();
		}

		NVMEV_SLM_INFO("Notify SLM Finalized - final page:%llu(offset:%llu), leftovers: %d\n", start_addr, start_offset,
					   remain);
	}
}

struct slm_lba_info *alloc_slm_lba_info(uint64_t start_addr, uint64_t start_lba, size_t len, uint32_t task_id,
										struct source_range_entry *sre, int nentry)
{
	struct slm_lba_info *info = vmalloc_node(sizeof(struct slm_lba_info), 1);
	if (info == NULL) {
		NVMEV_ERROR("slm_lba_info alloc fail!");
		BUG();
	}
	info->start_addr = start_addr;
	info->len = len;
	info->demand_read_offset = -1;
	info->request_done = true;
	info->random_access = false;
	info->task_id = task_id;
	info->next_addr = 0;
	info->is_output = false;
	info->final_offset = -1;
	info->final_leftovers = 0;
	info->stream_access = false;

	info->nentry = nentry;
	info->sre = sre;

	// Initialize circular buffer fields (defaults to non-circular)
	info->is_circular = false;
	info->logical_total_size = len;
	info->load_complete = false;
	info->compute_logical_offset = 0;

	// Initialize circular OUTPUT fields. vmalloc_node() does NOT zero memory and
	// the head/tail readiness branches test !output_is_circular, so these must be
	// explicit (otherwise a garbage-truthy read skips the head/tail path).
	info->output_is_circular = false;
	info->host_consumed_logical_offset = 0;
	info->output_logical_total_size = 0;

	// Initialize extent-based remapping fields (defaults to disabled)
	info->extent_mapped = false;
	info->extent_count = 0;
	info->extent_next_free_page = 0;
	info->demand_read_slm_target = 0;

	// Head/tail dependency tracking (disabled by default; enabled per-region by dispatcher)
	info->use_head_tail = false;
	info->ht_head = 0;
	info->ht_tail = 0;
	info->ht_producer_done = false;
	info->ht_pending_cnt = 0;

	return info;
}

void free_slm_lba_info(size_t slm_offset, size_t slm_size)
{
	size_t i = 0;
	size_t page_count = DIVIDE_UP(slm_size, SLM_PAGE_SIZE);
	uint64_t start_offset = slm_offset / SLM_PAGE_SIZE;
	uint64_t end_offset = start_offset + page_count;
	struct slm_lba_info *info;

	// Single pass: free structures and clear all references
	while (start_offset < end_offset) {
		info = slm_data_ready_info[start_offset].slm_lba_info;

		if (info == NULL) {
			memset(&slm_data_ready_info[start_offset], 0,
				sizeof(struct slm_data_ready_info));
			start_offset++;
			continue; 
		}

		// Clear task reference if this info is still in use by a task
		uint32_t task_id = info->task_id;
		if (task_id != TASK_ID_NONE) {
			uint32_t pool_idx = TASK_POOL_INDEX(task_id);
			struct ccsd_task_info *task = &(task_table.task[pool_idx]);
			if (TASK_GEN_VALID(task_id, task) && task->program_idx == COPY_TO_SLM_PROGRAM_INDEX && task->slm_lba_info != NULL) {
				// Normally the compaction free case wouldn't come in here

				/* Transition load task directly to END so __request_io
				   never touches the freed slm_lba_info */
				
				if (atomic_read(&task->task_step) == CCSD_TASK_SCHEDULE) {
					NVMEV_ERROR("Task %u is in step %d, origin_program_idx=%u when freeing its slm_lba_info! This should not happen to prevent page fault(cpu=%d)\n",
						   task_id, atomic_read(&task->task_step), task->origin_program_idx, smp_processor_id());
					 /* This should never happen. It means the task is being freed while still scheduled, which is a bug. */
					BUG();
				}

				if (atomic_read(&task->task_step) == CCSD_TASK_FREE) {
					NVMEV_ERROR("Task %u is in step %d, origin_program_idx=%u when freeing its slm_lba_info! This should not happen to prevent page fault(cpu=%d)\n",
						   task_id, atomic_read(&task->task_step), task->origin_program_idx, smp_processor_id());
					 /* This should never happen. It means the task is being freed while still scheduled, which is a bug. */
					BUG();
				}

				// CSD_DEBUG_MAGIC_READ("[DEBUG_FREE] free_slm_lba_info: setting task %u to END (was step=%d, info=%px)\n",
				// 	   task_id, task->task_step, info);
				atomic_set(&task->task_step, CCSD_TASK_END);
				task->slm_lba_info = NULL;
			}
		}

		// Calculate how many pages this info covers
		size_t info_pages = DIVIDE_UP(info->len, SLM_PAGE_SIZE);
		uint64_t info_end = start_offset + info_pages;
		if (info_end > end_offset) {
			info_end = end_offset;
		}

		// Free the sre if present
		if (info->sre != NULL) {
			kfree(info->sre);
			info->sre = NULL;
		}

		// Free the info structure
		vfree(info);

		// Clear all pages covered by this info
		size_t clear_count = info_end - start_offset;
		memset(&slm_data_ready_info[start_offset], 0,
		       clear_count * sizeof(struct slm_data_ready_info));

		start_offset = info_end;
	}
}

#if (USE_HEAD_TAIL_DEP)
/* Demand-mode extent allocation for a head/tail region that has switched to
 * one-way demand loading (e.g. rocksdb_read's scattered probes). Identity-mapped:
 * the demanded block lands at its natural file offset, so the compute reads
 * file_ptr+offset directly with no address translation. Readiness is tracked
 * per-extent (extents[].ready). Only one demand is in flight at a time; later
 * probes allocate further extents after the prior one completes (which clears
 * demand_read_offset). */
static void ht_demand_alloc_extent(struct slm_lba_info *info, size_t addr, size_t size)
{
	size_t read_offset = addr - info->start_addr;
	size_t aligned_offset = ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE);
	size_t aligned_size = ALIGNED_UP(size + (read_offset - aligned_offset), SLM_PAGE_SIZE);
	int idx, j;

	/* A demand read is already in flight: serialize. The consumer keeps waiting
	 * on this extent's ready flag via check_slm_data_ready. */
	if (info->demand_read_offset != -1)
		return;

	/* Already covered by a mapped extent: readiness handled by extents[j].ready. */
	for (j = 0; j < info->extent_count; j++) {
		if (aligned_offset >= info->extents[j].file_offset &&
		    aligned_offset + aligned_size <= info->extents[j].file_offset + info->extents[j].file_size)
			return;
	}

	if (info->extent_count >= MAX_EXTENT_ENTRIES) {
		NVMEV_ERROR("[SLM] head/tail demand extent map full (count=%d)\n", info->extent_count);
		return;
	}

	idx = info->extent_count;
	info->extents[idx].file_offset = aligned_offset;
	info->extents[idx].file_size = aligned_size;
	info->extents[idx].slm_offset = aligned_offset; /* identity: natural file offset */
	info->extents[idx].ready = false;
	info->extent_count++;

	info->random_access = true;
	info->request_done = false;
	info->demand_read_size = aligned_size;
	/* Land the demand IO at the natural offset (dispatcher redirect at 1918
	 * uses demand_read_slm_target when extent_mapped). */
	info->demand_read_slm_target = info->start_addr + aligned_offset;

	/* Wake the load task so __request_io picks up the demand read. */
	{
		uint32_t dr_pool = TASK_POOL_INDEX(info->task_id);
		struct ccsd_task_info *dr_task = &task_table.task[dr_pool];
		atomic_set(&dr_task->task_step, CCSD_TASK_SCHEDULE);
	}

	/* Publish demand_read_offset last; the loader polls it via
	 * slm_get_demand_read_offset (paired smp_rmb there). */
	smp_wmb();
	info->demand_read_offset = aligned_offset;
}
#endif

/* CONSUMER MISS handler (identity / non-magic read). Called when a probe found
 * data not-ready. A contiguous probe just waits for the stream; a scattered jump
 * flips the region one-way to demand/extent mode (use_head_tail=false,
 * extent_mapped=true) and demand-loads the block at its NATURAL file offset
 * (identity map: no address translation in the consumer). */
void slm_request_demand_read(size_t addr, size_t size)
{
	uint64_t offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[offset].slm_lba_info;

	if (info == NULL) {
		NVMEV_ERROR("[SLM] Invalid demand_read requested!:%llu", offset);

		return;
	}

	if (info->stream_access == true) {
		return;
	}

#if (USE_HEAD_TAIL_DEP)
	/* Already switched to one-way demand/extent mode: allocate the next extent
	 * for this scattered probe. */
	if (info->extent_mapped) {
		ht_demand_alloc_extent(info, addr, size);
		return;
	}

	/* Head/tail stream region. A consumer that starts at offset 0 is a sequential
	 * stream (compaction input, rocksdb_read search_type[1]) -> stay on head/tail
	 * and (re)assert stream_access so the loader's look-ahead throttle engages. A
	 * non-contiguous probe (rocksdb_read search_type[0] scatters) switches the
	 * region one-way to demand/extent mode (never reverts). A contiguous probe at
	 * a non-zero offset (stream reading just ahead of the loader) is left to wait
	 * for the stream. */
	if (info->use_head_tail) {
		/* Circular ring: NEVER flip to extent mode — the probe address is
		 * physical, so a large wrapped probe (e.g. the CRC metadata read) can
		 * land on an arbitrary ring page and masquerade as a scattered jump.
		 * Only the stream_access handshake applies (set at non-zero offsets
		 * so the loader's look-ahead quota engages). */
		if (info->is_circular) {
			if (info->demand_read_offset == -1 && info->is_output != true &&
			    (addr - info->start_addr) != 0 && info->random_access == false) {
				info->stream_access = true;
			}
			return;
		}

		if (info->demand_read_offset == -1 && info->is_output != true) {
			uint64_t read_offset = addr - info->start_addr;
			uint64_t read_page = read_offset / SLM_PAGE_SIZE;
			uint64_t next_page = info->next_addr / SLM_PAGE_SIZE;

			if (!(read_page == next_page || read_page == next_page + 1)) {
				info->extent_mapped = true;
				info->random_access = true;
				smp_wmb();
				info->use_head_tail = false;
				ht_demand_alloc_extent(info, addr, size);
			} else if (read_offset != 0 && info->random_access == false) {
				info->stream_access = true;
			}
		}
		return;
	}
#endif

	if (info->demand_read_offset == -1 && info->is_output != true) {
		uint64_t read_offset = addr - info->start_addr;
		uint64_t read_page = read_offset / SLM_PAGE_SIZE;
		uint64_t next_page = info->next_addr / SLM_PAGE_SIZE;

		// Allow access if read_page is same, previous, or next page relative to next_page
		// Don't do demand read for circular buffer
		if (! (read_page == next_page || read_page == next_page + 1) && !info->is_circular) {
			info->random_access = true;
			info->request_done = false;
			info->demand_read_size = size + (read_offset - ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE));
			/* Wake load task so __request_io picks up the demand read */
			{
				uint32_t dr_pool_idx = TASK_POOL_INDEX(info->task_id);
				struct ccsd_task_info *dr_task = &task_table.task[dr_pool_idx];
				atomic_set(&dr_task->task_step, CCSD_TASK_SCHEDULE);
			}
			/* Ensure all fields are visible before publishing demand_read_offset */
			smp_wmb();
			info->demand_read_offset = ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE);
			// printk("Demand - address:%lu, size:%lu, demand_offset:%lu, demand_size:%lu\n", get_slm_offset(addr), size, info->demand_read_offset, info->demand_read_size);
		} else {
			if (read_offset != 0) {
				if (info->random_access == false) {
					info->stream_access = true;
				} else {
					info->request_done = false;
					info->demand_read_size = size + (read_offset - ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE));
					/* Wake load task so __request_io picks up the demand read */
					{
						uint32_t dr_pool_idx = TASK_POOL_INDEX(info->task_id);
						struct ccsd_task_info *dr_task = &task_table.task[dr_pool_idx];
						atomic_set(&dr_task->task_step, CCSD_TASK_SCHEDULE);
					}
					/* Ensure all fields are visible before publishing demand_read_offset */
					smp_wmb();
					info->demand_read_offset = ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE);
					// printk("Always Random Demand - address:%lu, size:%lu, demand_offset:%lu, demand_size:%lu\n", get_slm_offset(addr), size, info->demand_read_offset, info->demand_read_size);
				}
			}
		}
	}
}

/* CONSUMER MISS handler (read type 0). Same one-way flip as
 * slm_request_demand_read, but the demanded block is PACKED into the separate
 * MAGIC_READ_BUFFER via the extent_next_free_page cursor (the file is larger than
 * its SLM slice), and the consumer translates logical->packed with
 * extent_translate_addr. Readiness is per-extent (extents[].ready). */
void slm_request_demand_read_info(size_t addr, size_t size, struct slm_lba_info *info)
{
	if (info == NULL) {
		NVMEV_ERROR("[SLM] Invalid demand_read_info requested!");
		return;
	}

	if (info->demand_read_offset == -1 && info->is_output != true) {
		uint64_t read_offset = addr - info->start_addr;
		uint64_t read_page = read_offset / SLM_PAGE_SIZE;
		uint64_t next_page = info->next_addr / SLM_PAGE_SIZE;

		// If already extent-mapped, use extent path directly
		if (info->extent_mapped) {
			size_t aligned_offset = ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE);
			size_t aligned_size = ALIGNED_UP(size + (read_offset - aligned_offset), SLM_PAGE_SIZE);
			size_t pages_needed = aligned_size / SLM_PAGE_SIZE;
			int j;

			// Check if already mapped
			for (j = 0; j < info->extent_count; j++) {
				if (aligned_offset >= info->extents[j].file_offset &&
				    aligned_offset + aligned_size <= info->extents[j].file_offset + info->extents[j].file_size) {
					CSD_DEBUG_MAGIC_READ("Extent already mapped: file_off=%lu in extent[%d] (file_off=%lu, pages=%lu, slm_page=%lu)\n",
						aligned_offset, j, info->extents[j].file_offset,
						info->extents[j].file_size / SLM_PAGE_SIZE,
						info->extents[j].slm_offset / SLM_PAGE_SIZE);
					return;
				}
			}

			// Allocate new extent
			if (info->extent_count < MAX_EXTENT_ENTRIES &&
			    info->extent_next_free_page + pages_needed <= MAGIC_READ_BUFFER_PAGES) {
				int idx = info->extent_count;
				size_t slm_page_offset = info->extent_next_free_page;

				info->extents[idx].file_offset = aligned_offset;
				info->extents[idx].file_size = aligned_size;
				info->extents[idx].slm_offset = slm_page_offset * SLM_PAGE_SIZE;
#if (USE_HEAD_TAIL_DEP)
				/* extents[] live in a vmalloc_node (un-zeroed) slm_lba_info and
				 * are reused across demands; init so the per-extent readiness
				 * scan never sees a stale/garbage ready (mirrors ht_demand_alloc_extent). */
				info->extents[idx].ready = false;
#endif
				info->extent_count++;
				info->extent_next_free_page += pages_needed;

				info->demand_read_size = aligned_size;
				info->demand_read_slm_target = info->start_addr + slm_page_offset * SLM_PAGE_SIZE;
				info->request_done = false;
				info->random_access = true;
				/* Wake load task so __request_io picks up the demand read */
				{
					uint32_t dr_pool = TASK_POOL_INDEX(info->task_id);
					struct ccsd_task_info *dr_t = &task_table.task[dr_pool];
					if (atomic_read(&dr_t->task_step) == CCSD_TASK_SCHEDULE) {
						NVMEV_ERROR("Task %u is already in SCHEDULE step when requesting demand read! This should not happen to prevent dead lock(cpu=%d)\n",
								   info->task_id, smp_processor_id());
						BUG();
					}
					atomic_set(&dr_t->task_step, CCSD_TASK_SCHEDULE);
				}
				/* Ensure all fields are visible before publishing demand_read_offset,
				   which is the flag the dispatcher checks */
				smp_wmb();
				info->demand_read_offset = aligned_offset;
				CSD_DEBUG_MAGIC_READ("Extent alloc[%d]: file_off=%lu, pages=%lu -> slm_page=%lu, target_page=%lu, task_id=%u\n",
					idx, aligned_offset, pages_needed,
					slm_page_offset,
					get_slm_offset(info->demand_read_slm_target) >> SLM_PAGE_SHIFT,
					info->task_id);
			} else {
				NVMEV_ERROR("[SLM] Extent map full or buffer exhausted! count=%d, next_free=%d, needed=%lu\n",
					    info->extent_count, info->extent_next_free_page, pages_needed);
			}
			return;
		}

		// Random access detection
		if (! (read_page == next_page || read_page == next_page + 1) && !info->is_circular) {
			if (!info->extent_mapped) {
				info->extent_mapped = true;
				info->extent_count = 0;
				/* Skip the pages loaded by the first sequential IO, which is
				 * MIN_IO_REQUEST_SIZE bytes (see __request_io), not one SLM page. */
				info->extent_next_free_page = DIVIDE_UP(MIN_IO_REQUEST_SIZE, SLM_PAGE_SIZE);
				/* Pre-flip readiness was head/tail (ht_head); post-flip
				 * readiness is per-extent (extents[].ready). */
				CSD_DEBUG_MAGIC_READ("Extent mode enabled: slm_page=%lu, read_off=%llu, next_addr=%llu\n",
					get_slm_offset(info->start_addr) >> SLM_PAGE_SHIFT, read_offset, info->next_addr);
			}
			{
				uint32_t dr_pool = TASK_POOL_INDEX(info->task_id);
				struct ccsd_task_info *dr_t = &task_table.task[dr_pool];
				if (atomic_read(&dr_t->task_step) != CCSD_TASK_SCHEDULE) {
					NVMEV_ERROR("Task %u is not in SCHEDULE step when requesting first demand read! It's in step %d. This should not happen to avoid dead lock(cpu=%d)\n",
							   info->task_id, atomic_read(&dr_t->task_step), smp_processor_id());
					NVMEV_ERROR("Task %u info: start_addr=%llu, next_addr=%llu, demand_read_offset=%llu, demand_read_size=%zu\n",
						   info->task_id, info->start_addr, info->next_addr, info->demand_read_offset, info->demand_read_size);
					BUG();
				}
			}

			info->random_access = true;
			info->request_done = false;
			info->demand_read_size = size + (read_offset - ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE));
			info->demand_read_slm_target = info->start_addr + info->extent_next_free_page * SLM_PAGE_SIZE;
			/* Wake load task so __request_io picks up the demand read */
			{
				uint32_t dr_pool2 = TASK_POOL_INDEX(info->task_id);
				struct ccsd_task_info *dr_t2 = &task_table.task[dr_pool2];
				atomic_set(&dr_t2->task_step, CCSD_TASK_SCHEDULE);
			}
			/* Ensure all fields are visible before publishing demand_read_offset */
			smp_wmb();
			info->demand_read_offset = ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE);

			{
				size_t aligned_offset = info->demand_read_offset;
				size_t aligned_size = ALIGNED_UP(info->demand_read_size, SLM_PAGE_SIZE);
				size_t pages_needed = aligned_size / SLM_PAGE_SIZE;

				if (info->extent_count < MAX_EXTENT_ENTRIES &&
				    info->extent_next_free_page + pages_needed <= MAGIC_READ_BUFFER_PAGES) {
					int idx = info->extent_count;
					info->extents[idx].file_offset = aligned_offset;
					info->extents[idx].file_size = aligned_size;
					info->extents[idx].slm_offset = info->extent_next_free_page * SLM_PAGE_SIZE;
#if (USE_HEAD_TAIL_DEP)
					info->extents[idx].ready = false; /* enable-transition first extent (see above) */
#endif
					info->extent_count++;
					info->extent_next_free_page += pages_needed;
					CSD_DEBUG_MAGIC_READ("Extent alloc[%d]: file_off=%lu, pages=%lu -> slm_page=%lu, target_page=%lu, task_id=%u\n",
						idx, aligned_offset, pages_needed,
						info->extents[idx].slm_offset / SLM_PAGE_SIZE,
						get_slm_offset(info->demand_read_slm_target) >> SLM_PAGE_SHIFT,
						info->task_id);
				}
			}
		} else {
			if (read_offset != 0) {
				if (info->random_access == false) {
					info->stream_access = true;
				} else {
					info->request_done = false;
					/* Wake load task so __request_io picks up the demand read */
					{
						uint32_t dr_pool3 = TASK_POOL_INDEX(info->task_id);
						struct ccsd_task_info *dr_t3 = &task_table.task[dr_pool3];
						atomic_set(&dr_t3->task_step, CCSD_TASK_SCHEDULE);
					}
					info->demand_read_offset = ALIGNED_DOWN(read_offset, SLM_PAGE_SIZE);
					info->demand_read_size = size + (read_offset - info->demand_read_offset);
				}
			}
		}
	}
}

void slm_detect_sequential(size_t addr)
{
	uint64_t offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[offset].slm_lba_info;

	if (info == NULL) {
		NVMEV_ERROR("Invalid slm_detect_sequential");

		return;
	}

	if (info->demand_read_offset == -1) {
		info->demand_read_offset = ALIGNED_DOWN(addr - info->start_addr, SLM_PAGE_SIZE);
		info->request_done = false;
		info->random_access = false;

		NVMEV_SLM_INFO("Request SLM Demand Read - start:%lu(offset:%llu)", addr, offset);
	}
}

size_t slm_get_demand_read_offset(size_t addr)
{
	uint64_t offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[offset].slm_lba_info;

	if (info == NULL) {
		return -1;
	}
	if (info->request_done == false) {
		size_t offset_val = READ_ONCE(info->demand_read_offset);
		if (offset_val != -1) {
			/* Pair with smp_wmb() in slm_request_demand_read_info/slm_request_demand_read
			   to ensure demand_read_size/demand_read_slm_target are visible */
			smp_rmb();
			NVMEV_SLM_INFO("Enqueue I/O for Demand Read - start:%lu(offset:%llu)", addr,
						   offset_val / SLM_PAGE_SIZE);
		}
		return offset_val;
	}
	return -1;
}

/* PRODUCER (demand): a demanded extent's IO landed -> publish extents[j].ready
 * (keyed on the LOGICAL file_offset == demand_read_offset) and clear
 * demand_read_offset so the next scattered probe can allocate the next extent. */
void complete_slm_demand_read(size_t addr, size_t len)
{
	size_t i = 0;
	uint64_t start_offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	size_t page_count = len / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	if (info != NULL && info->is_output == false) {
		if (info->demand_read_offset != -1) {
#if (USE_HEAD_TAIL_DEP)
			/* Whole extent has landed: publish its ready flag. demand_read_offset
			 * holds the in-flight extent's (page-aligned) file_offset. Pair the
			 * data writes from the load IO with smp_wmb() before the flag store;
			 * the consumer reads it after smp_rmb() in check_slm_data_ready. */
			if (info->extent_mapped) {
				int j;
				for (j = 0; j < info->extent_count; j++) {
					if (info->extents[j].file_offset == info->demand_read_offset) {
						smp_wmb();
						info->extents[j].ready = true;
						break;
					}
				}
				/* Publish ready BEFORE clearing demand_read_offset: the consumer
				 * checks the demand-pending gate (demand_read_offset) first, so the
				 * flag must already be visible when the gate opens (else a spurious
				 * extra spin). */
				smp_wmb();
			}
#endif
			info->demand_read_offset = -1;
		}
	}
}

void notify_slm_demand_read_requested(size_t addr)
{
	uint64_t offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	slm_data_ready_info[offset].slm_lba_info->request_done = true;

	NVMEV_SLM_INFO("Enqueue SLM Demand Read - start:%lu(offset:%llu)", addr, offset);
}

uint32_t get_io_task_from_slm_addr(size_t addr)
{
	uint64_t offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[offset].slm_lba_info;
	if (info != NULL && info->is_output == false) {
		return info->task_id;
	}

	return TASK_ID_NONE;
}

size_t get_continuous_size(size_t addr, size_t len)
{
	uint64_t start_offset = get_slm_offset(addr) / SLM_PAGE_SIZE;
	size_t page_count = len / SLM_PAGE_SIZE;
	struct slm_lba_info *info = slm_data_ready_info[start_offset].slm_lba_info;

	/* Contiguous fully-ready bytes from addr, page-granular (matching the old
	 * per-page scan: a streaming producer publishes whole pages, so count full
	 * pages below the produced frontier), capped at page_count pages. */
	if (info != NULL && info->use_head_tail) {
		size_t rel_page = (addr - info->start_addr) / SLM_PAGE_SIZE;
		size_t ready_pages;
		smp_rmb();
		ready_pages = ALIGNED_DOWN(info->ht_head, SLM_PAGE_SIZE) / SLM_PAGE_SIZE;
		if (rel_page >= ready_pages)
			return 0;
		ready_pages -= rel_page;
		if (ready_pages > page_count)
			ready_pages = page_count;
		return ready_pages * SLM_PAGE_SIZE;
	}

	WARN_ONCE(1, "get_continuous_size: addr=%lu not use_head_tail\n", addr);
	return 0;
}

void debug_slm_memory(uint64_t start_addr, size_t len)
{
#if _SLM_DEBUG_
	size_t i = 0;
	size_t page_count = len / SLM_PAGE_SIZE;
	uint64_t start_offset = get_slm_offset(start_addr);
	start_offset = start_offset / SLM_PAGE_SIZE;

	NVMEV_SLM_INFO("Debug_slm_memory:start_offset:%llu", start_offset);
	i = start_offset + page_count;
	NVMEV_SLM_INFO("Debug_slm_memory:last_offset:%llu", i);
#endif
}
