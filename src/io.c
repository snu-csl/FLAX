/**********************************************************************
 * Copyright (c) 2020-2023
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
#include <linux/slab.h>

#include "nvmev.h"
#include "dma.h"

#if ((BASE_SSD == SAMSUNG_970PRO) || (BASE_SSD) == ZNS_PROTOTYPE)
#include "ssd.h"
#else
struct buffer;
#endif

#if (CSD_ENABLE == 1)
#include "nvme_csd.h"
#include "csd_slm.h"
#endif

#undef PERF_DEBUG

#define PRP_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern struct nvmev_dev *vdev;

extern bool io_using_dma;

static struct kmem_cache *nvmev_deferred_io_cache;

int nvmev_init_deferred_cache(void)
{
	if (nvmev_deferred_io_cache)
		return 0;

	nvmev_deferred_io_cache = kmem_cache_create("nvmev_deferred_io",
						    sizeof(struct nvmev_deferred_io),
						    0, SLAB_HWCACHE_ALIGN, NULL);
	if (!nvmev_deferred_io_cache)
		return -ENOMEM;

	return 0;
}

void nvmev_destroy_deferred_cache(void)
{
	if (!nvmev_deferred_io_cache)
		return;

	kmem_cache_destroy(nvmev_deferred_io_cache);
	nvmev_deferred_io_cache = NULL;
}

int nvmev_init_dq(struct nvmev_submission_queue *sq)
{
	INIT_LIST_HEAD(&sq->deferred_list);
	sq->deferred_count = 0;
	return 0;
}

void nvmev_destroy_dq(struct nvmev_submission_queue *sq)
{
	struct nvmev_deferred_io *node, *tmp;

	if (!sq)
		return;

	list_for_each_entry_safe(node, tmp, &sq->deferred_list, list) {
		list_del(&node->list);
		if (nvmev_deferred_io_cache)
			kmem_cache_free(nvmev_deferred_io_cache, node);
		else
			kfree(node);
	}

	sq->deferred_count = 0;
}

static void nvmev_enqueue_deferred(struct nvmev_submission_queue *sq, int sq_entry,
				   unsigned int sq_head, u64 nsecs_start)
{
	struct nvmev_deferred_io *node;

	if (!sq)
		return;

	if (unlikely(!nvmev_deferred_io_cache))
		return;

	node = kmem_cache_alloc(nvmev_deferred_io_cache, GFP_KERNEL);
	if (!node) {
		NVMEV_ERROR("Failed to allocate deferred IO node (qid %d entry %d)\n",
			    sq->qid, sq_entry);
		return;
	}

	node->sq_entry = sq_entry;
	node->sq_head = sq_head;
	memcpy(&node->cmd, &sq_entry(sq_entry), sizeof(struct nvme_command));
	node->nsecs_start = nsecs_start;
	INIT_LIST_HEAD(&node->list);

	list_add_tail(&node->list, &sq->deferred_list);
	sq->deferred_count++;
}

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

static unsigned int __do_perform_io(int sqid, struct nvme_command *cmd_base)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	u64 prp1, prp2;

	struct nvme_rw_command *rw_cmd = &cmd_base->rw;
	size_t nsid = rw_cmd->nsid - 1; // 0-based
	int opcode = rw_cmd->opcode;

#if (CSD_ENABLE == 1)
	size_t slm_addr = 0;
	bool is_adjusted = false;
	if ((opcode == nvme_cmd_memory_read) || (opcode == nvme_cmd_memory_write)) {
		struct nvme_command_csd *cmd = (struct nvme_command_csd *)(cmd_base);
		NVMEV_CSD_PROFILE_START("SLM", 0, cmd->memory.cdw14, 0);
		offset = 0;
		slm_addr = get_slm_addr(cmd->memory.sb);
		length = cmd->memory.length;
		prp1 = cmd->memory.dptr.prp1;
		prp2 = cmd->memory.dptr.prp2;
		is_adjusted = (cmd->memory.cdw13 == 1) ? true : false;
	} else if (opcode == nvme_cmd_namespace_copy) {
		struct nvme_command_csd *cmd = (struct nvme_command_csd *)(cmd_base);
		struct source_range_entry sre;

		get_prp_data((struct nvme_command *)cmd, &sre, sizeof(struct source_range_entry), true);

		offset = sre.saddr << 9;
		length = sre.nByte;
		slm_addr = get_slm_addr(cmd->namespace_copy.sdaddr);

		// Data is not written/read from prp list for namespace copy commands
		if (cmd->namespace_copy.control_flag == nvme_cmd_write) {
			copy_from_slm(vdev->ns[nsid].mapped + offset, slm_addr, length);
			/* Notify consumption for circular output buffers */
			notify_output_consumed(slm_addr, length);
		} else if (cmd->namespace_copy.control_flag == nvme_cmd_read) {
			copy_to_slm(slm_addr, vdev->ns[nsid].mapped + offset, length);
		}

		return length;
	} else
