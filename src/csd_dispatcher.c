/**********************************************************************
 * Copyright (c) 2020-2021
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * *********************************************************************/

#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>
#include <linux/crypto.h>

#include <linux/version.h>
#include <linux/bpf.h>
#include <linux/filter.h>

#include "nvmev.h"
#include "nvme_csd.h"
#include "csd_slm.h"
#include "csd_dispatcher.h"
#include "csd_user_func.h"
#include "csd_ftl.h"

#undef PERF_DEBUG
#define PRP_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern struct nvmev_dev *vdev;

static struct csd_io_req_table io_req_table;
struct ccsd_task_table task_table;
static struct ccsd_magic_parameter magic_param;
static struct ccsd_magic_read_parameter magic_read_param;

#define MAX_DEFERRED_SLM_FREES 16 

struct deferred_slm_free {
	bool active;
	size_t input_buf;
	uint32_t load_task_ids[MAX_MAGIC_READ_FILES];
	int num_load_tasks;
};

static struct deferred_slm_free deferred_slm_frees[MAX_DEFERRED_SLM_FREES];

static struct task_struct *csd_compute_workder[32];
static struct task_struct *csd_slm_workder[32];
static struct task_struct *csd_dispatcher_helper;

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

static void __copy_prp_data(int sqid, int sq_entry, void *buf, size_t size, bool from_host)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	u64 paddr;
	u64 *paddr_list = NULL;
	int prp_offs = 0;
	int prp2_offs = 0;

	size_t offset = 0;
	size_t remaining = size;

	while (remaining) {
		size_t mem_offs = 0;
		size_t io_size;
		void *vaddr;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = sq_entry(sq_entry).rw.prp1;
		} else if (prp_offs == 2) {
			paddr = sq_entry(sq_entry).rw.prp2;
			if (remaining > PAGE_SIZE) {
				paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) + (paddr & PAGE_OFFSET_MASK);
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			paddr = paddr_list[prp2_offs++];
		}

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		if (from_host == true) {
			memcpy(buf + offset, vaddr + mem_offs, io_size);
		} else {
			memcpy(vaddr + mem_offs, buf + offset, io_size);
		}
		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);
}

static size_t __calculate_lba(struct slm_lba_info *sre_info, size_t io_offset)
{
	int i = 0;
	size_t base_addr = 0;
	size_t local_offset = io_offset;

	if (io_offset == 0) {
		return sre_info->sre[0].saddr;
	}

	for (i = 0; i < sre_info->nentry; i++) {
		base_addr = sre_info->sre[i].saddr;
		if (local_offset < sre_info->sre[i].nByte) {
			break;
		}
		local_offset -= sre_info->sre[i].nByte;
	}

	NVMEV_DEBUG("__calculate_lba: i: %d, io_offset: %llu, base_addr: %llu, local_offset: %llu\n", i, io_offset,
				base_addr, local_offset);

	return base_addr + (local_offset / 512);
}

static size_t __calculate_io_size(struct slm_lba_info *sre_info, size_t io_offset, size_t io_size)
{
	int i = 0;
	size_t base_addr = 0;
	size_t local_offset = io_offset;

	if (io_size % 512 != 0) {
		io_size = ALIGNED_UP(io_size, 512);
	}

	if (io_offset == 0) {
		if (io_size < sre_info->sre[0].nByte) {
			return io_size;
		}
		return sre_info->sre[0].nByte;
	}

	for (i = 0; i < sre_info->nentry; i++) {
		base_addr = sre_info->sre[i].saddr;
		if (local_offset < sre_info->sre[i].nByte) {
			break;
		}
		local_offset -= sre_info->sre[i].nByte;
	}

	if (i >= sre_info->nentry) {
		return 0;
	}

	if (local_offset + io_size > sre_info->sre[i].nByte) {
		NVMEV_DEBUG("__calculate_io_size: i: %d, io_offset: %llu, base_addr: %llu, local_offset: %llu, io_size: %llu\n",
					i, io_offset, base_addr, local_offset, io_size);
		return sre_info->sre[i].nByte - local_offset;
	}

	NVMEV_DEBUG("__calculate_io_size: i: %d, io_offset: %llu, base_addr: %llu, local_offset: %llu, io_size: %llu\n", i,
				io_offset, base_addr, local_offset, io_size);
	return io_size;
}

/* Calculate sub-sector byte offset for non-aligned reads */
static size_t __calculate_local_offset(struct slm_lba_info *sre_info, size_t io_offset)
{
	int i = 0;
	size_t local_offset = io_offset;

	for (i = 0; i < sre_info->nentry; i++) {
		if (local_offset < sre_info->sre[i].nByte) {
			break;
		}
		local_offset -= sre_info->sre[i].nByte;
	}

	return (local_offset % 512);
}

/* Calculate physical SLM address for circular buffer
 * For circular buffers: wraps logical_offset within MAGIC_BUFFER_SIZE
 * For non-circular buffers: returns base + logical_offset directly
 *
 * Ghost page handling: The buffer is laid out as [MAGIC_BUFFER_SIZE | GHOST_PAGE_SIZE]
 * When writing to offset 0..GHOST_PAGE_SIZE, the data also needs to be written to
 * the ghost page region (MAGIC_BUFFER_SIZE + offset). This is handled by the caller
 * who will issue a second IO for the ghost page when needed.
 */
static size_t __calculate_circular_buf_addr(struct slm_lba_info *lba_info, size_t base_addr, size_t logical_offset)
{
	if (lba_info == NULL || !lba_info->is_circular) {
		return base_addr + logical_offset;
	}
	/* Wrap offset within MAGIC_BUFFER_SIZE for circular buffer */
	size_t physical_offset = logical_offset & MAGIC_BUFFER_MASK;
	return base_addr + physical_offset;
}

/* Calculate IO size considering circular buffer boundary
 * IO should not cross the end of the circular buffer (MAGIC_BUFFER_SIZE)
 */
static size_t __calculate_circular_io_size(struct slm_lba_info *lba_info, size_t logical_offset, size_t requested_size)
{
	if (lba_info == NULL || !lba_info->is_circular) {
		return requested_size;
	}
	size_t physical_offset = logical_offset & MAGIC_BUFFER_MASK;
	size_t remaining_in_buffer = MAGIC_BUFFER_SIZE - physical_offset;
	if (requested_size > remaining_in_buffer) {
		return remaining_in_buffer;
	}
	return requested_size;
}

static void __enqueue_slm_work(unsigned int io_req_id)
{
	int i = 0;
	struct csd_internal_io_req *io_req = &(io_req_table.io_req[io_req_id]);
	int slm_turn = 0;
	struct ccsd_list *slm_list = NULL;
	int min = 0xFFFF;
	int min_index = 0;

	slm_turn = io_req_table.slm_turn;
	min_index = slm_turn;
	for (i = 0; i < task_table.num_slm_resources; i++) {
		slm_list = &(io_req_table.slm_list[slm_turn]);
		NVMEV_DEBUG("i:%d,slm_turn:%d,min:%d,min_index:%d,running:%d", i, slm_turn, min, min_index, slm_list->running);
		if (slm_list->running == 0)
			break;
		if (min > slm_list->running) {
			min = slm_list->running;
			min_index = slm_turn;
		}
		slm_turn = (slm_turn + 1) % task_table.num_slm_resources;
	}
	slm_list = &(io_req_table.slm_list[min_index]);

	if (slm_list->head == -1) {
		slm_list->head = io_req_id;
	} else {
		unsigned int tail = slm_list->tail;
		BUG_ON(tail == -1);

		io_req->prev = tail;
		io_req_table.io_req[tail].next = io_req_id;
	}
	slm_list->tail = io_req_id;
	slm_list->running = slm_list->running + 1;

	io_req_table.slm_turn = (io_req_table.slm_turn + 1) % task_table.num_slm_resources;
}

static void __enqueue_task_multi(struct ccsd_list *list, unsigned int start, unsigned int end, unsigned int count)
{
	struct ccsd_task_info *task_start = &(task_table.task[start]);
	struct ccsd_task_info *task_end = &(task_table.task[end]);

	task_start->prev = -1;
	task_end->next = -1;

	if (list->head == -1) {
		list->head = start;
	} else {
		unsigned int tail = list->tail;
		BUG_ON(tail == -1);

		task_start->prev = tail;
		task_table.task[tail].next = start;
	}
	list->tail = end;
	list->running = list->running + count;
}

static void __dequeue_task_multi(struct ccsd_list *list, unsigned int start, unsigned int end, unsigned int count)
{
	struct ccsd_task_info *task_start = &(task_table.task[start]);
	struct ccsd_task_info *task_end = &(task_table.task[end]);
	unsigned int prev = task_start->prev;
	unsigned int next = task_end->next;

	task_start->prev = -1;
	task_end->next = -1;

	if (list->head == start) {
		list->head = next;
	}

	if (list->tail == end) {
		list->tail = prev;
	}

	if (next != -1) {
		task_table.task[next].prev = prev;
	}

	if (prev != -1) {
		task_table.task[prev].next = next;
	}

	if (list->running < count) {
		NVMEV_ERROR("QUEUE_COUNT_ERROR : %d %d", list->running, count);
	}
	list->running = list->running - count;
}

static void __enqueue_task(struct ccsd_list *list, unsigned int task_id)
{
	__enqueue_task_multi(list, task_id, task_id, 1);
}

static void __dequeue_task(struct ccsd_list *list, unsigned int task_id)
{
	__dequeue_task_multi(list, task_id, task_id, 1);
}

static void __enqueue_compute_work(unsigned int task_id, unsigned int compute_turn)
{
	struct ccsd_list *comp_list = NULL;
	struct ccsd_task_info *task = &(task_table.task[task_id]);

	comp_list = &(task_table.comp_list[compute_turn]);
	__enqueue_task(comp_list, task_id);
	// printk("Enqueue_Compute_work %d (QID:%u)", task_id, compute_turn);
}

static bool __find_idle_compute_core(unsigned int *compute_core_id)
{
	struct ccsd_list *comp_list = NULL;

	for (int i = 0; i < task_table.num_cpu_resources; i++) {
		comp_list = &(task_table.comp_list[i]);
		if (comp_list->running == 0) {
			*compute_core_id = i;
			return true;
		}
	}

	return false;
}

static void __enqueue_io_req_free_list(unsigned int start, unsigned int end)
{
	struct csd_internal_io_req *io_req = &(io_req_table.io_req[end]);
	struct ccsd_list *free_list = &(io_req_table.free_list);

	if (io_req->next != -1) {
		io_req_table.io_req[io_req->next].prev = -1;
	}
	io_req->next = -1;

	io_req = &(io_req_table.io_req[start]);

	if (free_list->head == -1) {
		io_req->prev = -1;
		free_list->head = start;
	} else {
		unsigned int tail = free_list->tail;
		BUG_ON(tail == -1);

		io_req->prev = tail;
		io_req_table.io_req[tail].next = start;
	}
	free_list->tail = end;
}

static bool __do_perform_program_management(int sqid, int sq_entry, unsigned int *status)
{
	return true;
}

static bool __do_perform_memory_management(int sqid, int sq_entry, unsigned int *status, size_t *result)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvme_command_csd *cmd = (struct nvme_command_csd *)(&sq_entry(sq_entry));
	unsigned int i = 0;
	unsigned int type = cmd->memory_management.sel;
	unsigned int nEntry = cmd->memory_management.numr;
	struct memory_region_entry entry;

	if (type == nvme_memory_range_create) {
	} else if (type == nvme_memory_range_delete) {
	} else {
		NVMEV_ERROR("[%s] invalid memory management parameter (%d)", __FUNCTION__, type);
	}

	if (nEntry != 1) {
		NVMEV_ERROR("[%s] invalid memory management parameter (%d, %d)", __FUNCTION__, type, nEntry);
		BUG();
	}

	for (i = 0; i < nEntry; i++) {
		if (type == nvme_memory_range_create) {
			size_t len = ((__u64)cmd->memory_management.cdw13 << 32) | cmd->memory_management.cdw12;
			size_t slm_offset = alloc_slm_range(len);

			if (slm_offset == -1) {
				NVMEV_ERROR("[%s] SLM Allocation failed\n", __FUNCTION__);
				*status = (NVME_SCT_CMD_SPECIFIC_STATUS << 8) | NVME_SC_MAX_MEM_RANGE_EXCEEDED;
				return false;
			}

			size_t saddr = get_slm_offset(slm_offset);
			NVMEV_CSD_INFO("SLM", "Allocate buffer:%llu, %lu", saddr, len);
			// Return the allocated address via command result
			*result = saddr;
			entry.saddr = saddr;
			__copy_prp_data(sqid, sq_entry, &entry, sizeof(struct memory_region_entry), false);
		} else if (type == nvme_memory_range_delete) {
			size_t saddr = ((__u64)cmd->memory_management.cdw13 << 32) | cmd->memory_management.cdw12;
			size_t addr = get_slm_addr(saddr);
			NVMEV_CSD_INFO("SLM", "Release host buffer:%llu", saddr);

			// We should first set the running slm loads (probably demand loads) to WAIT state before freeing the SLM buffer, otherwise
			// there could be a page fault
			{
				size_t region_size = get_slm_range_size(addr);
				size_t start_page = get_slm_offset(addr) / SLM_PAGE_SIZE;
				size_t num_pages = region_size / SLM_PAGE_SIZE;
				size_t p;

				for (p = 0; p < num_pages; p++) {
					struct slm_lba_info *lba_info = slm_data_ready_info[start_page + p].slm_lba_info;
					if (lba_info == NULL)
						continue;
					if (lba_info->task_id == TASK_ID_NONE)
						continue;
					struct ccsd_task_info *t = &task_table.task[TASK_POOL_INDEX(lba_info->task_id)];
					if (!TASK_GEN_VALID(lba_info->task_id, t))
						continue;
					// printk("Check SLM Park for page %zu, task_id: %u, program_idx:%u, task_step:%u\n", start_page + p, lba_info->task_id, t->program_idx, t->task_step);
					if (t->program_idx == COPY_TO_SLM_PROGRAM_INDEX) {
						// Not going to touch the newly allocated ones
						atomic_cmpxchg(&t->task_step, CCSD_TASK_SCHEDULE, CCSD_TASK_WAIT);
					}
				}
			}

			free_slm_range(addr);
		}
	}
	return true;
}

int get_running_task_count(void)
{
	if (MAX_TASK_COUNT < task_table.free_list.running) {
		NVMEV_ERROR("Task's free list has error %d, %d", MAX_TASK_COUNT, task_table.free_list.running);
	}
	return (MAX_TASK_COUNT - task_table.free_list.running);
}

int get_used_io_req_count(void)
{
	int count = 0;
	int i;
	for (i = 0; i < MAX_INTERNAL_IO_COUNT; i++) {
		if (io_req_table.io_req[i].io_req_status != CSD_INTERNAL_IO_REQ_FREE)
			count++;
	}
	return count;
}

