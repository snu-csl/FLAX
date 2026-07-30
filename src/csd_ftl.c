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
 **********************************************************************/

#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "nvme_csd.h"
#include "csd_ftl.h"
#include "csd_slm.h"
#include "csd_dispatcher.h"

spinlock_t entry_lock;
struct nvmev_ns internal_ns;
extern struct nvmev_dev *vdev;

int enable_throttling;

size_t issue_count_for_internal;
size_t issue_count_for_external;
size_t complete_count_for_internal;
size_t complete_count_for_external;
size_t refill_token_for_internal;
size_t refill_token_for_external;
size_t token_for_internal;
size_t token_for_external;

size_t last_io_size;
size_t init_token = 2 * 1024 * 1024;

size_t lastest_token = -1;

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

struct nvmev_result csd_get_target_latency(uint64_t slba, uint64_t len, unsigned long long nsecs_start)
{
	struct nvme_command cmd;
	struct nvmev_request req = {
		.cmd = &cmd,
		.sq_id = 0xFFFFFFFF,
		.nsecs_start = nsecs_start,
	};

	struct nvmev_result ret = {
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
	};

	uint64_t nsecs_target = 0;
	uint64_t nsecs_nand_start = 0;
	size_t block_size = 0;
	if (len < 512) {
		block_size = 1;
	} else {
		block_size = len / 512 - 1;
	}

	size_t offset = 0;
	size_t remain = block_size;
	while (remain > 0) {
		size_t temp_len = (remain > 0xFFFF) ? 0xFFFF : remain;
		memset(&cmd, 0, sizeof(struct nvme_command));
		cmd.rw.slba = slba + offset;
		cmd.rw.length = temp_len;
		cmd.rw.opcode = nvme_cmd_read;
		// printk("Target_latency:%llu(%lu, %lu)", ret.nsecs_target, offset, remain);

		if (temp_len > 0xFFFF) {
			NVMEV_ERROR("block size overflow:%lu", temp_len);
		}

		if (csd_proc_nvme_io_cmd(&internal_ns, &req, &ret) != NVMEV_IO_STATUS_SUCCESS) {
			ret.nsecs_target = -1;
			return ret;
		}

		if (nsecs_target < ret.nsecs_target) {
			nsecs_target = ret.nsecs_target;
		}

		if (nsecs_nand_start < ret.nsecs_nand_start) {
			nsecs_nand_start = ret.nsecs_nand_start;
		}

		offset = offset + (temp_len + 1);
		remain = remain - temp_len;
	}

	ret.nsecs_target = nsecs_target;
	ret.nsecs_nand_start = nsecs_nand_start;

	return ret;
}

uint64_t get_seq_lpn(uint64_t start_lba, size_t max_size)
{
	struct nvme_command cmd;
	struct nvmev_request req = {
		.cmd = &cmd,
		.sq_id = 0xFFFFFFFF,
		.nsecs_start = 0,
	};

	struct nvmev_result ret = {
		.nsecs_nand_start = 0,
		.nsecs_target = 0,
		.status = NVME_SC_SUCCESS,
	};

	cmd.rw.slba = start_lba;
	cmd.rw.length = max_size / 512 - 1;
	cmd.rw.opcode = nvme_test;
	if (csd_proc_nvme_io_cmd(&internal_ns, &req, &ret) != NVMEV_IO_STATUS_SUCCESS) {
		NVMEV_ERROR("csd_proc_nvme_io_cmd error");
	}

	return ret.nsecs_target;
}

bool get_extra_token(void)
{
	bool isEmpty = false;
	int dbs_idx;
	int new_db;
	int old_db;
	isEmpty = true;
	for (int qid = 1; qid <= vdev->nr_sq; qid++) {
		if (vdev->sqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			isEmpty = false;
			break;
		}
	}

	if (isEmpty) {
		if (lastest_token == -1) {
			lastest_token = token_for_external;
		} else {
			if (lastest_token == token_for_external) {
				NVMEV_DEBUG("Extra Token Refill : %lu, %lu (%lu)", complete_count_for_external,
							issue_count_for_external, lastest_token);
				return true;
			}
			lastest_token = -1;
		}
	} else {
		lastest_token = -1;
	}

	return false;
}

void notify_internal_cmd(void)
{
	complete_count_for_internal += 128 * 1024;
}
void csd_notify_io_cmd(size_t len)
{
	complete_count_for_external += len;
	last_io_size = len;
}

void csd_init_namespace(struct nvmev_ns *ns)
{
	// Using conventional FTL
	memcpy(&internal_ns, ns, sizeof(struct nvmev_ns));

	ns->proc_io_cmd = csd_proc_nvme_io_cmd;
	//ns->notify_io_cmd = csd_notify_io_cmd;
	ns->notify_io_cmd = NULL;

	enable_throttling = 0;
	token_for_external = init_token / 2;
	refill_token_for_external = token_for_external;
	token_for_internal = init_token / 2;
	refill_token_for_internal = token_for_external;
	issue_count_for_internal = 0;
	issue_count_for_external = 0;
	complete_count_for_internal = 0;
	complete_count_for_external = 0;

	spin_lock_init(&entry_lock);

	return;
}