#endif
	{
		offset = rw_cmd->slba << 9;
		length = (rw_cmd->length + 1) << 9;
		prp1 = rw_cmd->prp1;
		prp2 = rw_cmd->prp2;
	}
	remaining = length;

	while (remaining) {
		size_t io_size;
		void *vaddr;
		size_t mem_offs = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = prp1;
		} else if (prp_offs == 2) {
			paddr = prp2;
			if (remaining > PAGE_SIZE) {
				paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) + (paddr & PAGE_OFFSET_MASK);
				paddr = paddr_list[prp2_offs++];
			} else if (opcode == nvme_cmd_memory_read || opcode == nvme_cmd_memory_write) {
				if (is_adjusted) {
					// For SLM memory read/write, host does not know the actual length after adjustment
					paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) + (paddr & PAGE_OFFSET_MASK);
					paddr = paddr_list[prp2_offs++];
				}
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

		if (opcode == nvme_cmd_write) {
			memcpy(vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
		} else if (opcode == nvme_cmd_read) {
			memcpy(vaddr + mem_offs, vdev->ns[nsid].mapped + offset, io_size);
		}
#if (CSD_ENABLE == 1)
		else if (opcode == nvme_cmd_memory_write) {
			NVMEV_DEBUG("MEM WRITE slm_id:%lu offset:%lu io_size:%lu vaddr:%p", slm_addr, offset, io_size,
						vaddr + mem_offs);
			copy_to_slm(slm_addr + offset, vaddr + mem_offs, io_size);
		} else if (opcode == nvme_cmd_memory_read) {
			NVMEV_DEBUG("MEM READ slm_id:%lu offset:%lu io_size:%lu vaddr:%p", slm_addr, offset, io_size,
						vaddr + mem_offs);
			copy_from_slm(vaddr + mem_offs, slm_addr + offset, io_size);
		}
#endif
		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if ((opcode == nvme_cmd_memory_read) || (opcode == nvme_cmd_memory_write)) {
		struct nvme_command_csd *cmd = (struct nvme_command_csd *)(cmd_base);
		NVMEV_CSD_PROFILE_END("SLM", 0, cmd->memory.cdw14, 0);
		NVMEV_DEBUG("CSD Addr:%lu (%d, %d)", get_slm_offset(slm_addr), sqid, sq_entry);

		/* Notify consumption for circular output buffers */
		if (opcode == nvme_cmd_memory_read) {
			notify_output_consumed(slm_addr, length);
		}
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length;
}

static u64 paddr_list[5][513] = {
	0,
}; // Not using index 0 to make max index == num_prp
static unsigned int __do_perform_io_using_dma(int id, int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;

	offset = sq_entry(sq_entry).rw.slba << 9;
	length = (sq_entry(sq_entry).rw.length + 1) << 9;
	remaining = length;

	memset(paddr_list[id], 0, sizeof(paddr_list[id]));
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr_list[id][prp_offs] = sq_entry(sq_entry).rw.prp1;
		} else if (prp_offs == 2) {
			paddr_list[id][prp_offs] = sq_entry(sq_entry).rw.prp2;
			if (remaining > PAGE_SIZE) {
				tmp_paddr_list =
					kmap_atomic_pfn(PRP_PFN(paddr_list[id][prp_offs])) + (paddr_list[id][prp_offs] & PAGE_OFFSET_MASK);
				paddr_list[id][prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			paddr_list[id][prp_offs] = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[id][prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[id][prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_list != NULL)
		kunmap_atomic(tmp_paddr_list);

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[id][prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[id][prp_offs] == paddr_list[id][prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
			ioat_dma_submit(id, paddr, vdev->config.storage_start + offset, io_size);
		} else if (sq_entry(sq_entry).rw.opcode == nvme_cmd_read) {
			ioat_dma_submit(id, vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}

	return length;
}

static void __enqueue_io_req(int sqid, int cqid, int sq_entry, unsigned int sq_head,
			     unsigned long long nsecs_start, struct nvmev_result *ret,
			     struct nvme_command *cmd, bool copy_cmd)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

#if SUPPORT_MULTI_IO_WORKER_BY_SQ
	unsigned int proc_turn = (sqid - 1) % (vdev->config.nr_io_cpu);
#else
	unsigned int proc_turn = vdev->proc_turn;
#endif
	struct nvmev_proc_info *pi;
	unsigned int entry;
	struct nvme_command *cmd_ref = cmd;

#if (CSD_ENABLE == 1)
	// if (sq_entry(sq_entry).common.opcode == nvme_cmd_compound_execute_program || sq_entry(sq_entry).common.opcode == nvme_cmd_execute_program || sq_entry(sq_entry).common.opcode == nvme_cmd_memory_management || sq_entry(sq_entry).common.opcode == nvme_cmd_memory_copy) {
	if (IS_CSD_PROCESS(cmd->common.opcode)) {
		pi = &vdev->csd_proc_info[0];
		if (copy_cmd) {
			printk("Something is wrong!!! CSD command was deferred - %d\n", cmd->common.opcode);
		}
	} else
#endif
	{
		pi = &vdev->proc_info[proc_turn];
		if (++proc_turn == vdev->config.nr_io_cpu)
			proc_turn = 0;
		vdev->proc_turn = proc_turn;
	}
	entry = pi->free_seq;

	if (pi->proc_table[entry].next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		pi->free_seq = entry;
		return;
	}
	
	if (copy_cmd) {
		cmd_ref = kmemdup(cmd, sizeof(*cmd), GFP_KERNEL);
		if (!cmd_ref) {
			NVMEV_ERROR("Failed to copy cmd for sq %d entry %d\n", sqid, sq_entry);
			BUG();
		}
	}

	pi->free_seq = pi->proc_table[entry].next;
	BUG_ON(pi->free_seq >= NR_MAX_PARALLEL_IO);

	NVMEV_DEBUG("%s/%u[%d], sq %d cq %d, entry %d %llu + %llu\n", pi->thread_name, entry, cmd->rw.opcode,
				sqid, cqid, sq_entry, nsecs_start, ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	pi->proc_table[entry].sqid = sqid;
	pi->proc_table[entry].cqid = cqid;
	pi->proc_table[entry].sq_entry = sq_entry;
	pi->proc_table[entry].sq_head = sq_head;
	pi->proc_table[entry].command_id = cmd->common.command_id;
	pi->proc_table[entry].cmd = cmd_ref;
	pi->proc_table[entry].owns_cmd = copy_cmd;
	pi->proc_table[entry].nsecs_start = nsecs_start;
	pi->proc_table[entry].nsecs_enqueue = local_clock();
	pi->proc_table[entry].nsecs_nand_start = ret->nsecs_nand_start;
	pi->proc_table[entry].nsecs_target = ret->nsecs_target;
	pi->proc_table[entry].status = ret->status;
	pi->proc_table[entry].is_completed = false;
	pi->proc_table[entry].is_copied = false;
	pi->proc_table[entry].prev = -1;
	pi->proc_table[entry].next = -1;

	pi->proc_table[entry].writeback_cmd = false;
	mb(); /* IO kthread shall see the updated pe at once */

	// (END) -> (START) order, nsecs target ascending order
	if (pi->io_seq == -1) {
		pi->io_seq = entry;
		pi->io_seq_end = entry;
	} else {
		unsigned int curr = pi->io_seq_end;

		while (curr != -1) {
			if (pi->proc_table[curr].nsecs_target <= pi->proc_io_nsecs)
				break;

			if (pi->proc_table[curr].nsecs_target <= ret->nsecs_target)
				break;

			curr = pi->proc_table[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			pi->proc_table[pi->io_seq].prev = entry;
			pi->proc_table[entry].next = pi->io_seq;
			pi->io_seq = entry;
		} else if (pi->proc_table[curr].next == -1) { /* Tail */
			pi->proc_table[entry].prev = curr;
			pi->io_seq_end = entry;
			pi->proc_table[curr].next = entry;
		} else { /* In between */
			pi->proc_table[entry].prev = curr;
			pi->proc_table[entry].next = pi->proc_table[curr].next;

			pi->proc_table[pi->proc_table[entry].next].prev = entry;
			pi->proc_table[curr].next = entry;
		}
	}
}

void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, struct buffer *write_buffer,
							  unsigned int buffs_to_release)
{
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
	unsigned int proc_turn = (sqid - 1) % (vdev->config.nr_io_cpu);
#else
	unsigned int proc_turn = vdev->proc_turn;
#endif
	struct nvmev_proc_info *pi = &vdev->proc_info[proc_turn];
	unsigned int entry = pi->free_seq;

	if (pi->proc_table[entry].next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		pi->free_seq = entry;
		return;
	}

	if (++proc_turn == vdev->config.nr_io_cpu)
		proc_turn = 0;
	vdev->proc_turn = proc_turn;
	pi->free_seq = pi->proc_table[entry].next;
	BUG_ON(pi->free_seq >= NR_MAX_PARALLEL_IO);

	NVMEV_DEBUG("%s/%u[%d], sq %d cq %d, entry %d %llu + %llu\n", pi->thread_name, entry, sq_entry(sq_entry).rw.opcode,
				sqid, cqid, sq_entry, nsecs_start, ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	pi->proc_table[entry].sqid = sqid;
	pi->proc_table[entry].sq_head = 0;
	pi->proc_table[entry].cmd = NULL;
	pi->proc_table[entry].owns_cmd = false;
	pi->proc_table[entry].nsecs_start = local_clock();
	pi->proc_table[entry].nsecs_enqueue = local_clock();
	pi->proc_table[entry].nsecs_target = nsecs_target;
	pi->proc_table[entry].is_completed = false;
	pi->proc_table[entry].is_copied = true;
	pi->proc_table[entry].prev = -1;
	pi->proc_table[entry].next = -1;

	pi->proc_table[entry].writeback_cmd = true;
	pi->proc_table[entry].buffs_to_release = buffs_to_release;
	pi->proc_table[entry].write_buffer = (void *)write_buffer;
	mb(); /* IO kthread shall see the updated pe at once */

	// (END) -> (START) order, nsecs target ascending order
	if (pi->io_seq == -1) {
		pi->io_seq = entry;
		pi->io_seq_end = entry;
	} else {
		unsigned int curr = pi->io_seq_end;

		while (curr != -1) {
			if (pi->proc_table[curr].nsecs_target <= pi->proc_io_nsecs)
				break;

			if (pi->proc_table[curr].nsecs_target <= nsecs_target)
				break;

			curr = pi->proc_table[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			pi->proc_table[pi->io_seq].prev = entry;
			pi->proc_table[entry].next = pi->io_seq;
			pi->io_seq = entry;
		} else if (pi->proc_table[curr].next == -1) { /* Tail */
			pi->proc_table[entry].prev = curr;
			pi->io_seq_end = entry;
			pi->proc_table[curr].next = entry;
		} else { /* In between */
			pi->proc_table[entry].prev = curr;
			pi->proc_table[entry].next = pi->proc_table[curr].next;

			pi->proc_table[pi->proc_table[entry].next].prev = entry;
			pi->proc_table[curr].next = entry;
		}
	}
}

static void __reclaim_completed_reqs(void)
{
	unsigned int turn;

	for (turn = 0; turn < vdev->config.nr_io_cpu; turn++) {
		struct nvmev_proc_info *pi;
		struct nvmev_proc_table *pe;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		unsigned int iter;
		int nr_reclaimed = 0;

		pi = &vdev->proc_info[turn];

		first_entry = pi->io_seq;
		curr = first_entry;

		while (curr != -1) {
			pe = &pi->proc_table[curr];
			if (pe->is_completed == true && pe->is_copied == true && pe->nsecs_target <= pi->proc_io_nsecs) {
				last_entry = curr;
				curr = pe->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

		if (last_entry != -1) {
			// Free allocated cmds
			iter = first_entry;
			while (true) {
				pe = &pi->proc_table[iter];

				if (pe->owns_cmd == true && pe->cmd != NULL) {
					kfree(pe->cmd);
				}
				pe->cmd = NULL;
				pe->owns_cmd = false;

				if (iter == last_entry) {
					break;
				}

				iter = pe->next;
			}

			// Unlink from io_seq list
			pe = &pi->proc_table[last_entry];
			pi->io_seq = pe->next;
			if (pe->next != -1) {
				pi->proc_table[pe->next].prev = -1;
			}
			pe->next = -1;

			pe = &pi->proc_table[first_entry];
			pe->prev = pi->free_seq_end;

			pe = &pi->proc_table[pi->free_seq_end];
			pe->next = first_entry;

			pi->free_seq_end = last_entry;
			NVMEV_DEBUG("Reclaimed %u -- %u, %d\n", first_entry, last_entry, nr_reclaimed);
		}
	}

	for (turn = 0; turn < 1; turn++) {
		struct nvmev_proc_info *pi;
		struct nvmev_proc_table *pe;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		pi = &vdev->csd_proc_info[turn];

		first_entry = pi->io_seq;
		curr = first_entry;

		while (curr != -1) {
			unsigned int next_entry = pi->proc_table[curr].next;
			pe = &pi->proc_table[curr];

			if (pe->is_completed == true && pe->is_copied == true) {
				// Free allocated cmds
				if (pe->owns_cmd == true && pe->cmd != NULL) {
					kfree(pe->cmd);
					printk("ERROR: CSD cmd was deferred\n");
				}

				// Unlink from io_seq list
				if (pe->prev != -1) {
					pi->proc_table[pe->prev].next = pe->next; // prev->next skips current entry
				} else {
					pi->io_seq = pe->next; // curr was head, update io_seq
				}

				if (pe->next != -1) {
					pi->proc_table[pe->next].prev = pe->prev; // next->prev skips current entry
				} else {
					pi->io_seq_end = pe->prev; // curr was tail, update io_seq_end 
				}

				// Add to free list
				pe->prev = pi->free_seq_end; // link curr to end of free list
				pe->next = -1; // curr is new end, so next is -1

				if (pi->free_seq_end != -1) {
					pi->proc_table[pi->free_seq_end].next = curr; // old end points to curr
				}
				pi->free_seq_end = curr; // update free_seq_end
				nr_reclaimed++;
			}

			curr = next_entry;
		}

		if (nr_reclaimed > 0) {
			NVMEV_DEBUG("Reclaimed %d CSD entries\n", nr_reclaimed);
		}
	}
}

void get_prp_data(struct nvme_command *cmd, void *buf, size_t size, bool from_host)
{
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
			paddr = cmd->rw.prp1;
		} else if (prp_offs == 2) {
			paddr = cmd->rw.prp2;
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

static enum nvmev_io_status __nvmev_proc_io(int sqid, int sq_entry, unsigned int sq_head,
					    struct nvme_command *cmd_override, bool copy_cmd,
					    unsigned long long nsecs_start)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvme_command *cmd = cmd_override ? cmd_override : &sq_entry(sq_entry);
	#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0; // Some KVSSD programs give 0 as nsid for KV IO
#else
	uint32_t nsid = cmd->common.nsid - 1;
#endif
	struct nvmev_ns *ns = &vdev->ns[nsid];

	struct nvmev_request req = {
		.cmd = cmd,
		.sq_id = sqid,
		.nsecs_start = nsecs_start,
	};
	struct nvmev_result ret = {
		.nsecs_nand_start = nsecs_start,
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
	};

	enum nvmev_io_status status;

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	status = ns->proc_io_cmd(ns, &req, &ret);
	if (status == NVMEV_IO_STATUS_DEFERRED) {
		return status;
	} else if (status == NVMEV_IO_STATUS_ERROR) {
		NVMEV_ERROR("IO error on sq %d entry %d cmd opcode %d\n", sqid, sq_entry, cmd->common.opcode);
		BUG();
		return status;
	}

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	__enqueue_io_req(sqid, sq->cqid, sq_entry, sq_head, nsecs_start, &ret, cmd, copy_cmd);

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	__reclaim_completed_reqs();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n", clock1 / counter, clock2 / counter, clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return NVMEV_IO_STATUS_SUCCESS;
}

int nvmev_proc_io_sq(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;
	const int max_batch = 4;

	if (unlikely(!sq))
		return old_db;
	if (unlikely(num_proc < 0))
		num_proc += sq->queue_size;

	if (num_proc > max_batch)
		num_proc = max_batch;

	for (seq = 0; seq < num_proc; seq++) {
		enum nvmev_io_status status;
		unsigned long long nsecs_start = __get_wallclock();
		unsigned int sq_head_next = sq_entry + 1;

		if (sq_head_next == sq->queue_size)
			sq_head_next = 0;

		status = __nvmev_proc_io(sqid, sq_entry, sq_head_next, NULL, false, nsecs_start);

		if (status == NVMEV_IO_STATUS_SUCCESS) {
			sq->stat.nr_dispatched++;
			sq->stat.nr_in_flight++;
		} else if (status == NVMEV_IO_STATUS_DEFERRED) {
			nvmev_enqueue_deferred(sq, sq_entry, sq_head_next, nsecs_start);
		} else if (status == NVMEV_IO_STATUS_ERROR) {
			sq->stat.nr_dispatched++;
			sq->stat.nr_in_flight++;
		}

		sq_entry = sq_head_next;
	}
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + num_proc) % sq->queue_size;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}
		vdev->sqes[cq_entry(i).sq_id]->stat.nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1)
		cq->cq_tail = cq->queue_size - 1;
}

bool nvmev_proc_io_dq(int sqid)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvmev_deferred_io *node;
	unsigned int processed = 0;
	unsigned int max_entries;
	bool progressed = false;

	if (!sq || !sq->deferred_count)
		return false;

	max_entries = sq->deferred_count;

	while (processed < max_entries) {
		enum nvmev_io_status status;

		if (list_empty(&sq->deferred_list))
			break;

		node = list_first_entry(&sq->deferred_list, struct nvmev_deferred_io, list);
		list_del_init(&node->list);

		if (sq->deferred_count)
			sq->deferred_count--;

		status = __nvmev_proc_io(sq->qid, node->sq_entry, node->sq_head,
					 &node->cmd, true, node->nsecs_start);

		if (status == NVMEV_IO_STATUS_SUCCESS) {
			sq->stat.nr_dispatched++;
			sq->stat.nr_in_flight++;
			sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight,
							  sq->stat.nr_in_flight);
			if (nvmev_deferred_io_cache)
				kmem_cache_free(nvmev_deferred_io_cache, node);
			else
				kfree(node);
			progressed = true;
		} else if (status == NVMEV_IO_STATUS_DEFERRED) {
			list_add(&node->list, &sq->deferred_list);
			sq->deferred_count++;
			processed++;
			break;
		} else if (status == NVMEV_IO_STATUS_ERROR) {
			NVMEV_ERROR("Deferred IO error on sq %d entry %d cmd opcode %d\n",
				    sqid, node->sq_entry, node->cmd.common.opcode);
			sq->stat.nr_dispatched++;
			sq->stat.nr_in_flight++;
			sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight,
							  sq->stat.nr_in_flight);
			if (nvmev_deferred_io_cache)
				kmem_cache_free(nvmev_deferred_io_cache, node);
			else
				kfree(node);
			progressed = true;
		} else {
			if (nvmev_deferred_io_cache)
				kmem_cache_free(nvmev_deferred_io_cache, node);
			else
				kfree(node);
		}

		processed++;
	}

	return progressed;
}

static void __fill_cq_result(struct nvmev_proc_table *proc_entry)
{
	int sqid = proc_entry->sqid;
	int cqid = proc_entry->cqid;
	int sq_entry = proc_entry->sq_entry;
	int sq_head;
	unsigned int command_id = proc_entry->command_id;
	unsigned int status = proc_entry->status;
	unsigned int result0 = proc_entry->result0;
	unsigned int result1 = proc_entry->result1;

	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head;

	if (sq && unlikely(proc_entry->sq_head >= sq->queue_size))
		sq_head = proc_entry->sq_head % sq->queue_size;
	else
		sq_head = proc_entry->sq_head;

	spin_lock(&cq->entry_lock);

	cq_head = cq->cq_head;
	cq_entry(cq_head).command_id = command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_head;
	cq_entry(cq_head).status = cq->phase | status << 1;
	cq_entry(cq_head).result0 = result0;
	cq_entry(cq_head).result1 = result1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static int nvmev_kthread_io(void *data)
{
	struct nvmev_proc_info *pi = (struct nvmev_proc_info *)data;
	struct nvmev_ns *ns;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif

	NVMEV_INFO("%s started on cpu %d (node %d), id: %d\n", pi->thread_name, smp_processor_id(),
			   cpu_to_node(smp_processor_id()), pi->id);

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = pi->io_seq;
		int qidx;

		while (curr != -1) {
			struct nvmev_proc_table *pe = &pi->proc_table[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			pi->proc_io_nsecs = curr_nsecs;

			if (pe->is_completed == true) {
				curr = pe->next;
				continue;
			}

			if (pe->is_copied == false) {
#ifdef PERF_DEBUG
				unsigned long long memcpy_time;
				pe->nsecs_copy_start = local_clock() + delta;
#endif
				struct nvme_command *cmd = pe->cmd;
				struct nvme_command *nvme_cmd = (struct nvme_command *)(cmd);
				if (pe->writeback_cmd) {
					;
				} else if (io_using_dma && (((nvme_cmd->rw.length + 1) << 9) >= 65536) 
						&& (nvme_cmd->common.opcode == nvme_cmd_write || nvme_cmd->common.opcode == nvme_cmd_read)) {
					__do_perform_io_using_dma(pi->id, pe->sqid, pe->sq_entry);
				} else {
					
					__do_perform_io(pe->sqid, cmd);
				}

#ifdef PERF_DEBUG
				pe->nsecs_copy_done = local_clock() + delta;
				memcpy_time = pe->nsecs_copy_done - pe->nsecs_copy_start;
#endif
				pe->is_copied = true;

				NVMEV_DEBUG("%s: copied %u, %d %d %d\n", pi->thread_name, curr, pe->sqid, pe->cqid, pe->sq_entry);
			}

			if (pe->nsecs_target <= curr_nsecs) {
				if (pe->writeback_cmd) {
#if (BASE_SSD == SAMSUNG_970PRO || BASE_SSD == ZNS_PROTOTYPE)
					buffer_release((struct buffer *)pe->write_buffer, pe->buffs_to_release);
#endif
				} else {
#if (CSD_ENABLE == 1)
					/* Use pe->cmd instead of sq_entry() because csd_ftl.c may have
					 * modified the command (e.g., adjusted length for circular buffers) */
					struct nvme_command_csd *cmd = (struct nvme_command_csd *)pe->cmd;
					if (cmd->common.opcode == nvme_cmd_namespace_copy) {
						// printk("TIME: %llu %llu %llu\n", pe->nsecs_start, pe->nsecs_nand_start, pe->nsecs_target);
						NVMEV_CSD_PROFILE_WITH_TIME("NVMWRITE", 0, cmd->memory.cdw14, 0, pe->nsecs_nand_start,
													pe->nsecs_target);
						pe->result0 = cmd->memory_copy.length;
#ifdef CONFIG_NVMEV_PROFILE_IO
						atomic64_add(cmd->memory_copy.length, &vdev->io_stat_csd_write);
#endif
					} else if (cmd->common.opcode == nvme_cmd_memory_write ||
							   cmd->common.opcode == nvme_cmd_memory_read) {
						pe->result0 = cmd->memory.length;
#ifdef CONFIG_NVMEV_PROFILE_IO
						if (cmd->common.opcode == nvme_cmd_memory_read) {
							atomic64_add(cmd->memory.length, &vdev->io_stat_host_read);
						} else {
							atomic64_add(cmd->memory.length, &vdev->io_stat_host_write);
						}
#endif
					}
#endif
					// struct nvme_command *nvme_cmd = (struct nvme_command *)(&sq_entry(pe->sq_entry));
					// if (curr_nsecs - pe->nsecs_target > 1000000) {
					// 	NVMEV_ERROR("IO_Long_tail_Latency: %d, %lu, %lu %lu\n", nvme_cmd->rw.opcode, (nvme_cmd->rw.length + 1) << 9, pe->nsecs_target, curr_nsecs);
					// }
#ifdef CONFIG_NVMEV_PROFILE_IO
					if (pe->cmd->common.opcode == nvme_cmd_read) {
						atomic64_add((pe->cmd->rw.length + 1) << 9, &vdev->io_stat_host_read);
					} else if (pe->cmd->common.opcode == nvme_cmd_write) {
						atomic64_add((pe->cmd->rw.length + 1) << 9, &vdev->io_stat_host_write);
					}
#endif
					__fill_cq_result(pe);
				}

				NVMEV_DEBUG("%s: completed %u, %d %d %d\n", pi->thread_name, curr, pe->sqid, pe->cqid, pe->sq_entry);

#ifdef PERF_DEBUG
				pe->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu\n", pe->nsecs_start, pe->nsecs_enqueue - pe->nsecs_start,
							 pe->nsecs_copy_start - pe->nsecs_start, pe->nsecs_copy_done - pe->nsecs_start,
							 pe->nsecs_cq_filled - pe->nsecs_start, pe->nsecs_target - pe->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */
				pe->is_completed = true;
			}

			curr = pe->next;
		}

		for (qidx = 1; qidx <= vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = vdev->cqes[qidx];
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
			if ((pi->id) != ((qidx - 1) % vdev->config.nr_io_cpu))
				continue;
#endif
			if (cq == NULL || !cq->irq_enabled)
				continue;

			if (mutex_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {
#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						NVMEV_DEBUG("Intr %d: %llu\n", qidx, intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				mutex_unlock(&cq->irq_lock);
			}
		}
		cond_resched();
	}

	return 0;
}

void NVMEV_IO_PROC_INIT(struct nvmev_dev *vdev)
{
	unsigned int i, proc_idx;

	vdev->proc_info = kcalloc(sizeof(struct nvmev_proc_info), vdev->config.nr_io_cpu, GFP_KERNEL);
	vdev->proc_turn = 0;

	for (proc_idx = 0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		struct nvmev_proc_info *pi = &vdev->proc_info[proc_idx];

		pi->proc_table = kzalloc_node(sizeof(struct nvmev_proc_table) * NR_MAX_PARALLEL_IO, GFP_KERNEL, 1);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			pi->proc_table[i].next = i + 1;
			pi->proc_table[i].prev = i - 1;
		}
		pi->proc_table[NR_MAX_PARALLEL_IO - 1].next = -1;
#if SUPPORT_MULTI_IO_WORKER_BY_SQ
		pi->id = proc_idx;
#endif
		pi->free_seq = 0;
		pi->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		pi->io_seq = -1;
		pi->io_seq_end = -1;

		snprintf(pi->thread_name, sizeof(pi->thread_name), "nvmev_proc_io_%d", proc_idx);

		pi->nvmev_io_worker = kthread_create(nvmev_kthread_io, pi, pi->thread_name);

		kthread_bind(pi->nvmev_io_worker, vdev->config.cpu_nr_proc_io[proc_idx]);
		wake_up_process(pi->nvmev_io_worker);
	}
}

void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev)
{
	unsigned int i;

	for (i = 0; i < vdev->config.nr_io_cpu; i++) {
		struct nvmev_proc_info *pi = &vdev->proc_info[i];

		if (!IS_ERR_OR_NULL(pi->nvmev_io_worker)) {
			kthread_stop(pi->nvmev_io_worker);
		}

		kfree(pi->proc_table);
	}

	kfree(vdev->proc_info);
}