static inline uint32_t allocate_new_task(struct ccsd_task_info **task)
{
	unsigned int pool_index;
	uint32_t gen_id;

	pool_index = task_table.free_list.head;
	if (pool_index == (unsigned int)-1) {
		return TASK_ID_NONE;
	}
	BUG_ON(pool_index >= MAX_TASK_COUNT);

	*task = &(task_table.task[pool_index]);
	if (atomic_read(&(*task)->task_step) != CCSD_TASK_FREE) {
		NVMEV_ERROR("Free list is corrupted Task id:%u (%u), Task step:%u, original_program_idx: %u, Input_buf_addr: %zu, Output_buf_addr: %zu, Total Size: %zu, Status: %u, program_idx: %u, requested_io_offset: %zu, done_io_offset: %zu, slm_lba_info: %p\n",
			   pool_index, (*task)->generation_id, atomic_read(&(*task)->task_step), (*task)->origin_program_idx, (*task)->input_buf_addr, (*task)->output_buf_addr, (*task)->total_size, (*task)->status, (*task)->program_idx, (*task)->requested_io_offset, (*task)->done_io_offset, (*task)->slm_lba_info);
		BUG();
	}
	__dequeue_task(&task_table.free_list, pool_index);

	/* Construct generation_id: upper bits = generation counter, low bits = pool index */
	gen_id = (task_table.next_generation << TASK_POOL_BITS) | pool_index;
	task_table.next_generation++;
	/* Skip if generation_id would collide with TASK_ID_NONE sentinel */
	if (unlikely(gen_id == TASK_ID_NONE)) {
		gen_id = (task_table.next_generation << TASK_POOL_BITS) | pool_index;
		task_table.next_generation++;
	}
	(*task)->generation_id = gen_id;

	return gen_id;
}

static bool __do_perform_magic_compaction(int sqid, int sq_entry, unsigned int proc_idx, unsigned int *status, size_t *result)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvme_command_csd *cmd = (struct nvme_command_csd *)(&sq_entry(sq_entry));

	int compute_core_id = 0;
	int opcode = cmd->common.opcode;

	// Task variables - 3 tasks for compaction (2 load + 1 compute)
	// + optional 1-2 tasks for CRC when crc_flag is true (depends on crc_cores)
	uint32_t task_id_1, task_id_2, task_id_3;
	uint32_t task_id_crc_1 = TASK_ID_NONE;
	uint32_t task_id_crc_2 = TASK_ID_NONE;
	struct ccsd_task_info *task_1 = NULL;  // Compaction task
	struct ccsd_task_info *task_2 = NULL;  // Load first level
	struct ccsd_task_info *task_3 = NULL;  // Load second level
	struct ccsd_task_info *task_crc_1 = NULL;  // CRC task 1 (even blocks)
	struct ccsd_task_info *task_crc_2 = NULL;  // CRC task 2 (odd blocks, chained)

	size_t nentry_1, nentry_2;
	struct source_range_entry *sre_1 = NULL;
	struct source_range_entry *sre_2 = NULL;
	struct magic_params *user_param = (struct magic_params *)(&magic_param.param);

	size_t first_input_buf = -1;
	size_t second_input_buf = -1;
	size_t output_data_buf = -1;
	size_t intermediate_buf_1 = -1;   // Intermediate buffer for CRC 1 (compaction output)
	size_t intermediate_buf_2 = -1;   // Intermediate buffer for CRC 2 (CRC 1 output, when crc_cores=2)
	size_t final_output_buf = -1;     // Final output buffer (CRC output)

	size_t first_input_size, second_input_size, total_input_size;
	bool crc_flag = false;
	int crc_cores = 1;

	// 0. Copy Magic Params from host
	__copy_prp_data(sqid, sq_entry, &magic_param, sizeof(struct ccsd_magic_parameter), true);

	CSD_DEBUG_MAGIC_COMPACTION("START ROCKSDB MAGIC COMPACTION\n");

	// 1. Get file sizes and SRE counts from magic_param
	first_input_size = magic_param.file_1_size;
	second_input_size = magic_param.file_2_size;
	size_t sstable_size = user_param->compaction.sstable_size;
	// The total input size is currently conservatively estimated
	total_input_size = (((first_input_size + second_input_size) / sstable_size) + 2) * sstable_size;
	nentry_1 = magic_param.sre_1_count;
	nentry_2 = magic_param.sre_2_count;
	crc_flag = user_param->compaction.crc_flag;
	crc_cores = user_param->compaction.crc_cores;
	if (crc_cores < 1) crc_cores = 1;
	if (crc_cores > 2) crc_cores = 2;

	// 2. Allocate 3 tasks
	task_id_1 = allocate_new_task(&task_1);
	if (task_id_1 == TASK_ID_NONE) {
		NVMEV_ERROR("[MAGIC] No free task available for task_1\n");
		return false;
	}

	task_id_2 = allocate_new_task(&task_2);
	if (task_id_2 == TASK_ID_NONE) {
		NVMEV_ERROR("[MAGIC] No free task available for task_2\n");
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_1));
		return false;
	}

	task_id_3 = allocate_new_task(&task_3);
	if (task_id_3 == TASK_ID_NONE) {
		NVMEV_ERROR("[MAGIC] No free task available for task_3\n");
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_1));
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_2));
		return false;
	}

	// 2.1 Allocate CRC task(s) if crc_flag is set
	if (crc_flag) {
		task_id_crc_1 = allocate_new_task(&task_crc_1);
		if (task_id_crc_1 == TASK_ID_NONE) {
			NVMEV_ERROR("[MAGIC] No free task available for CRC task 1\n");
			__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_1));
			__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_2));
			__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_3));
			return false;
		}
		CSD_DEBUG_MAGIC_COMPACTION("CRC task 1 allocated: task_id=%u\n", task_id_crc_1);

		// Allocate second CRC task if crc_cores == 2
		if (crc_cores == 2) {
			task_id_crc_2 = allocate_new_task(&task_crc_2);
			if (task_id_crc_2 == TASK_ID_NONE) {
				NVMEV_ERROR("[MAGIC] No free task available for CRC task 2\n");
				__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_1));
				__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_2));
				__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_3));
				__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_crc_1));
				return false;
			}
			CSD_DEBUG_MAGIC_COMPACTION("CRC task 2 allocated: task_id=%u\n", task_id_crc_2);
		}
	}

	// 3. Copy SREs
	if (nentry_1 > 0) {
		sre_1 = (struct source_range_entry *)kzalloc_node(
			sizeof(struct source_range_entry) * nentry_1, GFP_KERNEL, 1);
		if (sre_1 == NULL) {
			NVMEV_ERROR("[MAGIC] Failed to allocate sre_1\n");
			BUG();
		}
		memcpy(sre_1, magic_param.sre_1, sizeof(struct source_range_entry) * nentry_1);
	}
	if (nentry_2 > 0) {
		sre_2 = (struct source_range_entry *)kzalloc_node(
			sizeof(struct source_range_entry) * nentry_2, GFP_KERNEL, 1);
		if (sre_2 == NULL) {
			NVMEV_ERROR("[MAGIC] Failed to allocate sre_2\n");
			BUG();
		}
		memcpy(sre_2, magic_param.sre_2, sizeof(struct source_range_entry) * nentry_2);
	}

	CSD_DEBUG_MAGIC_COMPACTION("SRE settings done nentry_1: %lu, nentry_2: %lu\n", nentry_1, nentry_2);

	// 4. Allocate SLM buffers using circular buffers for input AND output
	// Each buffer uses MAGIC_BUFFER_SIZE + GHOST_PAGE_SIZE for wrap-around support
	size_t circular_buf_size = MAGIC_BUFFER_SIZE + GHOST_PAGE_SIZE;
	size_t meta_buf_addr;
	first_input_buf = alloc_slm_range(circular_buf_size);
	second_input_buf = alloc_slm_range(circular_buf_size);

	if (crc_flag) {
		// With CRC: compaction writes to intermediate_buf_1, CRC task(s) write to final output
		intermediate_buf_1 = alloc_slm_range(circular_buf_size);
		output_data_buf = intermediate_buf_1;  // Compaction writes to intermediate_buf_1

		if (crc_cores == 2) {
			// crc_cores=2: CRC1 writes to intermediate_buf_2, CRC2 writes to final_output_buf
			intermediate_buf_2 = alloc_slm_range(circular_buf_size);
			final_output_buf = alloc_slm_range(circular_buf_size);
		} else {
			// crc_cores=1: CRC1 writes directly to final_output_buf
			final_output_buf = alloc_slm_range(circular_buf_size);
		}
	} else {
		// Without CRC: compaction writes directly to output
		output_data_buf = alloc_slm_range(circular_buf_size);
	}
	meta_buf_addr = alloc_slm_range(FRONT_METADATA_SIZE); // Separate buffer for FRONT_METADATA

	// Metadata buffer (linear, host-read): track readiness with head/tail so the
	// host blocks until the CSD writes metadata and calls notify_if_slm_data_ready
	// (which advances ht_head). reserve_slm_memory resets ht_head=0.
	if (meta_buf_addr != (size_t)-1) {
		struct slm_lba_info *meta_info = alloc_slm_lba_info(
			meta_buf_addr, 0, FRONT_METADATA_SIZE, TASK_ID_NONE, NULL, 0);
		meta_info->is_output = true;
		meta_info->use_head_tail = true;
		reserve_slm_memory(meta_buf_addr, FRONT_METADATA_SIZE, meta_info);
	}

	if ((first_input_buf == -1) || (second_input_buf == -1) || (output_data_buf == -1) || (meta_buf_addr == -1) ||
	    (crc_flag && (intermediate_buf_1 == -1 || final_output_buf == -1)) ||
	    (crc_flag && crc_cores == 2 && intermediate_buf_2 == -1)) {
		NVMEV_ERROR("[MAGIC] SLM Allocation failed\n");
		*status = (NVME_SCT_CMD_SPECIFIC_STATUS << 8) | NVME_SC_MAX_MEM_RANGE_EXCEEDED;
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_1));
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_2));
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_3));
		if (crc_flag) __enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_crc_1));
		if (crc_flag && crc_cores == 2) __enqueue_task(&task_table.free_list, TASK_POOL_INDEX(task_id_crc_2));
		if (first_input_buf != -1) free_slm_range(first_input_buf);
		if (second_input_buf != -1) free_slm_range(second_input_buf);
		if (output_data_buf != -1 && !crc_flag) free_slm_range(output_data_buf);
		if (crc_flag && intermediate_buf_1 != -1) free_slm_range(intermediate_buf_1);
		if (crc_flag && crc_cores == 2 && intermediate_buf_2 != -1) free_slm_range(intermediate_buf_2);
		if (crc_flag && final_output_buf != -1) free_slm_range(final_output_buf);
		if (meta_buf_addr != -1) free_slm_range(meta_buf_addr);
		if (sre_1) vfree(sre_1);
		if (sre_2) vfree(sre_2);
		return false;
	}

	CSD_DEBUG_MAGIC_COMPACTION("Allocated circular SLM pages: first=%lu, second=%lu, output=%lu, meta=%lu (buf_size=%lu)\n",
		   get_slm_offset(first_input_buf) >> SLM_PAGE_SHIFT,
		   get_slm_offset(second_input_buf) >> SLM_PAGE_SHIFT,
		   get_slm_offset(output_data_buf) >> SLM_PAGE_SHIFT,
		   get_slm_offset(meta_buf_addr) >> SLM_PAGE_SHIFT, circular_buf_size);
	CSD_DEBUG_MAGIC_COMPACTION("Raw addrs: first_buf=%lu, second_buf=%lu, output_buf=%lu, meta_buf=%lu\n",
		   first_input_buf, second_input_buf, output_data_buf, meta_buf_addr);

	// 5. Setup slm_lba_info for buffers (circular for inputs)
	// First input buffer (for task_2 - load) - circular
	task_2->slm_lba_info = alloc_slm_lba_info(first_input_buf, 0, circular_buf_size, task_id_2, sre_1, nentry_1);
	{
		struct slm_lba_info *info = (struct slm_lba_info *)task_2->slm_lba_info;
		info->is_circular = true;
		info->logical_total_size = first_input_size;
		info->load_complete = false;
#if (USE_HEAD_TAIL_DEP)
		info->use_head_tail = true; /* head/tail in logical bytes (ring) */
#endif
	}
	CSD_DEBUG_MAGIC_COMPACTION("Reserving first_input_buf: slm_page=%lu, pages=%lu\n",
		   get_slm_offset(first_input_buf) >> SLM_PAGE_SHIFT, circular_buf_size >> SLM_PAGE_SHIFT);
	reserve_slm_memory(first_input_buf, circular_buf_size, task_2->slm_lba_info);

	// Second input buffer (for task_3 - load) - circular
	task_3->slm_lba_info = alloc_slm_lba_info(second_input_buf, 0, circular_buf_size, task_id_3, sre_2, nentry_2);
	{
		struct slm_lba_info *info = (struct slm_lba_info *)task_3->slm_lba_info;
		info->is_circular = true;
		info->logical_total_size = second_input_size;
		info->load_complete = false;
#if (USE_HEAD_TAIL_DEP)
		info->use_head_tail = true; /* head/tail in logical bytes (ring) */
#endif
	}
	CSD_DEBUG_MAGIC_COMPACTION("Reserving second_input_buf: slm_page=%lu, pages=%lu\n",
		   get_slm_offset(second_input_buf) >> SLM_PAGE_SHIFT, circular_buf_size >> SLM_PAGE_SHIFT);
	reserve_slm_memory(second_input_buf, circular_buf_size, task_3->slm_lba_info);

	// Output buffer (for task_1 - compaction) - now circular
	// When crc_flag is set, this slm_lba_info will be SHARED with CRC task (intermediate buffer)
	task_1->slm_lba_info = alloc_slm_lba_info(output_data_buf, 0, circular_buf_size, task_id_1, NULL, 0);
	{
		struct slm_lba_info *info = (struct slm_lba_info *)task_1->slm_lba_info;
		// Output fields (for compaction to write)
		info->is_output = true;
		info->output_is_circular = true;
		info->host_consumed_logical_offset = 0;
		info->output_logical_total_size = 0; // Set when compaction finalizes

		// Input fields (for CRC to read) - only relevant when crc_flag is set
		if (crc_flag) {
			info->is_circular = true;
			info->logical_total_size = total_input_size;
			info->load_complete = false;
		}
#if (USE_HEAD_TAIL_DEP)
		info->use_head_tail = true; /* head/tail in logical bytes (ring) */
#endif
	}
	CSD_DEBUG_MAGIC_COMPACTION("Reserving output_data_buf (intermediate): slm_page=%lu, pages=%lu, crc_flag=%d\n",
		   get_slm_offset(output_data_buf) >> SLM_PAGE_SHIFT, circular_buf_size >> SLM_PAGE_SHIFT, crc_flag);
	reserve_slm_memory(output_data_buf, circular_buf_size, task_1->slm_lba_info);

	CSD_DEBUG_MAGIC_COMPACTION("Task allocation & SLM allocation SUCCESS\n");

	// 6. Request SLML Load tasks first (task_2 and task_3)
	// Load first level (task_2)
	task_2->host_id = cmd->memory.cdw14;
	atomic_set(&task_2->task_step, CCSD_TASK_SCHEDULE);
	task_2->proc_idx = proc_idx;
	task_2->invalid = false;
	task_2->total_size = first_input_size;
	task_2->requested_io_offset = 0;
	task_2->done_io_offset = 0;
	task_2->input_buf_addr = first_input_buf;
	task_2->output_buf_addr = first_input_buf;
	task_2->program_idx = COPY_TO_SLM_PROGRAM_INDEX;
	task_2->origin_program_idx = ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX;
	task_2->param_size = 0;
	task_2->result = 0;
	task_2->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
	task_2->processed_offset = 0;
	task_2->opcode = opcode;
	task_2->internal_io_count = 0;
	task_2->prev = -1;
	task_2->next = -1;
	compute_core_id = task_table.copy_to_slm_list_id;
	__enqueue_compute_work(TASK_POOL_INDEX(task_id_2), compute_core_id);

	CSD_DEBUG_MAGIC_COMPACTION("Load 1 task %u enqueued, input_buf_addr=%lu\n", task_id_2, task_2->input_buf_addr);

	// Load second level (task_3)
	task_3->host_id = cmd->memory.cdw14;
	atomic_set(&task_3->task_step, CCSD_TASK_SCHEDULE);
	task_3->proc_idx = proc_idx;
	task_3->invalid = false;
	task_3->total_size = second_input_size;
	task_3->requested_io_offset = 0;
	task_3->done_io_offset = 0;
	task_3->input_buf_addr = second_input_buf;
	task_3->output_buf_addr = second_input_buf;
	task_3->program_idx = COPY_TO_SLM_PROGRAM_INDEX;
	task_3->origin_program_idx = ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX;
	task_3->param_size = 0;
	task_3->result = 0;
	task_3->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
	task_3->processed_offset = 0;
	task_3->opcode = opcode;
	task_3->internal_io_count = 0;
	task_3->prev = -1;
	task_3->next = -1;
	compute_core_id = task_table.copy_to_slm_list_id;
	__enqueue_compute_work(TASK_POOL_INDEX(task_id_3), compute_core_id);

	CSD_DEBUG_MAGIC_COMPACTION("Load 2 task %u enqueued, input_buf_addr=%lu\n", task_id_3, task_3->input_buf_addr);

	// 7. Setup and enqueue compaction task (task_1)
	// Set input buffer addresses, output buffer, and metadata buffer in user_param
	user_param->compaction.first_level_start = first_input_buf;
	user_param->compaction.second_level_start = second_input_buf;
	user_param->info.output_buf = output_data_buf;
	user_param->info.output_second_buf = meta_buf_addr; // Metadata buffer (FRONT_META)

	CSD_DEBUG_MAGIC_COMPACTION("Setting up compaction: first_input=%lu, second_input=%lu, output=%lu, meta=%lu\n",
		   first_input_buf, second_input_buf, output_data_buf, meta_buf_addr);

	task_1->host_id = cmd->memory.cdw14;
	atomic_set(&task_1->task_step, CCSD_TASK_SCHEDULE);
	task_1->proc_idx = proc_idx;
	task_1->invalid = false;
	task_1->total_size = first_input_size + second_input_size;
	task_1->requested_io_offset = 0;
	task_1->done_io_offset = 0;
	task_1->input_buf_addr = first_input_buf;
	task_1->output_buf_addr = output_data_buf;
	task_1->program_idx = ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX;

	// If CRC task handles CRC, disable inline CRC in compaction
	if (crc_flag) {
		user_param->compaction.crc_flag = false;  // Compaction will NOT do inline CRC
	} else {
		user_param->compaction.crc_flag = true;  // Compaction will NOT do inline CRC
    }

	// Copy magic_params directly
	memcpy(task_1->params, user_param, sizeof(struct magic_params));
	task_1->param_size = sizeof(struct magic_params);
	task_1->result = 0;
	task_1->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
	task_1->processed_offset = 0;
	task_1->opcode = opcode;
	task_1->internal_io_count = 0;
	task_1->prev = -1;
	task_1->next = -1;

