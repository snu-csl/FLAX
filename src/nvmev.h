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

#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/list.h>
#include <asm/apic.h>

#include "nvme.h"
#include "ssd_config.h"

#if (CSD_ENABLE == 1)
#include "profile.h"
#endif

#undef CONFIG_NVMEV_DEBUG_VERBOSE

#define SUPPORT_MULTI_IO_WORKER_BY_SQ 1

#undef CONFIG_NVMEV_PROFILE_IO
// #define CONFIG_NVMEV_PROFILE_IO

/*************************/
#define NVMEV_DRV_NAME "NVMeVirt"
#define NVMEV_VERSION 0x0110
#define NVMEV_DEVICE_ID	NVMEV_VERSION
#define NVMEV_VENDOR_ID 0x0c51
#define NVMEV_SUBSYSTEM_ID	0x370d
#define NVMEV_SUBSYSTEM_VENDOR_ID NVMEV_VENDOR_ID

#define NVMEV_INFO(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ERROR(string, args...) printk(KERN_ERR "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ASSERT(x) BUG_ON((!(x)))

#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
#define NVMEV_DEBUG(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#else
#define NVMEV_DEBUG(string, args...)
#endif

#define NR_MAX_IO_QUEUE 72
#define NR_MAX_PARALLEL_IO 16384

#define NVMEV_INTX_IRQ 15

#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)
#define PRP_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))

#define KB(k) ((k)*1024)
#define MB(m) (KB((m)*1024))

#define BYTE_TO_KB(b) ((b) >> 10)
#define BYTE_TO_MB(b) ((b) >> 20)
#define BYTE_TO_GB(b) ((b) >> 30)

#define MS_PER_SEC(s) ((s)*1000)
#define US_PER_SEC(s) (MS_PER_SEC(s) * 1000)
#define NS_PER_SEC(s) (US_PER_SEC(s) * 1000)

#define LBA_TO_BYTE(lba) ((lba) << 9)
#define BYTE_TO_LBA(byte) ((byte) >> 9)

#define INVALID32 (0xFFFFFFFF)
#define INVALID64 (0xFFFFFFFFFFFFFFFF)
#define ASSERT(X)

struct nvmev_sq_stat {
	unsigned int nr_dispatched;
	unsigned int nr_dispatch;
	unsigned int nr_in_flight;
	unsigned int max_nr_in_flight;
	unsigned long long total_io;
};

struct nvmev_deferred_io {
	struct list_head list;
	int sq_entry;
	unsigned int sq_head;
	struct nvme_command cmd;
	u64 nsecs_start;
};

struct nvmev_submission_queue {
	int qid;
	int cqid;
	int priority;
	bool phys_contig;

	int queue_size;

	struct nvmev_sq_stat stat;

	struct nvme_command __iomem **sq;

	struct list_head deferred_list;
	unsigned int deferred_count;
};

struct nvmev_completion_queue {
	int qid;
	int irq_vector;
	bool irq_enabled;
	bool interrupt_ready;
	bool phys_contig;

	spinlock_t entry_lock;
	struct mutex irq_lock;

	int queue_size;

	int phase;
	int cq_head;
	int cq_tail;

	struct nvme_completion __iomem **cq;
};

struct nvmev_admin_queue {
	int phase;

	int sq_depth;
	int cq_depth;

	int cq_head;

	struct nvme_command __iomem **nvme_sq;
	struct nvme_completion __iomem **nvme_cq;
};

#define NR_SQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_command))
#define NR_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_completion))

#define SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_CQE_PER_PAGE)

#define SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_CQE_PER_PAGE)

struct nvmev_config {
	unsigned long memmap_start; // byte
	unsigned long memmap_size; // byte

	unsigned long slm_start; // byte
	unsigned long slm_size; // byte

	unsigned long storage_start; //byte
	unsigned long storage_size; // byte

	unsigned int read_delay; // ns
	unsigned int read_time; // ns
	unsigned int read_trailing; // ns
	unsigned int write_delay; // ns
	unsigned int write_time; // ns
	unsigned int write_trailing; // ns

	unsigned int nr_io_units;
	unsigned int io_unit_shift; // 2^

	unsigned int cpu_nr_dispatcher;
	unsigned int nr_io_cpu;
	unsigned int cpu_nr_proc_io[32];
	unsigned int cpu_nr_csd_dispatcher;
	unsigned int cpu_nr_csd_helper;
	unsigned int nr_csd_cpu;
	unsigned int cpu_nr_csd[32];
	unsigned int nr_slm_cpu;
	unsigned int cpu_nr_slm[32];
};

struct nvmev_proc_table {
	int sqid;
	int cqid;

	int sq_entry;
	unsigned int command_id;

	unsigned int sq_head;
	struct nvme_command *cmd;
	bool owns_cmd;

	unsigned long long nsecs_start;
	unsigned long long nsecs_nand_start;
	unsigned long long nsecs_target;

	unsigned long long nsecs_enqueue;
	unsigned long long nsecs_copy_start;
	unsigned long long nsecs_copy_done;
	unsigned long long nsecs_cq_filled;

	bool is_copied;
	bool is_completed;

	unsigned int status;
	unsigned int result0;
	unsigned int result1;