void refill_token(void)
{
	if (token_for_internal == 0 && token_for_external == 0) {
		token_for_internal = refill_token_for_internal;
		token_for_external = refill_token_for_external;
	}
}
void adjust_throttling(void)
{
	size_t complete_count = complete_count_for_external + complete_count_for_internal;
	size_t issue_count = issue_count_for_external + issue_count_for_internal;
	size_t ratio = 0;
	size_t ratio_for_internal = 0;
	size_t ratio_for_external = 0;

	NVMEV_DEBUG("[Throttling] HostIO :%lu/%lu, SLMIO : %lu/%lu (%u / %u)", complete_count_for_external,
				issue_count_for_external, complete_count_for_internal, issue_count_for_internal,
				refill_token_for_external, refill_token_for_internal);
	if (issue_count > 0) {
		ratio = complete_count * 1000 / issue_count;
		NVMEV_DEBUG("[Throttling] Ratio(total) : %lu, %lu %lu", complete_count, issue_count, ratio);
		if (issue_count_for_internal > 0) {
			ratio_for_internal = complete_count_for_internal * 1000 / issue_count_for_internal;
			NVMEV_DEBUG("[Throttling] Ratio(internal) : %lu, %lu %lu", complete_count_for_internal,
						issue_count_for_internal, ratio_for_internal);
		}
		if (issue_count_for_external > 0) {
			ratio_for_external = complete_count_for_external * 1000 / issue_count_for_external;
			NVMEV_DEBUG("[Throttling] Ratio(external) : %lu, %lu %lu", complete_count_for_external,
						issue_count_for_external, ratio_for_external);
		}
	} else
		NVMEV_DEBUG("[Throttling] HostIO : %lu, %lu %lu", complete_count, issue_count);

	complete_count_for_external = 0;
	issue_count_for_external = 0;
	complete_count_for_internal = 0;
	issue_count_for_internal = 0;

	NVMEV_DEBUG("[Throttling] HostIO :%lu/%lu, SLMIO : %lu/%lu (%u / %u)", complete_count_for_external,
				issue_count_for_external, complete_count_for_internal, issue_count_for_internal,
				refill_token_for_external, refill_token_for_internal);
}

enum nvmev_io_status csd_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
					  struct nvmev_result *ret)
{
	enum nvmev_io_status status = NVMEV_IO_STATUS_SUCCESS;
	struct nvme_command_csd *cmd = (struct nvme_command_csd *)req->cmd;

	switch (cmd->common.opcode) {
	case nvme_cmd_compound_execute_program:
		// Latency will be determined before actual operation take place
		break;
	case nvme_cmd_memory_management: {
		break;
	}
	case nvme_cmd_memory_copy: {
		break;
	}
	case nvme_cmd_memory_read:
	case nvme_cmd_memory_write: {
#if (SUPPORT_ASYNC_COMPUTE == 1)
		uint64_t slm_addr = get_slm_addr(cmd->memory.sb);
		size_t length = cmd->memory.length;
		if (check_output_slm_data_ready(slm_addr, &length) == false) {
			status = NVMEV_IO_STATUS_DEFERRED;
			break;
		}
		if (cmd->memory.length == length) {
			cmd->memory.cdw13 = 0;
		} else {
			cmd->memory.cdw13 = 1; // indicate length adjusted
		}
		cmd->memory.length = length;
#endif
		spin_lock(&entry_lock);
		status = internal_ns.proc_io_cmd(ns, req, ret);
		spin_unlock(&entry_lock);
		break;
	}
	case nvme_cmd_namespace_copy: {
#if (SUPPORT_ASYNC_COMPUTE == 1)
		uint64_t slm_addr = get_slm_addr(cmd->memory.sb);
		size_t length = cmd->memory_copy.length;
		if (check_output_slm_data_ready(slm_addr, &length) == false) {
			status = NVMEV_IO_STATUS_DEFERRED;
			break;
		}
		cmd->memory_copy.length = length;
#endif
		spin_lock(&entry_lock);
		status = internal_ns.proc_io_cmd(ns, req, ret);
		spin_unlock(&entry_lock);
		break;
	}
	default: {
		if (status == NVMEV_IO_STATUS_SUCCESS) {
			spin_lock(&entry_lock);
			status = internal_ns.proc_io_cmd(ns, req, ret);
			spin_unlock(&entry_lock);
		}
		break;
	}
	}
	return status;
}

void notify_start_throttling(void)
{
	NVMEV_DEBUG("[THROTTLING START] H/I/F: %lu/%lu/%u", complete_count_for_external / 1024,
				complete_count_for_internal / 1024, refill_token_for_external);

	if (enable_throttling == 0) {
		token_for_external = init_token / 2;
		refill_token_for_external = token_for_external;
		token_for_internal = init_token / 2;
		refill_token_for_internal = token_for_internal;
		issue_count_for_internal = 0;
		issue_count_for_external = 0;
		complete_count_for_internal = 0;
		complete_count_for_external = 0;
	}
	enable_throttling++;
}

void notify_finish_throttling(void)
{
	NVMEV_DEBUG("[THROTTLING END] H/I/F: %lu/%lu/%u", complete_count_for_external / 1024,
				complete_count_for_internal / 1024, refill_token_for_external);

	if (enable_throttling > 0) {
		token_for_external = init_token / 2;
		refill_token_for_external = token_for_external;
		token_for_internal = init_token / 2;
		refill_token_for_internal = token_for_internal;
		issue_count_for_internal = 0;
		issue_count_for_external = 0;
		complete_count_for_internal = 0;
		complete_count_for_external = 0;
		enable_throttling--;
	}
}