#if (USE_IDLE_COMPUTE_CORE == 1)
		if (__find_idle_compute_core(&compute_core_id) == false) {
			// if these is no idle core, fallback to RR
			compute_core_id = task_table.compute_turn;
			task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
		}
#else
		compute_core_id = task_table.compute_turn;
		task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
#endif
	__enqueue_compute_work(TASK_POOL_INDEX(task_id_1), compute_core_id);

	CSD_DEBUG_MAGIC_COMPACTION("Compaction task %u enqueued to core %d\n", task_id_1, compute_core_id);

	// 7.1 Setup and enqueue CRC task(s) if crc_flag is set
	if (crc_flag) {
		int compaction_core_id = compute_core_id;
		size_t crc_1_output_buf;
		struct slm_lba_info *crc_1_output_info;

		// Share intermediate_buf_1 slm_lba_info with compaction task (CRC 1 reads from here)
		// (input fields already set above when crc_flag is true)
		task_crc_1->slm_lba_info = task_1->slm_lba_info;

		if (crc_cores == 2) {
			// crc_cores=2: CRC1 writes to intermediate_buf_2, CRC2 reads from there
			crc_1_output_buf = intermediate_buf_2;
			crc_1_output_info = alloc_slm_lba_info(intermediate_buf_2, 0,
				circular_buf_size, task_id_crc_1, NULL, 0);
			crc_1_output_info->is_output = true;
			crc_1_output_info->output_is_circular = true;
			crc_1_output_info->host_consumed_logical_offset = 0;
			crc_1_output_info->output_logical_total_size = 0;
			// Also set input fields for CRC2 to read
			crc_1_output_info->is_circular = true;
			crc_1_output_info->logical_total_size = total_input_size;
			crc_1_output_info->load_complete = false;
#if (USE_HEAD_TAIL_DEP)
			crc_1_output_info->use_head_tail = true; /* head/tail in logical bytes (ring) */
#endif
			CSD_DEBUG_MAGIC_COMPACTION("Reserving intermediate_buf_2 (CRC1 output/CRC2 input): slm_page=%lu, pages=%lu\n",
				   get_slm_offset(intermediate_buf_2) >> SLM_PAGE_SHIFT, circular_buf_size >> SLM_PAGE_SHIFT);
			reserve_slm_memory(intermediate_buf_2, circular_buf_size, crc_1_output_info);
		} else {
			// crc_cores=1: CRC1 writes directly to final_output_buf
			crc_1_output_buf = final_output_buf;
			crc_1_output_info = alloc_slm_lba_info(final_output_buf, 0,
				circular_buf_size, task_id_crc_1, NULL, 0);
			crc_1_output_info->is_output = true;
			crc_1_output_info->output_is_circular = true;
			crc_1_output_info->host_consumed_logical_offset = 0;
			crc_1_output_info->output_logical_total_size = 0;
#if (USE_HEAD_TAIL_DEP)
			crc_1_output_info->use_head_tail = true; /* head/tail in logical bytes (ring) */
#endif
			CSD_DEBUG_MAGIC_COMPACTION("Reserving final_output_buf (CRC output): slm_page=%lu, pages=%lu\n",
				   get_slm_offset(final_output_buf) >> SLM_PAGE_SHIFT, circular_buf_size >> SLM_PAGE_SHIFT);
			reserve_slm_memory(final_output_buf, circular_buf_size, crc_1_output_info);
		}

		// Setup CRC task 1 (processes even blocks, or all blocks if crc_cores=1)
		task_crc_1->host_id = cmd->memory.cdw14;
		atomic_set(&task_crc_1->task_step, CCSD_TASK_SCHEDULE);
		task_crc_1->proc_idx = proc_idx;
		task_crc_1->invalid = false;
		task_crc_1->total_size = total_input_size;
		task_crc_1->requested_io_offset = 0;
		task_crc_1->done_io_offset = 0;
		task_crc_1->input_buf_addr = intermediate_buf_1;
		task_crc_1->output_buf_addr = crc_1_output_buf;
		task_crc_1->program_idx = ROCKSDB_MAGIC_CRC_PROGRAM_INDEX;

		// Setup CRC 1 params
		struct rocksdb_magic_crc_params *crc_1_params =
			(struct rocksdb_magic_crc_params *)task_crc_1->params;
		crc_1_params->input_buf = intermediate_buf_1;
		crc_1_params->output_buf = crc_1_output_buf;
		crc_1_params->output_buf_size = circular_buf_size;
		crc_1_params->input_logical_size = total_input_size;
		crc_1_params->sstable_size = sstable_size;
		crc_1_params->datablock_threshold = user_param->compaction.datablock_threshold;
		crc_1_params->num_cores = crc_cores;
		crc_1_params->core_id = 0;  // CRC1 handles even blocks (block_num % num_cores == 0)

		task_crc_1->param_size = sizeof(struct rocksdb_magic_crc_params);
		task_crc_1->result = 0;
		task_crc_1->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
		task_crc_1->processed_offset = 0;
		task_crc_1->opcode = opcode;
		task_crc_1->internal_io_count = 0;
		task_crc_1->prev = -1;
		task_crc_1->next = -1;

		// Enqueue CRC task 1 to a DIFFERENT core than compaction
		int crc_1_core_id;
#if (USE_IDLE_COMPUTE_CORE == 1)
		if (__find_idle_compute_core(&crc_1_core_id) == false) {
			// if these is no idle core, fallback to RR
			crc_1_core_id = task_table.compute_turn;
			task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
		}
#else
		crc_1_core_id = task_table.compute_turn;
		task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
#endif
		__enqueue_compute_work(TASK_POOL_INDEX(task_id_crc_1), crc_1_core_id);

		CSD_DEBUG_MAGIC_COMPACTION("CRC task 1 (id=%u) enqueued to core %d (compaction on core %d), num_cores=%d, core_id=0\n",
			   task_id_crc_1, crc_1_core_id, compaction_core_id, crc_cores);

		// Setup CRC task 2 if crc_cores == 2
		if (crc_cores == 2) {
			// CRC2 reads from intermediate_buf_2 (CRC1 output), writes to final_output_buf
			task_crc_2->slm_lba_info = crc_1_output_info;

			// Setup final output buffer slm_lba_info for CRC2
			struct slm_lba_info *final_output_info = alloc_slm_lba_info(final_output_buf, 0,
				circular_buf_size, task_id_crc_2, NULL, 0);
			final_output_info->is_output = true;
			final_output_info->output_is_circular = true;
			final_output_info->host_consumed_logical_offset = 0;
			final_output_info->output_logical_total_size = 0;
#if (USE_HEAD_TAIL_DEP)
			final_output_info->use_head_tail = true; /* head/tail in logical bytes (ring) */
#endif
			CSD_DEBUG_MAGIC_COMPACTION("Reserving final_output_buf (CRC2 output): slm_page=%lu, pages=%lu\n",
				   get_slm_offset(final_output_buf) >> SLM_PAGE_SHIFT, circular_buf_size >> SLM_PAGE_SHIFT);
			reserve_slm_memory(final_output_buf, circular_buf_size, final_output_info);

			// Setup CRC task 2 (processes odd blocks)
			task_crc_2->host_id = cmd->memory.cdw14;
			atomic_set(&task_crc_2->task_step, CCSD_TASK_SCHEDULE);
			task_crc_2->proc_idx = proc_idx;
			task_crc_2->invalid = false;
			task_crc_2->total_size = total_input_size;
			task_crc_2->requested_io_offset = 0;
			task_crc_2->done_io_offset = 0;
			task_crc_2->input_buf_addr = intermediate_buf_2;
			task_crc_2->output_buf_addr = final_output_buf;
			task_crc_2->program_idx = ROCKSDB_MAGIC_CRC_PROGRAM_INDEX;

			// Setup CRC 2 params
			struct rocksdb_magic_crc_params *crc_2_params =
				(struct rocksdb_magic_crc_params *)task_crc_2->params;
			crc_2_params->input_buf = intermediate_buf_2;
			crc_2_params->output_buf = final_output_buf;
			crc_2_params->output_buf_size = circular_buf_size;
			crc_2_params->input_logical_size = total_input_size;
			crc_2_params->sstable_size = sstable_size;
			crc_2_params->datablock_threshold = user_param->compaction.datablock_threshold;
			crc_2_params->num_cores = crc_cores;
			crc_2_params->core_id = 1;  // CRC2 handles odd blocks (block_num % num_cores == 1)

			task_crc_2->param_size = sizeof(struct rocksdb_magic_crc_params);
			task_crc_2->result = 0;
			task_crc_2->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
			task_crc_2->processed_offset = 0;
			task_crc_2->opcode = opcode;
			task_crc_2->internal_io_count = 0;
			task_crc_2->prev = -1;
			task_crc_2->next = -1;

			// Enqueue CRC task 2 to a DIFFERENT core than CRC1
			int crc_2_core_id;
#if (USE_IDLE_COMPUTE_CORE == 1)
			if (__find_idle_compute_core(&crc_2_core_id) == false) {
				// if these is no idle core, fallback to RR
				crc_2_core_id = task_table.compute_turn;
				task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
			}
#else
			crc_2_core_id = task_table.compute_turn;
			task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
#endif
			__enqueue_compute_work(TASK_POOL_INDEX(task_id_crc_2), crc_2_core_id);

			CSD_DEBUG_MAGIC_COMPACTION("CRC task 2 (id=%u) enqueued to core %d, num_cores=%d, core_id=1\n",
				   task_id_crc_2, crc_2_core_id, crc_cores);
		}
	}

	// 8. Return output buffer info to host via magic_info (So that the host can read & free it later)
	if (crc_flag) {
		// With CRC task: host reads from final_output_buf
		user_param->info.output_buf = get_slm_offset(final_output_buf);
	} else {
		// Without CRC task: host reads from output_data_buf
		user_param->info.output_buf = get_slm_offset(output_data_buf);
	}
	user_param->info.output_buf_size = MAGIC_BUFFER_SIZE;
	user_param->info.output_second_buf = get_slm_offset(meta_buf_addr); // Metadata buffer offset
	user_param->info.output_second_buf_size = FRONT_METADATA_SIZE;
	user_param->compaction.first_level_size = first_input_size;
	user_param->compaction.second_level_size = second_input_size;

	CSD_DEBUG_MAGIC_COMPACTION("Returning to host: output_slm_page=%lu, meta_slm_page=%lu, output_buf_size=%lu, crc_flag=%d\n",
		   user_param->info.output_buf >> SLM_PAGE_SHIFT,
		   user_param->info.output_second_buf >> SLM_PAGE_SHIFT,
		   user_param->info.output_buf_size, crc_flag);

	__copy_prp_data(sqid, sq_entry, &(user_param->info), sizeof(struct magic_info), false);
	// __copy_prp_data(sqid, sq_entry, &magic_param, sizeof(struct ccsd_magic_parameter), false);

	CSD_DEBUG_MAGIC_COMPACTION("START ROCKSDB MAGIC COMPACTION DONE\n");

	return true;
}

