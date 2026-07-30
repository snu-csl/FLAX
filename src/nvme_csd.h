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

#ifndef _NVME_CSD_H
#define _NVME_CSD_H

#include "nvme.h"

#define COMPOUND_EXECUTE_LIMITED_SIZE (256 * 1024) // 256KB

enum nvme_opcode_csd {
	nvme_cmd_memory_read = 0x32,
	nvme_cmd_memory_write = 0x35,
	nvme_cmd_memory_management = 0x37,
	nvme_cmd_memory_copy = 0x3d,

	nvme_cmd_execute_program = 0x41,
	nvme_cmd_compound_execute_program = 0x43,

	nvme_admin_load_program = 0x45,
	nvme_test = 0x46,
	nvme_cmd_magic_compaction = 0x47,
	nvme_cmd_magic_read = 0x48,
	nvme_cmd_namespace_copy = 0x49,
};

// Status Code for CSD
enum nvme_status_code_csd {
	// I/O Command Specific Status Codes
	NVME_SC_INVALID_PROTECTION_INFO = 0x81,
	NVME_SC_CMD_SIZE_LIMIT_EXCEED = 0x83,
	NVME_SC_INCOMPATIBLE_NS = 0x85,
	NVME_SC_FAST_COPY_NOT_POSSIBLE = 0x86,
	NVME_SC_OVERLAPPING_IO_RANGE = 0x87,
	NVME_SC_NAMESPACE_NOT_REACHABLE = 0x88,

	// Computational programs specific status values
	NVME_SC_INSUFFICIENT_RESOURCE = 0x8a,
	NVME_SC_INVALID_MEM_NS = 0x8b,
	NVME_SC_INVALID_MEM_RANGE_SET = 0x8c,
	NVME_SC_INVALID_MEM_RANGE_SET_ID = 0x8d,
	NVME_SC_INVALID_PROGRAM_DATA = 0x8e,
	NVME_SC_INVALID_PROGRAM_INDEX = 0x8f,
	NVME_SC_INVALID_PROGRAM_TYPE = 0x90,
	NVME_SC_MAX_MEM_RANGE_EXCEEDED = 0x91,
	NVME_SC_MAX_MEM_RANGE_SET_EXCEEDED = 0x92,
	NVME_SC_MAX_PROGRAM_ACTIVATED = 0x93,
	NVME_SC_MAX_PROGRAM_BYTES_EXCEEDED = 0x94,
	NVME_SC_MEM_RANGE_SET_IN_USE = 0x95,
	NVME_SC_NO_PROGRAM = 0x96,
	NVME_SC_OVERLAPPING_MEM_RANGE = 0x97,
	NVME_SC_PROGRAM_NOT_ACTIVATED = 0x98,
	NVME_SC_PROGRAM_IN_USE = 0x99,
	NVME_SC_PROGRAM_INDEX_NOT_DOWNLOADABLE = 0x9a,
	NVME_SC_PROGRAM_TOO_BIG = 0x9b,
};

bool IS_CSD_PROCESS(int opcode);

struct nvme_sgl_desc {
	__le64 addr;
	__le32 length;
	__u8 rsvd[3];
	__u8 type;
};

struct nvme_keyed_sgl_desc {
	__le64 addr;
	__u8 length[3];
	__u8 key[4];
	__u8 type;
};

union nvme_data_ptr {
	struct {
		__le64 prp1;
		__le64 prp2;
	};
	struct nvme_sgl_desc sgl;
	struct nvme_keyed_sgl_desc ksgl;
};

enum nvme_memory_management_select {
	nvme_memory_range_create,
	nvme_memory_range_delete,
};

struct memory_region_entry {
	__le32 nsid; // 03:00 -> 4b
	__le32 nByte; // 07:04 -> 4b
	__le64 saddr; // 15:08 -> 8b
	__le64 rsvd1; // 23:16 -> 8b
	__le64 rsvd2; // 31:24 -> 8b
};

struct nvme_memory_management_command {
	__u8 opcode;
	__u8 flags;
	__u16 command_id; // 0
	__le32 nsid; // 1
	__u64 rsvd1; // 2 3
	__u64 rsvd2; // 4 5
	union nvme_data_ptr dptr; // 6 7 8 9
	__u8 sel;
	__u8 rsvd3;
	__u16 rsid; // 10
	__u32 numr; // 11
	__le32 cdw12;
	__le32 cdw13;
	__le32 cdw14;
	__le32 cdw15;
};

struct nvme_memory_command {
	__u8 opcode;
	__u8 flags;
	__u16 command_id; // 0
	__le32 nsid; // 1
	__u64 rsvd2; // 2 3
	__le64 metadata; // 4 5
	union nvme_data_ptr dptr; // 6 7 8 9
	__le64 sb; // 10, 11
	__le32 length; // *12
	__le32 cdw13;
	__le32 cdw14;
	__le32 cdw15;
};