	bool writeback_cmd;
	void *write_buffer;
	unsigned int buffs_to_release;

	unsigned int next, prev;
};

struct nvmev_proc_info {
	struct nvmev_proc_table *proc_table;

	unsigned int free_seq; /* free io req head index */
	unsigned int free_seq_end; /* free io req tail index */
	unsigned int io_seq; /* io req head index */
	unsigned int io_seq_end; /* io req tail index */
	unsigned int id;

	unsigned long long proc_io_nsecs;

	struct task_struct *nvmev_io_worker;
	char thread_name[32];
};

struct nvmev_dev {
	struct pci_bus *virt_bus;
	void *virtDev;
	struct pci_header *pcihdr;
	struct pci_pm_cap *pmcap;
	struct pci_msix_cap *msixcap;
	struct pcie_cap *pciecap;
	struct pci_ext_cap *extcap;

	struct pci_dev *pdev;

	struct nvmev_config config;
	struct task_struct *nvmev_manager;

	void *slm_mapped;
	void *storage_mapped;

	struct nvmev_proc_info *proc_info;
	unsigned int proc_turn;
	struct nvmev_proc_info *csd_proc_info;

	void __iomem *msix_table;
	bool intx_disabled;

	struct __nvme_bar *old_bar;
	struct nvme_ctrl_regs __iomem *bar;

	u32 *old_dbs;
	u32 __iomem *dbs;

	int nr_ns;
	int nr_sq, nr_cq;

	struct nvmev_admin_queue *admin_q;
	struct nvmev_submission_queue *sqes[NR_MAX_IO_QUEUE + 1];
	struct nvmev_completion_queue *cqes[NR_MAX_IO_QUEUE + 1];

	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *proc_read_times;
	struct proc_dir_entry *proc_write_times;
	struct proc_dir_entry *proc_io_units;
	struct proc_dir_entry *proc_stat;
	struct proc_dir_entry *proc_debug;
	struct proc_dir_entry *proc_deferred;
	struct proc_dir_entry *proc_ebpf;
#ifdef CONFIG_NVMEV_PROFILE_IO
	struct proc_dir_entry *proc_io_stat;

	atomic64_t io_stat_host_read;
	atomic64_t io_stat_host_write;
	atomic64_t io_stat_csd_read;
	atomic64_t io_stat_csd_write;
#endif

	unsigned long long *io_unit_stat;

	struct nvmev_ns *ns;
	int mdts;
	void *bpf_prog;

	struct crypto_comp *tfm;
};

struct nvmev_request {
	struct nvme_command *cmd;
	uint32_t sq_id;
	uint64_t nsecs_start;
};

struct nvmev_result {
	uint32_t status;
	uint64_t nsecs_nand_start;
	uint64_t nsecs_target;
	uint32_t early_completion;
	uint64_t wp; // only for zone append
};

enum nvmev_io_status {
	NVMEV_IO_STATUS_SUCCESS = 0,
	NVMEV_IO_STATUS_DEFERRED,
	NVMEV_IO_STATUS_ERROR,
};

struct nvmev_ns {
	uint32_t id;
	uint32_t csi;
	uint64_t size;
	void *mapped;

	/*conv ftl or zns or kv*/
	uint32_t nr_parts; // partitions
	void *ftls; // ftl instances. one ftl per partition

	/*io command handler*/
	enum nvmev_io_status (*proc_io_cmd)(struct nvmev_ns *ns, struct nvmev_request *req,
					    struct nvmev_result *ret);

	/*specific CSS io command identifier*/
	bool (*identify_io_cmd)(struct nvmev_ns *ns, struct nvme_command cmd);
	/*specific CSS io command processor*/
	unsigned int (*perform_io_cmd)(struct nvmev_ns *ns, struct nvme_command *cmd, uint32_t *status);
	void (*notify_io_cmd)(size_t size);
};

// VDEV Init, Final Function
struct nvmev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct nvmev_dev *vdev);

// OPS_PCI
bool nvmev_proc_bars(void);
bool NVMEV_PCI_INIT(struct nvmev_dev *dev);
void nvmev_signal_irq(int msi_index);

// OPS ADMIN QUEUE
void nvmev_proc_admin_sq(int new_db, int old_db);
void nvmev_proc_admin_cq(int new_db, int old_db);

// OPS I/O QUEUE
void NVMEV_IO_PROC_INIT(struct nvmev_dev *vdev);
void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev);
int nvmev_proc_io_sq(int qid, int new_db, int old_db);
void nvmev_proc_io_cq(int qid, int new_db, int old_db);
bool nvmev_proc_io_dq(int sqid);

void get_prp_data(struct nvme_command *cmd, void *buf, size_t size, bool from_host);

// Deferred Queue Operations
int nvmev_init_deferred_cache(void);
void nvmev_destroy_deferred_cache(void);
int nvmev_init_dq(struct nvmev_submission_queue *sq);
void nvmev_destroy_dq(struct nvmev_submission_queue *sq);

void NVMEV_CSD_PROC_INIT(struct nvmev_dev *vdev);
void NVMEV_CSD_PROC_FINAL(struct nvmev_dev *vdev);
#endif /* _LIB_NVMEV_H */