static bool __do_perform_magic_read(int sqid, int sq_entry, unsigned int proc_idx, unsigned int *status, size_t *result)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvme_command_csd *cmd = (struct nvme_command_csd *)(&sq_entry(sq_entry));

	int compute_core_id = 0;
	int opcode = cmd->common.opcode;

	/* Task variables: up to 4 load tasks + 1 compute task */
	uint32_t load_task_ids[MAX_MAGIC_READ_FILES] = {TASK_ID_NONE, TASK_ID_NONE, TASK_ID_NONE, TASK_ID_NONE};
	struct ccsd_task_info *load_tasks[MAX_MAGIC_READ_FILES] = {NULL, NULL, NULL, NULL};
	uint32_t compute_task_id;
	struct ccsd_task_info *compute_task = NULL;

	struct source_range_entry *sres[MAX_MAGIC_READ_FILES] = {NULL, NULL, NULL, NULL};
	struct magic_params *user_param = (struct magic_params *)(&magic_read_param.param);
	int num_files;
	int i;

	size_t input_buf = -1;
	size_t output_buf = -1;
	size_t total_input_size = 0;
	size_t file_start_offset[MAX_MAGIC_READ_FILES] = {0};

	/* 0. Copy magic read params from host */
	CSD_DEBUG_MAGIC_READ("START MAGIC READ\n");
	__copy_prp_data(sqid, sq_entry, &magic_read_param,
			sizeof(struct ccsd_magic_read_parameter), true);

	num_files = magic_read_param.num_files;
	CSD_DEBUG_MAGIC_READ("num_files: %d\n", num_files);
	if (num_files < 1 || num_files > MAX_MAGIC_READ_FILES) {
		NVMEV_ERROR("[MAGIC_READ] Invalid num_files: %d\n", num_files);
		return false;
	}

	/* 1. Calculate total input size and per-file start offsets (fixed per-file SLM allocation) */
	size_t per_file_slm_size = (MAGIC_READ_BUFFER_PAGES / MAX_MAGIC_READ_FILES) * SLM_PAGE_SIZE;
	for (i = 0; i < num_files; i++) {
		file_start_offset[i] = total_input_size;
		total_input_size += per_file_slm_size;
		CSD_DEBUG_MAGIC_READ("file[%d]: size=%lu, sre_count=%d, start_offset=%lu\n",
			i, magic_read_param.file_size[i], magic_read_param.sre_count[i],
			file_start_offset[i]);
	}
	CSD_DEBUG_MAGIC_READ("total_input_size: %lu\n", total_input_size);

	/* 2. Allocate tasks: 1 compute + num_files load tasks */
	compute_task_id = allocate_new_task(&compute_task);
	if (compute_task_id == TASK_ID_NONE) {
		NVMEV_ERROR("[MAGIC_READ] No free task for compute\n");
		return false;
	}
	CSD_DEBUG_MAGIC_READ("compute_task_id: %u\n", compute_task_id);

	for (i = 0; i < num_files; i++) {
		load_task_ids[i] = allocate_new_task(&load_tasks[i]);
		if (load_task_ids[i] == TASK_ID_NONE) {
			NVMEV_ERROR("[MAGIC_READ] No free task for load %d\n", i);
			__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(compute_task_id));
			for (int j = 0; j < i; j++)
				__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(load_task_ids[j]));
			return false;
		}
		CSD_DEBUG_MAGIC_READ("load_task_ids[%d]: %u\n", i, load_task_ids[i]);
	}

	/* 3. Copy SREs */
	for (i = 0; i < num_files; i++) {
		int nentry = magic_read_param.sre_count[i];
		if (nentry > 0) {
			sres[i] = (struct source_range_entry *)kzalloc_node(
				sizeof(struct source_range_entry) * nentry, GFP_KERNEL, 1);
			if (sres[i] == NULL) {
				NVMEV_ERROR("[MAGIC_READ] Failed to allocate sre for file %d\n", i);
				BUG();
			}
			memcpy(sres[i], magic_read_param.sre[i],
			       sizeof(struct source_range_entry) * nentry);
		}
	}

	/* 4. Allocate SLM buffers: one contiguous input + one page output */
	input_buf = alloc_slm_range(per_file_slm_size * num_files);
	output_buf = alloc_slm_range(16384);
	CSD_DEBUG_MAGIC_READ("SLM alloc: input_buf=%lu, output_buf=%lu\n",
		get_slm_offset(input_buf) >> SLM_PAGE_SHIFT,
		get_slm_offset(output_buf) >> SLM_PAGE_SHIFT);

	if (input_buf == -1 || output_buf == -1) {
		NVMEV_ERROR("[MAGIC_READ] SLM allocation failed\n");
		*status = (NVME_SCT_CMD_SPECIFIC_STATUS << 8) | NVME_SC_MAX_MEM_RANGE_EXCEEDED;
		__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(compute_task_id));
		for (i = 0; i < num_files; i++) {
			__enqueue_task(&task_table.free_list, TASK_POOL_INDEX(load_task_ids[i]));
			if (sres[i]) vfree(sres[i]);
		}
		if (input_buf != -1) free_slm_range(input_buf);
		if (output_buf != -1) free_slm_range(output_buf);
		return false;
	}

	/* 5. Setup and enqueue load tasks (one per file) */
	for (i = 0; i < num_files; i++) {
		size_t file_buf_addr = input_buf + (i * per_file_slm_size);
		size_t file_sz = magic_read_param.file_size[i];
		size_t alloc_sz = per_file_slm_size;
		int nentry = magic_read_param.sre_count[i];

		load_tasks[i]->slm_lba_info = alloc_slm_lba_info(
			file_buf_addr, 0, alloc_sz, load_task_ids[i], sres[i], nentry);
		/* Both per-file modes start on head/tail. type 1 is a direct sequential
		 * block read (host pre-positions the block at the slice base, offset 0)
		 * and stays a pure head/tail stream. type 0 is the scattered path: it
		 * head/tail-tracks its forced page-0 prefetch, then the first probe past
		 * ht_head flips it to extent_mapped (COMPACT) in slm_request_demand_read_info. */
		((struct slm_lba_info *)load_tasks[i]->slm_lba_info)->use_head_tail = true;
		reserve_slm_memory(file_buf_addr, alloc_sz, load_tasks[i]->slm_lba_info);

		load_tasks[i]->host_id = cmd->memory.cdw14;
		atomic_set(&load_tasks[i]->task_step, CCSD_TASK_SCHEDULE);
		load_tasks[i]->proc_idx = proc_idx;
		load_tasks[i]->invalid = false;
		// Set io_quota
		load_tasks[i]->is_first_io = true;
		load_tasks[i]->io_quota = 1;
		load_tasks[i]->total_size = file_sz;
		load_tasks[i]->requested_io_offset = 0;
		load_tasks[i]->done_io_offset = 0;
		load_tasks[i]->input_buf_addr = file_buf_addr;
		load_tasks[i]->output_buf_addr = file_buf_addr;
		load_tasks[i]->program_idx = COPY_TO_SLM_PROGRAM_INDEX;
		load_tasks[i]->origin_program_idx = ROCKSDB_MAGIC_READ_PROGRAM_INDEX;
		load_tasks[i]->param_size = 0;
		load_tasks[i]->result = 0;
		load_tasks[i]->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
		load_tasks[i]->processed_offset = 0;
		load_tasks[i]->opcode = opcode;
		load_tasks[i]->internal_io_count = 0;
		load_tasks[i]->prev = -1;
		load_tasks[i]->next = -1;

		compute_core_id = task_table.copy_to_slm_list_id;
		__enqueue_compute_work(TASK_POOL_INDEX(load_task_ids[i]), compute_core_id);
		CSD_DEBUG_MAGIC_READ("load task[%d]: buf_addr=%lu, size=%lu, alloc=%lu, sre_count=%d, slm_lba_info=%p\n",
			i, get_slm_offset(file_buf_addr) >> SLM_PAGE_SHIFT, file_sz, alloc_sz, nentry, load_tasks[i]->slm_lba_info);
	}

	/* 6. Setup compute task */
	user_param->read.input_buf = input_buf;
	user_param->read.output_buf = output_buf;
	user_param->read.output_buf_size = 16384;
	user_param->read.num_files = num_files;
	for (i = 0; i < num_files; i++) {
		user_param->read.file_start_offset[i] = file_start_offset[i];
		user_param->read.file_size[i] = magic_read_param.file_size[i];
	}
	/* Ensure read_params.start_offset matches file_start_offset */
	for (i = 0; i < num_files; i++) {
		user_param->read.read_params.start_offset[i] = file_start_offset[i];
	}
	/* Store load task IDs for deferred SLM free */
	user_param->read.num_load_tasks = num_files;
	for (i = 0; i < num_files; i++) {
		user_param->read.load_task_ids[i] = load_task_ids[i];
	}

	compute_task->slm_lba_info = alloc_slm_lba_info(
		output_buf, 0, 16384, TASK_ID_NONE, NULL, 0);
	((struct slm_lba_info *)compute_task->slm_lba_info)->is_output = true;
	/* Magic-read output (linear, host-read): publish via head/tail. The compute
	 * finalizes with finalize_slm_data_ready + notify_slm_data_ready, which set
	 * ht_producer_done/ht_head; the host reads it via check_output_slm_data_ready. */
	((struct slm_lba_info *)compute_task->slm_lba_info)->use_head_tail = true;
	reserve_slm_memory(output_buf, 16384, compute_task->slm_lba_info);

	compute_task->host_id = cmd->memory.cdw14;
	atomic_set(&compute_task->task_step, CCSD_TASK_SCHEDULE);
	compute_task->proc_idx = proc_idx;
	compute_task->invalid = false;
	{
		size_t actual_input_size = 0;
		for (int j = 0; j < num_files; j++)
			actual_input_size += magic_read_param.file_size[j];
		compute_task->total_size = actual_input_size;
	}
	compute_task->requested_io_offset = 0;
	compute_task->done_io_offset = 0;
	compute_task->input_buf_addr = input_buf;
	compute_task->output_buf_addr = output_buf;
	compute_task->program_idx = ROCKSDB_MAGIC_READ_PROGRAM_INDEX;

	memcpy(compute_task->params, user_param, sizeof(struct magic_params));
	compute_task->param_size = sizeof(struct magic_params);
	compute_task->result = 0;
	compute_task->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;
	compute_task->processed_offset = 0;
	compute_task->opcode = opcode;
	compute_task->internal_io_count = 0;
	compute_task->prev = -1;
	compute_task->next = -1;

#if (USE_IDLE_COMPUTE_CORE == 1)
		if (__find_idle_compute_core(&compute_core_id) == false) {
			// if these is no idle core, fallback to RR
			NVMEV_ERROR("NO IDLE CORE!!!!\n");
			compute_core_id = task_table.compute_turn;
			task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
		}
#else
		compute_core_id = task_table.compute_turn;
		task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
#endif
	__enqueue_compute_work(TASK_POOL_INDEX(compute_task_id), compute_core_id);
	CSD_DEBUG_MAGIC_READ("compute task enqueued: task_id=%u, core=%d, input_buf=%lu, output_buf=%lu, total_size=%lu\n",
		compute_task_id, compute_core_id,
		get_slm_offset(input_buf) >> SLM_PAGE_SHIFT,
		get_slm_offset(output_buf) >> SLM_PAGE_SHIFT,
		total_input_size);

	/* 7. Return buffer info to host via magic_info */
	user_param->info.output_buf = get_slm_offset(output_buf);
	user_param->info.output_buf_size = 16384;
	user_param->info.output_second_buf = 0;
	user_param->info.output_second_buf_size = 0;

	__copy_prp_data(sqid, sq_entry, &(user_param->info),
			sizeof(struct magic_info), false);

	CSD_DEBUG_MAGIC_READ("MAGIC READ DONE: output_buf_offset=%lu, output_buf_size=%lu\n",
		user_param->info.output_buf, user_param->info.output_buf_size);
	return true;
}