struct nvme_memory_copy_command {
	__u8 opcode;
	__u8 flags;
	__u16 command_id; // 0
	__le32 nsid; // 1
	__le64 length; // *2 3
	__le64 metadata; // 4 5
	union nvme_data_ptr dptr; // 6 7 8 9
	__le64 sdaddr; // *10, 11
	__le16 format; // *12
	__le16 control; // *12
	__le32 control_flag; // 13
	__le32 cdw14;
	__le32 cdw15;
};

struct source_range_entry {
	__le32 nsid; // 03:00 -> 4b
	__le32 rsvd; // 07:04 -> 4b
	__le64 saddr; // 15:08 -> 8b
	__le32 nByte; // 19:16 -> 4b
	__le64 rsvd2; // 27:20 -> 8b
	__le32 rsvd3; // 31:28 -> 4b
};

struct ccsd_parameter {
	__le64 input_slm; //8
	__le64 output_slm; //8
	__le64 nByte; //8
	__le32 nEntry; //4
	struct source_range_entry sre[8]; // 40*8 320
	__le32 param_size;
	__le32 param[128];
};

#define MAX_CSSD_MAGIC_PARAM_SIZE (512)

struct ccsd_magic_parameter {
	__le32 param_size;			// 4
	__u8 param[MAX_CSSD_MAGIC_PARAM_SIZE];			// 4*64 = 256
	__le32 sre_1_count;			// 4
	__le32 sre_2_count;			// 4
	__le64 file_1_size;			// 8
	__le64 file_2_size;			// 8
	struct source_range_entry sre_1[100];	// 100 * 32 = 3200
	struct source_range_entry sre_2[100];	// 100 * 32 = 3200
};

#define MAX_MAGIC_READ_FILES (4)
#define MAX_MAGIC_READ_SRE_PER_FILE (25)

struct ccsd_magic_read_parameter {
	__le32 param_size;						// 4
	__u8 param[MAX_CSSD_MAGIC_PARAM_SIZE];				// 512
	__le32 num_files;						// 4
	__le32 sre_count[MAX_MAGIC_READ_FILES];				// 16
	__le64 file_size[MAX_MAGIC_READ_FILES];				// 32
	struct source_range_entry sre[MAX_MAGIC_READ_FILES][MAX_MAGIC_READ_SRE_PER_FILE]; // 4*25*32 = 3200
};

enum {
	NVME_MEMORY_COPY_LBA_FORMAT_1 = 1 << 9,
	NVME_MEMORY_COPY_LBA_FORMAT_2 = 2 << 9,
	NVME_MEMORY_COPY_BYTE_FORMAT_1 = 3 << 9,
};

struct nvme_load_program {
	__u8 opcode;
	__u8 flags;
	__u16 command_id; // 0
	__le32 nsid; // 1
	__le32 cdw2;
	__le32 cdw3;
	__le32 cdw4;
	__le32 cdw5;
	union nvme_data_ptr dptr; // 6 7 8 9
	__u16 pind; // 10, Program Index (PIND)
	__u8 ptype; // 10, Program Type
	__u8 resv2; // 10, Select (SEL), PIT, Reserved
	__le32 numb; // 11, the number of bytes to transfer
	__le64 pid; // 12 13, Program Identifier
	__le32 cdw14;
	__le32 cdw15;
};

enum {
	NVME_COMPUTE_SEL_LOAD = (0 << 0),
	NVME_COMPUTE_SEL_UNLOAD = (1 << 0),
	NVME_COMPUTE_PID_USED = (0 << 1),
	NVME_COMPUTE_PID_NOT_USED = (1 << 1)
};

struct nvme_execute_program_command {
	__u8 opcode;
	__u8 flags;
	__u16 command_id; // 0
	__le32 nsid; // 1
	__u16 pind; // 2, Program Index (PIND)
	__u16 rsid; // 2, Memory Range Set ID (RSID)
	__le32 cdw3;
	__le64 metadata; // 4 5
	union nvme_data_ptr dptr; // 6 7 8 9
	__le64 cparam1; // 10 11, Generic parameter data 1
	__le64 cparam2; // 12 13, Generic parameter data 2
	__le32 cdw14;
	__le32 cdw15; // (tmp) use for DLEN too
};

struct nvme_command_csd {
	union {
		struct nvme_common_command common;

		struct nvme_memory_command memory;
		struct nvme_memory_copy_command memory_copy;
		struct nvme_memory_management_command memory_management;

		struct nvme_load_program load_program;
		struct nvme_execute_program_command execute_program;

		struct nvme_memory_copy_command namespace_copy;
	};
};

#endif
