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

#ifndef _NVMEVIRT_CSD_FTL_H
#define _NVMEVIRT_CSD_FTL_H

#include <linux/types.h>

struct nvmev_result csd_get_target_latency(uint64_t slba, uint64_t len, unsigned long long nsec_start);
void csd_init_namespace(struct nvmev_ns *ns);
enum nvmev_io_status csd_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
					  struct nvmev_result *ret);

uint64_t get_seq_lpn(uint64_t start_lba, size_t max_size);

void notify_start_throttling(void);
void notify_finish_throttling(void);

void notify_internal_cmd(void);
void adjust_throttling(void);
#endif