static bool __do_perform_dispatch(int sqid, int sq_entry, unsigned int proc_idx, unsigned int *status)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvme_command_csd *cmd = (struct nvme_command_csd *)(&sq_entry(sq_entry));

	struct ccsd_parameter param;

	size_t input_size = 0;
	int nentry; // TODO

	int compute_core_id = 0;
	int opcode = cmd->common.opcode;
	int program_idx;
	uint32_t gen_id;
	struct ccsd_task_info *task = NULL;

	size_t lba = 0;
	size_t requested_io_offset = 0;
	size_t done_io_offset = 0;
	size_t input_buf_addr = 0;
	size_t output_buf_addr = 0;
	size_t output_size = 0;

	gen_id = allocate_new_task(&task);
	if (gen_id == TASK_ID_NONE) {
		int comp_total = 0;
		int i;
		for (i = 0; i < task_table.num_cpu_resources + 1; i++) {
			comp_total += task_table.comp_list[i].running;
		}
		NVMEV_ERROR("No free task available: total(%d) - free: %d, comp: %d, done: %d\n",
				task_table.free_list.running + comp_total + task_table.done_list.running,
		       task_table.free_list.running, comp_total, task_table.done_list.running);
		BUG();
		// No free task
		return false;
	}

	if (opcode == nvme_cmd_execute_program) {
		__copy_prp_data(sqid, sq_entry, &param, sizeof(struct ccsd_parameter), true);

		lba = 0;
		input_size = param.nByte;
		program_idx = cmd->execute_program.pind;

		requested_io_offset = param.nByte;
		done_io_offset = param.nByte;

		input_buf_addr = get_slm_addr(param.input_slm);
		output_buf_addr = get_slm_addr(param.output_slm);

		if (input_size >= 0xFFFFFFFF) {
			NVMEV_ERROR("Invalid input size\n");
			return true;
		}

		output_size = get_slm_range_size(output_buf_addr);
		if (input_size < output_size) {
			output_size = input_size;
		}
		
		task->slm_lba_info = alloc_slm_lba_info(output_buf_addr, 0, output_size, gen_id, NULL, 0);
		((struct slm_lba_info *)task->slm_lba_info)->is_output = true;
		/* All non-magic compute outputs are tracked with head/tail (sequential
		 * single-writer), whether consumed in-SLM (compaction feeds CRC) or
		 * host-read (CRC's own output, and every other compute program's output).
		 * Magic programs use their own circular output rings (output_is_circular). */
		if (program_idx != ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX &&
		    program_idx != ROCKSDB_MAGIC_CRC_PROGRAM_INDEX &&
		    program_idx != ROCKSDB_MAGIC_READ_PROGRAM_INDEX)
			((struct slm_lba_info *)task->slm_lba_info)->use_head_tail = true;
#if (SUPPORT_ASYNC_COMPUTE == 1)
		reserve_slm_memory(output_buf_addr, output_size, task->slm_lba_info);
#else
		/* Host-managed (synchronous) CSD: the compute writes its whole output
		 * before the host issues the consuming command, so publish the output
		 * region as ready now (same head/tail one-shot as the input load). */
		set_slm_ready_for_csd(output_buf_addr, output_size, (struct slm_lba_info *)task->slm_lba_info);
#endif
	} else if (opcode == nvme_cmd_memory_copy) {
		uint32_t io_task_id = TASK_ID_NONE;
		nentry = cmd->memory_copy.format;
		struct source_range_entry *sre =
			(struct source_range_entry *)kzalloc_node(sizeof(struct source_range_entry) * nentry, GFP_KERNEL, 1);

		__copy_prp_data(sqid, sq_entry, sre, sizeof(struct source_range_entry) * nentry, true);
		// for (int i = 0; i < nentry; i++) {
		// 	printk("sre[%d].saddr:%llu, sre[%d].nByte:%llu", i, sre[i].saddr, i, sre[i].nByte);
		// }

		input_size = cmd->memory_copy.length;
		program_idx = COPY_TO_SLM_PROGRAM_INDEX;
		param.param_size = 0;
		input_buf_addr = get_slm_addr(cmd->memory_copy.sdaddr);
		output_buf_addr = input_buf_addr;

		NVMEV_CSD_INFO("SCHD", "Issue SLMCPY (lba:%lu, size:%lu, input_addr:%lu, sre:%d)", lba, input_size, input_buf_addr, nentry);

		task->is_first_io = false;
		task->io_quota = 0;

#if (SUPPORT_ASYNC_COMMAND == 0)
		// The requested SLM offset has not been allocated
		if (check_allocated_slm_range(input_buf_addr, input_size) == false) {
			NVMEV_ERROR("[%s] SLM Range unallocated - input_addr: %lu, size: %lu\n", __FUNCTION__, input_buf_addr,
						input_size);
			*status = ((NVME_SCT_CMD_SPECIFIC_STATUS << 8) | NVME_SC_CMD_SIZE_LIMIT_EXCEED);
			kfree(sre);
			return false;
		}
#endif

		task->slm_lba_info = alloc_slm_lba_info(input_buf_addr, lba, input_size, gen_id, sre, nentry);
		/* Non-magic input load: start on head/tail (the only readiness mechanism).
		 * Sequential loads (compaction input) stream on head/tail; random-access
		 * loads (rocksdb_read/btree) flip one-way to demand/extent on the first
		 * scattered probe in slm_request_demand_read (use_head_tail -> extent_mapped). */
		((struct slm_lba_info *)task->slm_lba_info)->use_head_tail = true;
#if (SUPPORT_ASYNC_COMMAND == 1)
#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
		task->is_first_io = true;
		task->io_quota = 1; // turn on 1 again. Btree and rocksdb direct data read will favor 1
		io_task_id = get_io_task_from_slm_addr(input_buf_addr);
		if (unlikely(io_task_id != TASK_ID_NONE)) {
			BUG();
		}
#endif
		reserve_slm_memory(input_buf_addr, input_size, task->slm_lba_info);
#else
		/* Host-managed (synchronous) CSD: the memory_copy command places the whole
		 * input region in SLM and the compute command only follows afterwards, so
		 * publish the entire region as ready now. */
		set_slm_ready_for_csd(input_buf_addr, input_size, (struct slm_lba_info *)task->slm_lba_info);
#endif
	} else {
		NVMEV_ERROR("opcode error : proc_index:%d, opcode:%d", proc_idx, opcode);
		BUG();
	}

	// 1. Make task
	NVMEV_CSD_INFO(
		"SCHD",
		"Result - task_id:%u(program_idx:%d), input_size:%lu, nentry:%d, host_id:%d, input_offset:%lu, output_offset:%lu",
		gen_id, program_idx, input_size / SLM_PAGE_SIZE, nentry, cmd->memory.cdw14,
		get_slm_offset(input_buf_addr) / SLM_PAGE_SIZE, get_slm_offset(output_buf_addr) / SLM_PAGE_SIZE);

	task->host_id = cmd->memory.cdw14;
	atomic_set(&task->task_step, CCSD_TASK_SCHEDULE);
	task->proc_idx = proc_idx;
	task->invalid = false;
	task->total_size = input_size;

	task->requested_io_offset = requested_io_offset;
	task->done_io_offset = done_io_offset;

	task->input_buf_addr = input_buf_addr;
	task->output_buf_addr = output_buf_addr;
	task->program_idx = program_idx;
	task->param_size = param.param_size;

	if (param.param_size != 0) {
		memcpy(task->params, param.param, param.param_size);
	}
	task->result = 0;
	task->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;

	task->processed_offset = 0;
	task->opcode = opcode;
	task->internal_io_count = 0;
	//task->io_quota = 0;

	task->prev = -1;
	task->next = -1;
	if (opcode == nvme_cmd_memory_copy) {
		compute_core_id = task_table.copy_to_slm_list_id;
	} else {
#if (USE_ONLY_ONE_COMPUTE_CORE == 1)
		compute_core_id = 0;
#elif (USE_IDLE_COMPUTE_CORE == 1)
		if (__find_idle_compute_core(&compute_core_id) == false) {
			// if these is no idle core, fallback to RR
			compute_core_id = task_table.compute_turn;
			task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
		}
#else
		compute_core_id = task_table.compute_turn;
		task_table.compute_turn = (task_table.compute_turn + 1) % task_table.num_cpu_resources;
#endif
	}
	__enqueue_compute_work(TASK_POOL_INDEX(gen_id), compute_core_id);
	return true;
}

static void __fill_cq_result(int sqid, int cqid, int sq_entry, unsigned int command_id, unsigned int status,
							 size_t result)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head;

	NVMEV_CSD_INFO("SCHD", "cq_result : result : %lu", result);

	spin_lock(&cq->entry_lock);
	cq_head = cq->cq_head;
	cq_entry(cq_head).command_id = command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_entry;
	cq_entry(cq_head).status = cq->phase | (status << 1);
	cq_entry(cq_head).result0 = result & 0xFFFFFFFF;
	cq_entry(cq_head).result1 = result >> 32;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static void __reclaim_io_req(void)
{
	unsigned int turn;
	unsigned int first_entry = -1;
	unsigned int last_entry = -1;
	unsigned int curr;
	unsigned int total_count = 0;
	struct csd_internal_io_req *io_req;

	unsigned long long curr_nsecs_wall = __get_wallclock();
	unsigned long long curr_nsecs_local = local_clock();
	long long delta = curr_nsecs_wall - curr_nsecs_local;
	unsigned long long curr_nsecs = local_clock() + delta;

	// SLM Queue
	for (turn = 0; turn < task_table.num_slm_resources; turn++) {
		int count = 0;
		struct ccsd_list *slm_list = &(io_req_table.slm_list[turn]);
		first_entry = slm_list->head;
		curr = first_entry;
		last_entry = -1;

		while (curr != -1) {
			io_req = &(io_req_table.io_req[curr]);

			if (io_req->io_req_status == CSD_INTERNAL_IO_REQ_WAITING_TIME) {
				if (curr_nsecs >= io_req->nsecs_target) {
					struct ccsd_task_info *task = &(task_table.task[TASK_POOL_INDEX(io_req->task_id)]);
					io_req->io_req_status = CSD_INTERNAL_IO_REQ_DONE;

					NVMEV_CSD_PROFILE_WITH_TIME("NVMREAD", 0, task->host_id, 0, io_req->nsecs_nand_start,
												io_req->nsecs_target);
#ifdef CONFIG_NVMEV_PROFILE_IO
					atomic64_add(io_req->length, &vdev->io_stat_csd_read);
#endif
					NVMEV_DEBUG(
						"T_%d_ReadSource - TASK_%d_%d - io_size:%llu, io_lba:%llu, cur_time:%llu, target_time:%llu, diff:%llu",
						task->host_id, io_req->task_id, io_req_id, io_req->length, io_req->lba, curr_nsecs,
						io_req->nsecs_target, curr_nsecs - io_req->nsecs_target);
					task->is_first_io = false;
				}
			}

			if (io_req->io_req_status == CSD_INTERNAL_IO_REQ_DONE) {
				uint32_t task_id = TASK_POOL_INDEX(io_req->task_id);
				struct ccsd_task_info *task = &(task_table.task[task_id]);

				/* Detect orphaned I/O: SLM page was reallocated */
				bool is_orphan_io = false;
				{
					size_t page_idx = get_slm_offset(io_req->buf_addr) / SLM_PAGE_SIZE;
					struct slm_lba_info *page_info = slm_data_ready_info[page_idx].slm_lba_info;
					if (page_info != task->slm_lba_info || page_info == NULL || !TASK_GEN_VALID(io_req->task_id, task)) {
						/* An orphan I/O: the SLM page has been reallocated to a different task,
						likely due to a new command from the host that uses the same SLM buffer
						for demand read. We should skip notify_slm_data_ready for this I/O since
						the original task is no longer waiting for this data, and it can cause data
						corruption if we incorrectly notify the new task that happens to use the same SLM page.
						*/
						is_orphan_io = true;
						CSD_DEBUG_MAGIC_READ("ORPHAN RECLAIM: task_%d, io_req_%d, buf_page=%lu, length=%lu page_info=%lu task->slm_lba_info=%lu\n",
							task_id, curr, page_idx, io_req->length, page_info, task->slm_lba_info);
					}
				}

				if (!is_orphan_io) {
#if (SUPPORT_ASYNC_COMMAND == 1)
					struct slm_lba_info *lba_info = (struct slm_lba_info *)task->slm_lba_info;
					bool is_circular = (lba_info != NULL && lba_info->is_circular);
					size_t logical_total = is_circular ? lba_info->logical_total_size : task->total_size;

					/* For circular buffers, handle ghost page copy and notification */
					if (is_circular) {
						size_t physical_offset = io_req->buf_addr - task->input_buf_addr;
						if (physical_offset < GHOST_PAGE_SIZE) {
							/* Copy data to ghost page region */
							size_t ghost_copy_size = GHOST_PAGE_SIZE - physical_offset;
							if (ghost_copy_size > io_req->length)
								ghost_copy_size = io_req->length;
							size_t ghost_addr = task->input_buf_addr + MAGIC_BUFFER_SIZE + physical_offset;
							/* Copy from main buffer to ghost page (both are SLM addresses) */
							memcpy((void *)ghost_addr, (void *)io_req->buf_addr, ghost_copy_size);
							CSD_DEBUG_MAGIC_INPUT("Ghost copy: phys_off=%lu, copy_size=%lu, ghost_addr=%lu\n",
								   physical_offset, ghost_copy_size, ghost_addr);
						}
					}

					// The original flow had a pit fall
					// When the final load is small, it might finish before the previous load.
					// This leads to missing notification for the final part (misaligned slm dat aready)
					// Therefore We are going to use the final_io for notify misaligned

					/* After demand read batch completes with no more IOs in flight,
					   park the task in WAIT so __request_io won't touch slm_lba_info.
					   his should be done before notify_slm_data_ready, because it might cause a
					   dead lock. A new request might be requested but the task_step is stuck at
					   CCSD_TASK_WAIT*/

					if (io_req->demand_read && io_req->last_io &&
					    atomic_cmpxchg(&task->task_step, CCSD_TASK_SCHEDULE, CCSD_TASK_WAIT) == CCSD_TASK_SCHEDULE) {
						CSD_DEBUG_MAGIC_READ("task_id=%d io_req_id: %d enters WAIT after demand read completion with no more IO in flight\n", task_id, curr);
					}

#if (USE_HEAD_TAIL_DEP)
					if (is_circular && lba_info != NULL && lba_info->use_head_tail) {
						/* Head/tail ring: advance the produced frontier in LOGICAL
						 * bytes (byte-exact, out-of-order coalesced). Subsumes both
						 * notify variants (DIVIDE_UP readiness covers the misaligned
						 * final chunk) and the ghost notify (ghost validity follows
						 * from the wrapped bytes' logical readiness; the ghost
						 * memcpy above precedes this publish in program order). */
						notify_circular_load_ready(lba_info, io_req->stream_offset,
									   io_req->length);
					} else
#endif
					if (io_req->sequential_last_io) {
						if (lba_info != NULL) {
							notify_misaligned_slm_data_ready(io_req->buf_addr, io_req->length);
						}
					} else {
						if (lba_info != NULL) {
							notify_slm_data_ready(io_req->buf_addr, io_req->length);
						}
					}

					/* For circular buffers, also notify ghost page if applicable */
					if (is_circular
#if (USE_HEAD_TAIL_DEP)
					    && (lba_info == NULL || !lba_info->use_head_tail)
#endif
					) {
						size_t physical_offset = io_req->buf_addr - task->input_buf_addr;
						if (physical_offset < GHOST_PAGE_SIZE) {
							size_t ghost_copy_size = GHOST_PAGE_SIZE - physical_offset;
							if (ghost_copy_size > io_req->length)
								ghost_copy_size = io_req->length;
							size_t ghost_addr = task->input_buf_addr + MAGIC_BUFFER_SIZE + physical_offset;
							notify_slm_data_ready(ghost_addr, ghost_copy_size);
						}
					}

#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
					if (io_req->demand_read && io_req->last_io) {
						if (task->slm_lba_info != NULL) {
							complete_slm_demand_read(io_req->buf_addr, io_req->length);
						}
					}
#endif
#endif
					if (!io_req->demand_read) {
						task->done_io_offset += io_req->length;

#if (SUPPORT_ASYNC_COMMAND == 1)
						/* For circular buffers, set load_complete when all IO is done */
						struct slm_lba_info *ci_info = (struct slm_lba_info *)task->slm_lba_info;
						if (ci_info != NULL && ci_info->is_circular) {
							if (task->done_io_offset >= ci_info->logical_total_size) {
								ci_info->load_complete = true;
								CSD_DEBUG_MAGIC_INPUT("load_complete=true, lba_info=%p\n", ci_info);
							}
						}
#endif
					}

					if (task->done_io_offset > task->requested_io_offset) {
						NVMEV_ERROR("BUG::task->done_io_offset > task->requested_io_offset (%lu > %lu)",
									task->done_io_offset, task->requested_io_offset);
						BUG();
					}

					task->internal_io_count--;
				}
				last_entry = curr;
				curr = io_req->next;

				io_req->io_req_status = CSD_INTERNAL_IO_REQ_FREE;
				slm_list->running = slm_list->running - 1;
				NVMEV_DEBUG("[COMPLETION_ALERT] SLM_Q[%u] - TASK_%d_%d  - %d (%d)", turn, task_id, last_entry, count++,
							total_count++);
				NVMEV_DEBUG("I/O_Count:%u, done_io_offset:%lu", task->internal_io_count, task->done_io_offset);

				notify_internal_cmd();
				io_req_table.enqueuing_io_size = io_req_table.enqueuing_io_size - io_req->length;

			} else {
				break;
			}
		}

		if (last_entry != -1) {
			io_req = &(io_req_table.io_req[last_entry]);
			slm_list->head = io_req->next;
			if (io_req->next != -1) {
				io_req_table.io_req[io_req->next].prev = -1;
			}
			io_req->next = -1;
			__enqueue_io_req_free_list(first_entry, last_entry);
		}
	}

	curr = task_table.comp_list[task_table.copy_to_slm_list_id].head;
	while (curr != -1) {
		struct ccsd_task_info *task = &(task_table.task[curr]);

		if (atomic_read(&task->task_step) == CCSD_TASK_SCHEDULE) {
			if (task->done_io_offset >= task->total_size) {
				atomic_cmpxchg(&task->task_step, CCSD_TASK_SCHEDULE, CCSD_TASK_END);
			}
		}

		curr = task->next;
	}
}

static void __request_io(void)
{
	unsigned long long curr_time = __get_wallclock();
	uint32_t curr = task_table.comp_list[task_table.copy_to_slm_list_id].head;
	uint32_t token_expired = true;

	while (curr != -1) {
		struct ccsd_task_info *task = &(task_table.task[curr]);

			if (atomic_read(&task->task_step) == CCSD_TASK_SCHEDULE) {
	#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
				if (task->slm_lba_info != NULL && ((struct slm_lba_info *)task->slm_lba_info)->random_access == true) {
					task->io_quota = 1;
				} else if (task->slm_lba_info == NULL) {
					// The task has been freed
					CSD_DEBUG_MAGIC_READ("__request_io: slm_lba_info=NULL (task_id=%u)\n", curr);
					task->io_quota = 0;
					curr = task->next;
					continue;
				}
	#endif

				/* Orphan detection: SLM was reallocated under this task */
				if (task->slm_lba_info != NULL) {
					size_t page_idx = get_slm_offset(task->input_buf_addr) / SLM_PAGE_SIZE;
					if (slm_data_ready_info[page_idx].slm_lba_info != task->slm_lba_info) {
						CSD_DEBUG_MAGIC_READ("ORPHAN detected: task_id=%u, page_idx=%lu, expected=%px, got=%px, total_size=%lu->%lu\n",
							curr, page_idx, task->slm_lba_info, slm_data_ready_info[page_idx].slm_lba_info,
							task->total_size, task->requested_io_offset);
						task->io_quota = 0;
						curr = task->next;
						continue;
					}
				}

				if (task->io_quota > 0) {
					uint32_t quota = task->io_quota;
	#if (SUPPORT_ASYNC_COMMAND == 1)
					if (task->requested_io_offset > 0) {
						struct slm_lba_info *lba_info = (struct slm_lba_info *)task->slm_lba_info;
						bool is_circular = (lba_info != NULL && lba_info->is_circular);

						bool compute_caught_up;
						size_t request_threshold_bytes = IO_REQUEST_SIZE;

						if (is_circular) {
							/* For circular buffers, use page-based comparison.
								compute_logical_offset is only updated on page crossings, so compare pages
								to be fair with non-circular case which also uses page granularity. */
							size_t io_page = task->requested_io_offset / SLM_PAGE_SIZE;
							size_t compute_page =
#if (USE_HEAD_TAIL_DEP)
								/* head/tail ring: ht_tail is the consumed frontier */
								lba_info->use_head_tail ? lba_info->ht_tail / SLM_PAGE_SIZE :
#endif
								lba_info->compute_logical_offset / SLM_PAGE_SIZE;
							size_t request_threshold = IO_REQUEST_SIZE / SLM_PAGE_SIZE;
							/* Add 1 page slack since compute_logical_offset lags by up to 1 page */
							compute_caught_up = (io_page <= compute_page + request_threshold + 1);
						} else {
							/* Non-circular: use physical page comparison */
							size_t physical_offset = task->requested_io_offset;
							size_t buf_size = task->total_size;
							size_t current_slm_page = get_slm_offset(task->input_buf_addr + physical_offset) / SLM_PAGE_SIZE;
							size_t latest_slm_page = get_latest_compute_ready(task->input_buf_addr, buf_size);
							size_t request_threshold = IO_REQUEST_SIZE / SLM_PAGE_SIZE;
							compute_caught_up = (latest_slm_page + request_threshold >= current_slm_page);
						}

						if (compute_caught_up &&
							lba_info != NULL &&
							lba_info->stream_access == true) {
							quota = 1;
						} else {
							if (lba_info != NULL && lba_info->random_access == true) {
								quota = 1;
							} else {
								quota = 0;
							}
						}
					}
	#endif

					uint32_t i = 0;
					for (i = 0; i < quota; i++) {
						uint32_t io_req_id = io_req_table.free_list.head;
						struct csd_internal_io_req *io_req;
						size_t io_size = IO_REQUEST_SIZE;
						size_t io_offset = task->requested_io_offset;
						uint64_t lba = 0;
						struct nvmev_result ret;
						size_t demand_read_offset = -1;
						size_t total_issued_size = 0;
						size_t target_total_size = 0;
						size_t remaining = task->total_size - task->requested_io_offset;

						token_expired = false;

	#if (SUPPORT_ASYNC_COMMAND == 1)
						demand_read_offset = slm_get_demand_read_offset(task->input_buf_addr);
						if (task->requested_io_offset == 0) {
							io_size = SLM_PAGE_SIZE;
						}
	#endif

						if (task->total_size < task->requested_io_offset) {
							remaining = 0;
						}

						if (remaining == 0 && demand_read_offset == -1) {
							task->io_quota = 0;
							break;
						}

						if (io_req_id == -1) {
							// No free task
							NVMEV_DEBUG("No free I/O request available 1");
							break;
						}

						if (io_size > remaining) {
							io_size = remaining;
						}

						if (io_size < MIN_IO_REQUEST_SIZE) {
							io_size = MIN_IO_REQUEST_SIZE;
						}

	#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
						if (demand_read_offset != -1) {
							NVMEV_DEBUG(
								"Enqueue_I/O offset : (Cur:%lu, Demand:%lu, io_offset:%lu) (from task_id:%u, io_req_id:%u)",
								task->requested_io_offset, demand_read_offset, io_offset, curr, io_req_id);
							// if (task->requested_io_offset >= demand_read_offset) {
							// 	demand_read_offset = -1; // do not demand request
							// 	notify_slm_demand_read_requested(task->input_buf_addr);
							// 	slm_detect_sequential(task->input_buf_addr);
							// } else {
							io_offset = demand_read_offset;
							{
								struct slm_lba_info *lba_info_snap = READ_ONCE(task->slm_lba_info);
								if (lba_info_snap == NULL) {
									NVMEV_ERROR("[CSD_BUG] __request_io: slm_lba_info NULL in demand read path (task_id:%u, task_step:%d)\n", curr, atomic_read(&task->task_step));
									BUG();
								}
								target_total_size = lba_info_snap->demand_read_size;
							}
							io_size = target_total_size;
							// }
						} else {
							struct slm_lba_info *lba_info = task->slm_lba_info;

							if (lba_info != NULL) {
								if (lba_info->random_access == true) {
									break;
								}
								// else if (task->requested_io_offset != 0) {
								// 	break;
								// }
							}
							else {
								// The task has been freed
								break;
							}
						}
	#endif
						/* Set target_total_size for normal reads if not set by demand read */
						if (target_total_size == 0) {
							target_total_size = io_size;
						}
						/* Prevent do-while loop from creating IOs past the file end */
						if (demand_read_offset == (size_t)-1 && target_total_size > remaining) {
							target_total_size = remaining;
						}

						/* Issue multiple I/O requests to cover the full target_total_size */
						do {
							struct slm_lba_info *lba_info_snap = READ_ONCE(task->slm_lba_info);
							if (lba_info_snap == NULL) {
								NVMEV_ERROR("[CSD_BUG] __request_io: slm_lba_info NULL in IO loop (task_id:%u, task_step:%d, io_offset:%lu)\n", curr, atomic_read(&task->task_step), io_offset);
								BUG();
							}
							lba = __calculate_lba(lba_info_snap, io_offset);
							io_size = __calculate_io_size(lba_info_snap, io_offset, io_size);
							/* For circular buffers, limit IO size to not cross buffer boundary */
							io_size = __calculate_circular_io_size(lba_info_snap, io_offset, io_size);

							/* Past SRE boundary — stop before allocating an io_req */
							if (io_size == 0) {
								// This shouldn't happen for magic demand read
								// printk("Zero io_size calculated, likely due to SRE boundary. io_offset:%lu, requested_io_offset:%lu, demand_read_offset:%lu\n",
								// 	   io_offset, task->requested_io_offset, demand_read_offset);
								break;
							}

							ret = csd_get_target_latency(lba, io_size, curr_time);
							if (ret.nsecs_target == -1) {
								break;
							}

							io_req = &(io_req_table.io_req[io_req_id]);
							BUG_ON(io_req->io_req_status != CSD_INTERNAL_IO_REQ_FREE);
							io_req->io_req_status = CSD_INTERNAL_IO_REQ_ALLOC;

							io_req_table.free_list.head = io_req->next;
							if (io_req->next != -1) {
								io_req_table.io_req[io_req->next].prev = -1;
							}

							io_req->task_id = task->generation_id;
							/* Modify SLM buffer according to the extent map */
							{
								struct slm_lba_info *lba_info = task->slm_lba_info;
								if (demand_read_offset != -1 && lba_info != NULL && lba_info->extent_mapped) {
									io_req->buf_addr = lba_info->demand_read_slm_target + total_issued_size;
									CSD_DEBUG_MAGIC_READ("Demand Read I/O: slm_start_page=%lu, slm_end_page=%lu, io_offset: %lu, io_size: %lu, target_total_size: %lu (from task_id:%u, io_req_id:%u)\n",
											get_slm_offset(io_req->buf_addr) >> SLM_PAGE_SHIFT, get_slm_offset(io_req->buf_addr + io_size - 1) >> SLM_PAGE_SHIFT,
										io_offset, io_size, target_total_size, curr, io_req_id);
								} else {
									io_req->buf_addr = __calculate_circular_buf_addr(lba_info, task->input_buf_addr, io_offset);
								}
							}
							io_req->local_offset = __calculate_local_offset(task->slm_lba_info, io_offset);
							io_req->lba = lba;
							io_req->length = io_size;
							/* Logical stream offset of this chunk (head/tail rings:
							 * buf_addr alone is ambiguous after a wrap) */
							io_req->stream_offset = io_offset;

							/* Head/tail rings need no per-page state reset on wrap:
							 * readiness is a monotonic logical compare. */

							io_req->prev = -1;
							io_req->next = -1;
							io_req->nsecs_nand_start = ret.nsecs_nand_start;
							io_req->nsecs_target = ret.nsecs_target;

							task->internal_io_count++;
							io_req_table.enqueuing_io_size = io_req_table.enqueuing_io_size + io_req->length;

							if (demand_read_offset != -1) {
	#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
								io_req->demand_read = true;
								if (total_issued_size == 0) {
									/* Notify only once for the first chunk */
									/* The magic read buffer is way much smaller than the total file size */
									struct slm_lba_info *lba_info = task->slm_lba_info;
									if (lba_info != NULL && lba_info->extent_mapped)
										notify_slm_demand_read_requested(task->input_buf_addr);
									else
										notify_slm_demand_read_requested(task->input_buf_addr + demand_read_offset);
								}
								total_issued_size += io_size;
	#else
								BUG();
	#endif
							} else {
								io_req->demand_read = false;
								task->requested_io_offset = task->requested_io_offset + io_size;
								total_issued_size += io_size;
							}

							{
								io_req->sequential_last_io = false;
								struct slm_lba_info *lba_info = task->slm_lba_info;
								bool is_circular = (lba_info != NULL && lba_info->is_circular);
								size_t logical_total = is_circular ? lba_info->logical_total_size : task->total_size;
								if (task->requested_io_offset + io_size >= logical_total) {
									io_req->sequential_last_io = true;
									// printk("SEQUENTIAL LAST I/O for task %u io_req %u\n", curr, io_req_id);
								}
							}

							io_req->last_io = false;
							if (total_issued_size >= target_total_size) {
								io_req->last_io = true;
							} else {
								/* Peek ahead: if next iteration would get zero io_size,
									this is the last valid I/O — set last_io now before enqueueing */
								size_t next_offset = io_offset + io_size;
								size_t next_remaining = target_total_size - total_issued_size;
								if (__calculate_io_size(lba_info_snap, next_offset, next_remaining) == 0) {
									io_req->last_io = true;
								}
							}

							__enqueue_slm_work(io_req_id);
							io_req->io_req_status = CSD_INTERNAL_IO_REQ_READY;

							NVMEV_DEBUG(
								"TASK_%d_%d (total_size:%lu, requested_offset:%lu, remaining:%lu, target:%llu, cur_time:%llu, diff:%llu, io_size:%lu)",
								curr, io_req_id, task->total_size, task->requested_io_offset, remaining, io_req->nsecs_target,
								curr_time, io_req->nsecs_target - task->nsecs_target, io_size);

							if (io_size == 0) {
								// this can happen if file is smaller than IO_REQUEST_SIZE
								break;
							}

							/* Continue issuing I/O until full target_total_size is covered */
							if (total_issued_size < target_total_size) {
								io_offset += io_size;
								io_size = target_total_size - total_issued_size;
								io_req_id = io_req_table.free_list.head;
								if (io_req_id == -1) {
									/* No more free I/O requests, exit loop */
									NVMEV_DEBUG("No free I/O request available 2");
									break;
								}
							} else {
								break;
							}
						} while (1);

						task->io_quota = task->io_quota - 1;
					}
				}
			}
			curr = task->next;
	}

	if (token_expired == true) {
		curr = task_table.comp_list[task_table.copy_to_slm_list_id].head;
		while (curr != -1) {
			struct ccsd_task_info *task = &(task_table.task[curr]);

			if (atomic_read(&task->task_step) == CCSD_TASK_SCHEDULE) {
				size_t remaining = task->total_size - task->requested_io_offset;

				if (remaining > 0) {
					if (task->is_first_io == false) {
						task->io_quota = IO_TOKEN_PER_TASK;
#if (CSD_IO_SCHEDULING_TYPE != CSD_SCHEDULING_TYPE_RR)
						break;
#endif
					}
				}
			}

			curr = task->next;
		}
	}
}

static void __reclaim_task(void)
{
	uint32_t comp_list_count = task_table.num_cpu_resources + 1; // For copy to slm list
	int io_count = 0;
	// Compute Queue

	for (uint32_t turn = 0; turn < comp_list_count; turn++) {
		struct ccsd_list *comp_list = &(task_table.comp_list[turn]);
		uint32_t first_entry = comp_list->head;
		uint32_t curr = first_entry;
		uint32_t last_entry = -1;
		uint32_t count = 0;

		curr = first_entry;
		while (curr != -1) {
			struct ccsd_task_info *task = &(task_table.task[curr]);

			if (atomic_read(&task->task_step) == CCSD_TASK_END) {
				if (last_entry == -1) {
					first_entry = curr;
				}
				last_entry = curr;
				count++;
				NVMEV_DEBUG("[COMPLETION_ALERT] found COM completed %d - %llu", last_entry, __get_wallclock());
				// if (task->program_idx == 0) {
				// 	printk("Task terminated slm load task ID : %lu\n", curr);
				// }
			} else if (last_entry != -1) {
				break;
			}
			curr = task->next;
		}

		if (last_entry != -1) {
			__dequeue_task_multi(comp_list, first_entry, last_entry, count);
			__enqueue_task_multi(&task_table.done_list, first_entry, last_entry, count);
		}
	}
}

static void __process_deferred_slm_frees(void)
{
	int i, j;
	for (i = 0; i < MAX_DEFERRED_SLM_FREES; i++) {
		if (!deferred_slm_frees[i].active)
			continue;

		bool all_done = true;
		for (j = 0; j < deferred_slm_frees[i].num_load_tasks; j++) {
			uint32_t tid = deferred_slm_frees[i].load_task_ids[j];
			uint32_t tid_pool = TASK_POOL_INDEX(tid);
			struct ccsd_task_info *tid_task = &task_table.task[tid_pool];
			/* If generation doesn't match, the load task was already freed+recycled → done */
			if (!TASK_GEN_VALID(tid, tid_task))
				continue;
			if (READ_ONCE(tid_task->internal_io_count) != 0) {
				all_done = false;
				break;
			}
		}

		if (all_done) {
			free_slm_range(deferred_slm_frees[i].input_buf);
			deferred_slm_frees[i].active = false;
		}
	}
}

static int nvmev_kthread_ccsd_io(void *data)
{
	struct nvmev_proc_info *pi = (struct nvmev_proc_info *)data;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif
	unsigned long long prev_clock;
	prev_clock = __get_wallclock();

	NVMEV_INFO("%s started on cpu %d (node %d)", pi->thread_name, smp_processor_id(), cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = pi->io_seq;
		int qidx;

		// Create Task
		// Set a dispatch limit to avoid task run out
		int dispatch_count = 0;
		int dispatch_limit = task_table.num_cpu_resources;
		while (curr != -1) {
			struct nvmev_proc_table *pe = &pi->proc_table[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			pi->proc_io_nsecs = curr_nsecs;

			if (pe->is_completed == true) {
				curr = pe->next;
				continue;
			}

			if (dispatch_count >= dispatch_limit)
				break;

			if (pe->is_copied == false) {
				bool is_success = true;
				int sqid = pe->sqid;
				int sq_entry = pe->sq_entry;
				struct nvmev_submission_queue *sq = vdev->sqes[sqid];
				struct nvme_command *cmd = (struct nvme_command *)(&sq_entry(sq_entry));
				int opcode = cmd->common.opcode;
				bool send_cq = false;
				size_t result = 0;

				// Set Status Code to success (this could be omitted)
				pe->status = (NVME_SCT_GENERIC_CMD_STATUS << 8) | NVME_SC_SUCCESS;

#ifdef PERF_DEBUG
				unsigned long long memcpy_time;
				pe->nsecs_copy_start = local_clock() + delta;
#endif
				if (opcode == nvme_admin_load_program) {
					is_success = __do_perform_program_management(sqid, sq_entry, &(pe->status));
					send_cq = true;
				} else if (opcode == nvme_cmd_memory_management) {
					is_success = __do_perform_memory_management(sqid, sq_entry, &(pe->status), &result);
					send_cq = true;

				} else if (opcode == nvme_cmd_magic_compaction) {
					is_success = __do_perform_magic_compaction(sqid, sq_entry, curr, &(pe->status), &result);
					send_cq = true;
					result = -1;

				} else if (opcode == nvme_cmd_magic_read) {
					is_success = __do_perform_magic_read(sqid, sq_entry, curr, &(pe->status), &result);
					send_cq = true;
					result = -1;

				} else {
					is_success = __do_perform_dispatch(sqid, sq_entry, curr, &(pe->status));
#if (SUPPORT_ASYNC_MEM_COPY == 1)
					if (opcode == nvme_cmd_memory_copy) {
						send_cq = true;
					}
#endif

#if (SUPPORT_ASYNC_COMPUTE == 1)
					if (opcode == nvme_cmd_execute_program) {
						struct nvme_command_csd *cs_cmd = (struct nvme_command_csd *)cmd;
						send_cq = true;
						result = -1;

						if (cs_cmd->execute_program.pind == COMPRESSION_INDEX) {
							send_cq = false;
						}
					}
#endif
				}

				pe->is_copied = true;
				dispatch_count++;
				NVMEV_DEBUG("%s: copied %u, %d %d %d", pi->thread_name, curr, pe->sqid, pe->cqid, pe->sq_entry);

				// If failed to process a command, just send completion right away
				if (is_success == false) {
					send_cq = true;
				}

				if (send_cq) {
					__fill_cq_result(pe->sqid, pe->cqid, pe->sq_entry, pe->command_id, pe->status, result);
					mb(); /* Reclaimer shall see after here */
					pe->is_completed = true;
				}

#ifdef PERF_DEBUG
				pe->nsecs_copy_done = local_clock() + delta;
				memcpy_time = pe->nsecs_copy_done - pe->nsecs_copy_start;
#endif
			}
			__process_deferred_slm_frees();
			__reclaim_task();
			curr = pe->next;
		}
		__reclaim_task();

		// Create CQ
		curr = task_table.done_list.head;
		while (curr != -1) {
			int task_id = curr;
			struct ccsd_task_info *task = &(task_table.task[task_id]);
			struct nvmev_proc_table *pe = &(pi->proc_table[task->proc_idx]);
			bool send_cq = true;

#if (SUPPORT_ASYNC_MEM_COPY == 1)
			if (task->opcode == nvme_cmd_memory_copy) {
				send_cq = false;
			}
#endif
#if (SUPPORT_ASYNC_COMPUTE == 1)
			if (task->opcode == nvme_cmd_execute_program) {
				send_cq = false;

				if (task->program_idx == COMPRESSION_INDEX) {
					send_cq = true;
				}
			}
			if (task->opcode == nvme_cmd_magic_compaction) {
				send_cq = false;
			}
			if (task->program_idx == ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX) {
				// Magic command already sent completion during dispatch
				send_cq = false;

				// Free input SLM buffers for magic command
				struct magic_params *mp = (struct magic_params *)task->params;
				free_slm_range(mp->compaction.first_level_start);
				free_slm_range(mp->compaction.second_level_start);
				CSD_DEBUG_MAGIC_COMPACTION("Freed input SLM buffers: first=%lu, second=%lu\n",
					   mp->compaction.first_level_start, mp->compaction.second_level_start);
			}
			if (task->program_idx == ROCKSDB_MAGIC_CRC_PROGRAM_INDEX) {
				// CRC task completion - free intermediate buffer
				send_cq = false;

				struct rocksdb_magic_crc_params *crc_params =
					(struct rocksdb_magic_crc_params *)task->params;
				free_slm_range(crc_params->input_buf);
				CSD_DEBUG_MAGIC_COMPACTION("Freed intermediate SLM buffer: %lu\n",
					   crc_params->input_buf);
			}
			if (task->opcode == nvme_cmd_magic_read) {
				send_cq = false;
			}
			if (task->program_idx == ROCKSDB_MAGIC_READ_PROGRAM_INDEX) {
				// Magic read already sent completion during dispatch
				send_cq = false;

				// Defer input SLM buffer free until load tasks' IOs complete
				struct magic_params *mp = (struct magic_params *)task->params;

				uint32_t wait_task_ids[MAX_MAGIC_READ_FILES];
				int num_wait_tasks = 0;

				// The "untouched" slm loads could be in "SCHEDULE" set these to "WAIT" to prevent them from being scheduled and issuing new IOs
				for (int j = 0; j < mp->read.num_load_tasks; j++) {
					uint32_t lt_pool = TASK_POOL_INDEX(mp->read.load_task_ids[j]);
					struct ccsd_task_info *lt = &task_table.task[lt_pool];

					// Skip if load task was already freed and slot reused (stale generation)
					if (!TASK_GEN_VALID(mp->read.load_task_ids[j], lt))
						continue;

					// Only move the ones in the schedule mode, leave out the terminated ones
					if (atomic_read(&lt->task_step) == CCSD_TASK_SCHEDULE) {
						if (lt->program_idx != COPY_TO_SLM_PROGRAM_INDEX) {
							NVMEV_ERROR("Unexpected program index for load task (task_id:%u, program_idx:%d)\n",
										mp->read.load_task_ids[j], lt->program_idx);
							BUG();
						}
						if (atomic_cmpxchg(&lt->task_step, CCSD_TASK_SCHEDULE, CCSD_TASK_WAIT) == CCSD_TASK_SCHEDULE)
							wait_task_ids[num_wait_tasks++] = mp->read.load_task_ids[j];
					}
				}

				if (num_wait_tasks > 0) {
					// Add only WAIT tasks to deferred free list
					int slot;
					for (slot = 0; slot < MAX_DEFERRED_SLM_FREES; slot++) {
						if (!deferred_slm_frees[slot].active)
							break;
					}
					if (slot < MAX_DEFERRED_SLM_FREES) {
						deferred_slm_frees[slot].input_buf = mp->read.input_buf;
						deferred_slm_frees[slot].num_load_tasks = num_wait_tasks;
						for (int j = 0; j < num_wait_tasks; j++)
							deferred_slm_frees[slot].load_task_ids[j] = wait_task_ids[j];
						smp_wmb();
						deferred_slm_frees[slot].active = true;
					} else {
						NVMEV_ERROR("DEFERRED_FREE: no slot, freeing immediately\n");
						BUG();
					}
				} else {
					// No pending load IOs, safe to free immediately
					free_slm_range(mp->read.input_buf);
				}
			}
#endif
			if (send_cq == true) {
				// Do nothing
				__fill_cq_result(pe->sqid, pe->cqid, pe->sq_entry, pe->command_id, task->status, task->result);

				NVMEV_DEBUG("%s: completed %u, %d %d %d", pi->thread_name, curr, pe->sqid, pe->cqid, pe->sq_entry);
#ifdef PERF_DEBUG
				pe->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu", pe->nsecs_start, pe->nsecs_enqueue - pe->nsecs_start,
							 pe->nsecs_copy_start - pe->nsecs_start, pe->nsecs_copy_done - pe->nsecs_start,
							 pe->nsecs_cq_filled - pe->nsecs_start, pe->nsecs_target - pe->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */

				pe->is_completed = true;
			}

			task->generation_id = TASK_ID_NONE;
			atomic_set(&task->task_step, CCSD_TASK_FREE);

			NVMEV_DEBUG("[CCSD] task_%d_Complete (result:%d, output:%u)", curr, task->result, output_id);

			/* Defer slm_lba_info freeing to free_slm_range()/free_slm_lba_info(),
			 * which frees both the sre and the info and clears the page->info
			 * back-pointers when the host frees the SLM range. Host-managed (sync)
			 * mode must NOT free here: the back-pointer installed by
			 * set_slm_ready_for_csd has to stay valid for the consuming command
			 * (e.g. compaction reads the loaded input, CRC reads the compaction
			 * output in place) which only runs after this producer task completes.
			 * Freeing per-task would leave a dangling back-pointer. */
			task->slm_lba_info = NULL;

			uint32_t next_task = task->next; // Store next task before dequeuing

			__dequeue_task(&task_table.done_list, task_id);
			__enqueue_task(&task_table.free_list, task_id);

			curr = next_task;
		}
		__process_deferred_slm_frees();
		cond_resched();
	}

	return 0;
}

static int dispatcher_helper(void *data)
{
	struct nvmev_proc_info *pi = (struct nvmev_proc_info *)data;
	int pid = smp_processor_id();

	NVMEV_INFO("dispatcher_helper started on cpu %d (node %d)", pid, cpu_to_node(pid));

	while (!kthread_should_stop()) {
		__request_io();
		__reclaim_io_req();

		cond_resched();
	}

	return 0;
}

static int compute_work(void *data)
{
	struct ccsd_list *comp_list = (struct ccsd_list *)data;
	int pid = smp_processor_id();

	NVMEV_INFO("compute_work started on cpu %d (node %d)", pid, cpu_to_node(pid));
	while (!kthread_should_stop()) {
		int task_id = comp_list->head;
		while (task_id != -1) {
			struct ccsd_task_info *task = &(task_table.task[task_id]);

			if (atomic_read(&task->task_step) == CCSD_TASK_SCHEDULE) {
				size_t processed_offset = task->processed_offset; // Already processed
				size_t remaining = task->total_size - processed_offset;
				size_t result = 0;

				if (task->program_idx == COPY_TO_SLM_PROGRAM_INDEX) {
					NVMEV_ERROR("WRONG PROGRAM IS COMING! (task_id:%d, program_idx:%d)", task_id, task->program_idx);
					BUG();
				}

				while (remaining > 0) {
					int finish = 0;
					size_t processing_size = remaining; // Will process this time
					size_t temp = 0;
					size_t input_addr = task->input_buf_addr + processed_offset;
					size_t output_addr = task->output_buf_addr;

#if (SUPPORT_ASYNC_COMMAND == 1)
					if (task->program_idx == DECOMPRESSION_INDEX) {
						processing_size = remaining;
					} else if (IS_STREAM_TYPE_PROGRAM_INDEX(task->program_idx) == true &&
							   IS_AUTO_OUTPUT_RECORD_PROGRAM_INDEX(task->program_idx) == false) {
						if (processing_size > IO_REQUEST_SIZE) {
							processing_size = IO_REQUEST_SIZE;
						}
					}
#endif
					if (IS_STATIC_OUTPUT_TYPE_PROGRAM_INDEX(task->program_idx) == false) {
						output_addr = task->output_buf_addr + task->result + result;
					} else {
#if (SUPPORT_ASYNC_COMMAND == 1)
						/* No need to split static output type programs since
							their output readiness is not published incrementally */
						if (IS_STREAM_TYPE_PROGRAM_INDEX(task->program_idx) == true &&
							IS_AUTO_OUTPUT_RECORD_PROGRAM_INDEX(task->program_idx) == false) {
							size_t temp_size = 1024 * 1024 * 4;
							size_t test_size = (remaining > temp_size) ? temp_size : remaining;
							processing_size = get_continuous_size(input_addr, test_size);
							if (processing_size == 0) {
								//printk("Any data is available");
								break;
							}
							// processing_size = ALIGNED_DOWN(processing_size, IO_REQUEST_SIZE);

							if (processing_size > remaining) {
								processing_size = remaining;
							}
						}
#endif
					}
					NVMEV_CSD_PROFILE_START("COMPUTE", pid, task->host_id, 0);

#if (SUPPORT_ASYNC_COMMAND == 1)
					/* Add profiling info for DCSD real compute time profiling */
					if (task->program_idx != ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX &&
					    task->program_idx != ROCKSDB_MAGIC_CRC_PROGRAM_INDEX &&
					    task->program_idx != ROCKSDB_MAGIC_READ_PROGRAM_INDEX) {
						((struct CSD_PARAMS *)task->params)->profile_info.pid = pid;
						((struct CSD_PARAMS *)task->params)->profile_info.host_id = task->host_id;
					}
#endif
					temp = __csdvirt_run_program(task->program_idx, (void *)input_addr, (void *)output_addr,
												 processing_size, task->params);

					finish = ((struct CSD_PARAMS *)task->params)->finish;
					if (finish > 0) {
						NVMEV_CSD_INFO("COM", "finish: %d %lu", finish, remaining);
						processing_size = remaining;
					}

					processed_offset = processed_offset + processing_size;
					remaining = remaining - processing_size;

					if (IS_STATIC_OUTPUT_TYPE_PROGRAM_INDEX(task->program_idx) == true) {
						result = temp;
					} else {
						result = result + temp;
					}

					if (IS_RANDOM_ACCESS_PROGRAM_INDEX(task->program_idx) == true) {
						if (processed_offset != task->total_size) {
							NVMEV_ERROR("Compute size error: processed_size : %lu, total_size: %lu", processed_offset,
										task->total_size);
							BUG();
						}
						if (remaining != 0) {
							NVMEV_ERROR("Compute size error: remaining : %lu", remaining);
							BUG();
						}
					}

					NVMEV_DEBUG("processed_offset:%lu, processing_size:%lu, remaining:%lu, result:%lu",
								processed_offset, processing_size, remaining, result);
					NVMEV_CSD_PROFILE_END("COMPUTE", pid, task->host_id, 0);

#if (SUPPORT_ASYNC_COMPUTE == 1)
					if (result >= SLM_PAGE_SIZE) {
						// To notify that slm data is ready
						break;
					}
#endif
				}

				task->processed_offset = processed_offset;

#if (SUPPORT_ASYNC_COMPUTE == 1)
				/* Not needed for computations using set_data_from_ptr.
				   However, leaving this code line for computations that set_data_from_ptr
					if not yet applied */
				if ((IS_AUTO_OUTPUT_RECORD_PROGRAM_INDEX(task->program_idx) == false) &&
							(IS_MULTI_STREAM_PROGRAM_INDEX(task->program_idx) == false)) {
					notify_slm_data_ready(task->output_buf_addr + task->result, result);
				}
#endif
				if (IS_STATIC_OUTPUT_TYPE_PROGRAM_INDEX(task->program_idx) == true) {
					task->result = result;
				} else {
					task->result += result; // TODO Not using in multicore system
				}

				if (remaining == 0) {
#if (SUPPORT_ASYNC_COMPUTE == 1)
					int i = 0;
					size_t remain = task->total_size - task->result;
					uint64_t buf = task->output_buf_addr + task->result;

					// printk("remain: %lu, result: %lu, total_size: %lu", remain, task->result, task->total_size);
					// printk("[%d] Output SLM ready: %llu\n", task->host_id, get_slm_offset(task->output_buf_addr) / SLM_PAGE_SIZE);
					// Don't do this for magic commands
					if (task->program_idx != ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX &&
						task->program_idx != ROCKSDB_MAGIC_CRC_PROGRAM_INDEX &&
						task->program_idx != ROCKSDB_MAGIC_READ_PROGRAM_INDEX) {
						finalize_slm_data_ready(task->output_buf_addr + task->result, task->result);
						notify_slm_data_ready(task->output_buf_addr + task->result, SLM_PAGE_SIZE);
					}
#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 0)
					debug_slm_memory(task->output_buf_addr, task->total_size);
#else
					debug_slm_memory(task->output_buf_addr, task->total_size + SLM_PAGE_SIZE);
#endif
#endif
					NVMEV_CSD_INFO("COM", "TASK_END - TaskID:%u(ProgramIDX:%u), result: %lu, intput_offset:%lu",
								   task_id, task->program_idx, task->result,
								   get_slm_offset(task->input_buf_addr) / SLM_PAGE_SIZE);
					atomic_set(&task->task_step, CCSD_TASK_END);
				}
			}
			task_id = task->next;
		}
		cond_resched();
	}
	return 0;
}

static int slm_work(void *data)
{
	struct ccsd_list *slm_list = (struct ccsd_list *)data;
	int pid = smp_processor_id();
	int nsid = 0; // TODO
	unsigned long long curr_nsecs_wall = __get_wallclock();
	unsigned long long curr_nsecs_local = local_clock();
	long long delta = curr_nsecs_wall - curr_nsecs_local;

	NVMEV_INFO("slm_work started on cpu %d (node %d)", pid, cpu_to_node(smp_processor_id()));
	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs = local_clock() + delta;
		unsigned long long min_target_time = -1;
		int io_req_id = slm_list->head;
		struct csd_internal_io_req *io_req;

		// Copy data
		io_req_id = slm_list->head;
		while (io_req_id != -1) {
			io_req = &(io_req_table.io_req[io_req_id]);

			if (io_req->io_req_status == CSD_INTERNAL_IO_REQ_READY) {
				struct ccsd_task_info *task = &(task_table.task[TASK_POOL_INDEX(io_req->task_id)]);

				/* Detect orphaned I/O: page was reallocated under us */
				{
					size_t page_idx = get_slm_offset(io_req->buf_addr) / SLM_PAGE_SIZE;
					struct slm_lba_info *page_info = slm_data_ready_info[page_idx].slm_lba_info;
					if (page_info == NULL || page_info != task->slm_lba_info) {
						NVMEV_DEBUG("ORPHAN IO: task_%d, io_req_%d, buf_page=%lu, "
							"task_info=%p, page_info=%p",
							io_req->task_id, io_req_id, page_idx,
							task->slm_lba_info, page_info);
						io_req->io_req_status = CSD_INTERNAL_IO_REQ_DONE;
						io_req_id = io_req->next;
						continue;
					}
				}

				NVMEV_DEBUG("SLM_%d_TASK_%d_%d_MEMCPY_START", pid, io_req->task_id, io_req_id);
				copy_to_slm(io_req->buf_addr, vdev->ns[nsid].mapped + (io_req->lba << 9) + io_req->local_offset, io_req->length);

				io_req->io_req_status = CSD_INTERNAL_IO_REQ_WAITING_TIME;
				NVMEV_DEBUG("SLM_%d_TASK_%d_%d_MEMCPY_DONE", pid, io_req->task_id, io_req_id);

				curr_nsecs = local_clock() + delta;
				// if (curr_nsecs >= io_req->nsecs_target) {
				// 	NVMEV_CSD_INFO("SLM", "SLM_%d_TASK_%d_%d_NO WAIT", pid, io_req->task_id, io_req_id);
				// }
#if 0
				if (min_target_time > io_req->nsecs_target) {
					min_target_time = io_req->nsecs_target;
				}

				if (curr_nsecs >= min_target_time) {
					break;
				}
#endif
				NVMEV_DEBUG("SLM_%d_TASK_%d_%d min: %llu, cur: %llu, diff: %llu", pid, io_req->task_id, io_req_id,
							min_target_time, curr_nsecs, min_target_time - curr_nsecs);
			}
			io_req_id = io_req->next;
		}
		cond_resched();
	}
	return 0;
}

void NVMEV_CSD_PROC_INIT(struct nvmev_dev *vdev)
{
	char name[32];
	unsigned int i, proc_idx;
	int nr_compound_csd_cpu = 1; // only one CCSD scheduler exists

	/* Initialize pre-allocated compaction buffers */
	if (csd_user_func_init() != 0) {
		NVMEV_ERROR("csd_user_func_init fail");
		BUG();
	}

	vdev->csd_proc_info = kcalloc(sizeof(struct nvmev_proc_info), nr_compound_csd_cpu, GFP_KERNEL);
	if (vdev->csd_proc_info == NULL) {
		NVMEV_ERROR("Memory alloc fail");
		BUG();
	}

	/* Initialize CCSD task management slots */
	BUILD_BUG_ON(MAX_TASK_COUNT != (1 << TASK_POOL_BITS));

	task_table.free_list.head = 0;
	task_table.free_list.tail = MAX_TASK_COUNT - 1;
	task_table.free_list.running = MAX_TASK_COUNT;
	task_table.next_generation = 1; /* Start at 1 so gen 0 + slot 0 != 0 */

	task_table.done_list.head = -1;
	task_table.done_list.tail = -1;
	task_table.done_list.running = 0;

	for (i = 0; i < MAX_TASK_COUNT; i++) {
		atomic_set(&task_table.task[i].task_step, CCSD_TASK_FREE);
		task_table.task[i].next = i + 1;
		task_table.task[i].prev = i - 1;
		task_table.task[i].slm_lba_info = NULL;
		task_table.task[i].generation_id = TASK_ID_NONE;
	}
	task_table.task[MAX_TASK_COUNT - 1].next = -1;

	if (vdev->config.nr_csd_cpu < 2 || vdev->config.nr_slm_cpu < 1) {
		NVMEV_ERROR("CPU Count Error: %d %d", vdev->config.nr_csd_cpu, vdev->config.nr_slm_cpu);
	}

	task_table.num_cpu_resources = vdev->config.nr_csd_cpu;
	task_table.num_slm_resources = vdev->config.nr_slm_cpu;
	task_table.compute_turn = 0;
	task_table.copy_to_slm_list_id = task_table.num_cpu_resources;
	task_table.comp_list = kcalloc(sizeof(struct ccsd_list), task_table.num_cpu_resources + 1, GFP_KERNEL);
	if (task_table.comp_list == NULL) {
		NVMEV_ERROR("Memory alloc fail");
		BUG();
	}

	io_req_table.slm_list = kcalloc(sizeof(struct ccsd_list), task_table.num_slm_resources, GFP_KERNEL);
	io_req_table.slm_turn = 0;
	if (io_req_table.slm_list == NULL) {
		NVMEV_ERROR("Memory alloc fail");
		BUG();
	}

	for (i = 0; i < vdev->config.nr_csd_cpu; i++) {
		task_table.comp_list[i].head = -1;
		task_table.comp_list[i].tail = -1;
		task_table.comp_list[i].running = 0;

		snprintf(name, sizeof(name), "ccsd_compute_%d", i);
		csd_compute_workder[i] = kthread_create(compute_work, &(task_table.comp_list[i]), name);
		kthread_bind(csd_compute_workder[i], vdev->config.cpu_nr_csd[i]);
		wake_up_process(csd_compute_workder[i]);
	}
	task_table.comp_list[task_table.copy_to_slm_list_id].head = -1;
	task_table.comp_list[task_table.copy_to_slm_list_id].tail = -1;
	task_table.comp_list[task_table.copy_to_slm_list_id].running = 0;

	for (i = 0; i < vdev->config.nr_slm_cpu; i++) {
		io_req_table.slm_list[i].head = -1;
		io_req_table.slm_list[i].tail = -1;
		io_req_table.slm_list[i].running = 0;

		snprintf(name, sizeof(name), "ccsd_slm_%d", i);
		csd_slm_workder[i] = kthread_create(slm_work, &(io_req_table.slm_list[i]), name);
		kthread_bind(csd_slm_workder[i], vdev->config.cpu_nr_slm[i]);
		wake_up_process(csd_slm_workder[i]);
	}

	io_req_table.enqueuing_io_size = 0;
	io_req_table.free_list.head = 0;
	io_req_table.free_list.tail = MAX_INTERNAL_IO_COUNT - 1;
	for (i = 0; i < MAX_INTERNAL_IO_COUNT; i++) {
		io_req_table.io_req[i].io_req_status = CSD_INTERNAL_IO_REQ_FREE;
		io_req_table.io_req[i].next = i + 1;
		io_req_table.io_req[i].prev = i - 1;
		io_req_table.io_req[i].demand_read = false;
		io_req_table.io_req[i].last_io = false;
	}
	io_req_table.io_req[MAX_INTERNAL_IO_COUNT - 1].next = -1;

	for (proc_idx = 0; proc_idx < nr_compound_csd_cpu; proc_idx++) {
		struct nvmev_proc_info *pi = &vdev->csd_proc_info[proc_idx];

		pi->proc_table = kzalloc_node(sizeof(struct nvmev_proc_table) * NR_MAX_PARALLEL_IO, GFP_KERNEL, 1);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			pi->proc_table[i].next = i + 1;
			pi->proc_table[i].prev = i - 1;
		}
		pi->proc_table[NR_MAX_PARALLEL_IO - 1].next = -1;

		pi->free_seq = 0;
		pi->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		pi->io_seq = -1;
		pi->io_seq_end = -1;

		snprintf(pi->thread_name, sizeof(pi->thread_name), "csd_dispatcher");

		pi->nvmev_io_worker = kthread_create(nvmev_kthread_ccsd_io, pi, pi->thread_name);

		kthread_bind(pi->nvmev_io_worker, vdev->config.cpu_nr_csd_dispatcher);
		wake_up_process(pi->nvmev_io_worker);
		
		csd_dispatcher_helper = kthread_create(dispatcher_helper, pi, "csd_dispatcher_helper");

		kthread_bind(csd_dispatcher_helper, vdev->config.cpu_nr_csd_helper);
		wake_up_process(csd_dispatcher_helper);
	}

	init_slm_memory(vdev->slm_mapped, vdev->config.slm_size);

	vdev->tfm = crypto_alloc_comp("lz4", 0, 0);
	if (IS_ERR_OR_NULL(vdev->tfm)) {
		NVMEV_ERROR("Error allocating LZ4 compressor\n");
	}
}

void NVMEV_CSD_PROC_FINAL(struct nvmev_dev *vdev)
{
	unsigned int i;

	for (i = 0; i < 1; i++) {
		struct nvmev_proc_info *pi = &vdev->csd_proc_info[i];

		if (!IS_ERR_OR_NULL(pi->nvmev_io_worker)) {
			kthread_stop(pi->nvmev_io_worker);
		}
		
		if (!IS_ERR_OR_NULL(csd_dispatcher_helper)) {
			kthread_stop(csd_dispatcher_helper);
		}

		kfree(pi->proc_table);
	}

	for (i = 0; i < vdev->config.nr_csd_cpu; i++) {
		if (!IS_ERR_OR_NULL(csd_compute_workder[i])) {
			kthread_stop(csd_compute_workder[i]);
		}
	}

	for (i = 0; i < vdev->config.nr_slm_cpu; i++) {
		if (!IS_ERR_OR_NULL(csd_slm_workder[i])) {
			kthread_stop(csd_slm_workder[i]);
		}
	}

	kfree(vdev->csd_proc_info);
	kfree(task_table.comp_list);
	kfree(io_req_table.slm_list);

	/* Free pre-allocated compaction buffers */
	csd_user_func_exit();

	final_slm_memory();

	crypto_free_comp(vdev->tfm);
}

bool IS_CSD_PROCESS(int opcode)
{
	bool result = false;
	switch (opcode) {
	case nvme_cmd_memory_management:
	case nvme_cmd_memory_copy:
	case nvme_cmd_execute_program:
	case nvme_admin_load_program:
	case nvme_cmd_magic_compaction:
	case nvme_cmd_magic_read:
		result = true;
		break;
	default:
		break;
	}

	return result;
}
