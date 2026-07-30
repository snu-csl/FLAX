#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/nvme_ioctl.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <ftw.h>
#include <time.h>
#include <malloc.h>
#include <time.h>
#include <pthread.h>
#include <cassert>
#include <errno.h>
#include <limits.h>

#include <linux/fs.h>
#include <linux/fiemap.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "../csd_user_func.h"
#include "CSDVirt.hpp"

// Helper function to dynamically get CSD device path
std::string get_csd_device_path() {
	char resolved_path[PATH_MAX];

	// Use realpath to resolve the by-id alias to actual device path
	if (realpath(CSD_DEVICE_ALIAS, resolved_path) != NULL) {
		return std::string(resolved_path);
	}

	printf("Warning: Could not resolve CSD device path from alias\n");
	exit(1);
}

CSDVirt::CSDVirt()
{
	m_fd = INVALID_VALUE;
	m_nsid = INVALID_VALUE;

	for (int i = 0; i < MAX_OPEN_FILE_COUNT; i++) {
		m_fd_map[i] = INVALID_VALUE;
	}

	m_cur_fd_index = 0;
	m_debug_info = 0;
	csdvirt_init_stat();
}

CSDVirt::~CSDVirt()
{
}

int CSDVirt::loadMultiInputData(char *sourceBuf, __u64 destBuf, __u8 sourceNr)
{
	struct timespec start, end;
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	clock_gettime(CLOCK_REALTIME, &start);
	size_t copyLen = 0;
	struct source_range_entry *entry = (struct source_range_entry *)sourceBuf;
	for (int i = 0; i < sourceNr; i++) {
		copyLen += entry->nByte;
	}
	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_copy);
	m_nvmeCmd->addr = (__u64)sourceBuf;
	m_nvmeCmd->data_len = sizeof(struct source_range_entry) * sourceNr;
	m_nvmeCmd->cdw2 = copyLen & 0xFFFFFFFF;
	m_nvmeCmd->cdw3 = copyLen >> 32;
	m_nvmeCmd->cdw10 = destBuf & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = destBuf >> 32;
	m_nvmeCmd->cdw12 = sourceNr;

	int ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
	if (ret < 0) {
		printf("loadMultiInputData Fail : %d\n", ret);

		return -1;
	}

	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[LOAD_TIME].insert_log(_getNanoSecondTime(&start, &end), copyLen);

	return 0;
}

int CSDVirt::loadBPF(int bpf_fd)
{
	int ret = 0;
#if 0 // TODO
    _initNvmeCommand(m_nvmeCmd, nvme_admin_load_program);
    m_nvmeCmd->cdw10 = fd;
    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
#else
	int fd = -1;
	fd = openat(AT_FDCWD, "/proc/nvmev/ebpf", O_RDWR);
	if (fd < 0) {
		perror("openat");
	}
	ret = ::write(fd, &bpf_fd, sizeof(int));
#endif
	if (ret < 0) {
		printf("Load eBPF Function Fail : %d\n", ret);

		return -1;
	}

	return 0;
}

int CSDVirt::read(char *hostBuf, __u64 lba, size_t len)
{
	assert(len <= (size_t)MAX_IO_SPLIT_SIZE);
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	_initNvmeCommand(m_nvmeCmd, 0x2);
	m_nvmeCmd->addr = (__u64)hostBuf;
	m_nvmeCmd->data_len = len;
	m_nvmeCmd->cdw10 = lba & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = lba >> 32;
	m_nvmeCmd->cdw12 = len / 512 - 1;

	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
	if (ret < 0) {
		printf("Read Fail : %d\n", errno);

		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[READ_NVM_TIME].insert_log(_getNanoSecondTime(&start, &end), len);
	return ret;
}

int CSDVirt::write(char *hostBuf, __u64 lba, size_t len)
{
	assert(len <= (size_t)MAX_IO_SPLIT_SIZE);
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	_initNvmeCommand(m_nvmeCmd, 0x1);
	m_nvmeCmd->addr = (__u64)hostBuf;
	m_nvmeCmd->data_len = len;
	m_nvmeCmd->cdw10 = lba & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = lba >> 32;
	m_nvmeCmd->cdw12 = len / 512 - 1;

	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("Write Fail : %d, %d\n", m_fd, errno);

		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[WRITE_NVM_TIME].insert_log(_getNanoSecondTime(&start, &end), len);
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

int CSDVirt::csdvirt_init_dev(const char *path)
{
	m_fd = open(path, O_RDWR | O_DIRECT);
	if (m_fd <= 0) {
		printf("[Failed]\tOpen device file %d %d\n", m_fd, errno);
		return -1;
	} else {
		// printf("[success]\tDevice file opened (%s, %d)\n", path, m_fd);
	}

	m_nsid = 1;
	m_cur_fd_index = 0;

	csdvirt_init_stat();
	return 0;
}

void CSDVirt::csdvirt_release_dev(void)
{
	if (m_fd > 0) {
		close(m_fd);
	}
}

// Memory API
size_t CSDVirt::csdvirt_alloc_memory(size_t len)
{
	struct timespec start, end;
	struct nvme_passthru_cmd64 nvmeCmd;
	struct nvme_passthru_cmd64 *m_nvmeCmd = &nvmeCmd;
	clock_gettime(CLOCK_REALTIME, &start);

	struct memory_region_entry input_memory_entry;
	input_memory_entry.saddr = 0;

	memset(m_nvmeCmd, 0, sizeof(struct nvme_passthru_cmd64));
	m_nvmeCmd->nsid = m_nsid;
	m_nvmeCmd->opcode = nvme_cmd_memory_management;
	m_nvmeCmd->cdw10 = (m_nvmeCmd->cdw10 & (~0xFF)) | (nvme_memory_range_create & 0xFF);
	m_nvmeCmd->cdw11 = 1;
	m_nvmeCmd->cdw12 = len & 0xFFFFFFFF;
	m_nvmeCmd->cdw13 = len >> 32;
	m_nvmeCmd->cdw14 = m_debug_info;
	m_nvmeCmd->addr = (__u64)(&input_memory_entry);
	m_nvmeCmd->data_len = sizeof(struct memory_region_entry);
	int ret = ioctl(m_fd, NVME_IOCTL_IO64_CMD, m_nvmeCmd);

	if (ret != 0) {
		printf("Memory allocation Fail : %d (%d)\n", ret, errno);

		return -1;
	}

	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[MEMORY_ALLOC].insert_log(_getNanoSecondTime(&start, &end), len);

	return input_memory_entry.saddr;
}

int CSDVirt::csdvirt_release_memory(size_t addr)
{
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_management);
	m_nvmeCmd->cdw10 = (m_nvmeCmd->cdw10 & (~0xFF)) | (nvme_memory_range_delete & 0xFF);
	m_nvmeCmd->cdw11 = 1;
	m_nvmeCmd->cdw12 = addr & 0xFFFFFFFF;
	m_nvmeCmd->cdw13 = addr >> 32;
	m_nvmeCmd->addr = 0;
	m_nvmeCmd->data_len = 0;
	
	size_t saddr = ((__u64)m_nvmeCmd->cdw13 << 32) | m_nvmeCmd->cdw12;
	if (saddr != addr) {
		printf("Warning: Attempting to free invalid SLM address(%lu): %lu (%u %u)\n", 
				addr, saddr, m_nvmeCmd->cdw12, m_nvmeCmd->cdw13);
	}
	
	int ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("Memory Release Fail : %d (%d)\n", ret, errno);

		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[MEMORY_RELEASE].insert_log(_getNanoSecondTime(&start, &end), 0);

	return ret;
}

// with filesystem api
int CSDVirt::csdvirt_open(const char *file_path)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int fd = open(file_path, O_RDWR | O_DIRECT);
	if (fd == -1) {
		printf("Unable to open file: %s\n", file_path);
		exit(-1);
	}

	m_fd_map[m_cur_fd_index] = fd;
	if (m_cur_fd_index == MAX_OPEN_FILE_COUNT) {
		m_cur_fd_index = 0;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[CSD_VIRT_OPEN].insert_log(_getNanoSecondTime(&start, &end), 0);

	return m_cur_fd_index++;
}

int CSDVirt::csdvirt_close(int fd)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int real_fd = m_fd_map[fd];
	if (real_fd == -1) {
		printf("Bad fd:%d", fd);
		exit(-1);
	}

	m_fd_map[fd] = INVALID_VALUE;
	close(real_fd);

	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[CSD_VIRT_CLOSE].insert_log(_getNanoSecondTime(&start, &end), 0);

	return 0;
}

size_t CSDVirt::csdvirt_load(int fd, __u64 device_buf, size_t count, off_t offset)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int real_fd = m_fd_map[fd];

	struct fiemap *fiemap = (struct fiemap *)_getLBA(real_fd);

	size_t total_size = 0;
	size_t lba = 0;
	size_t len = 0;
	size_t num_extents = fiemap->fm_mapped_extents;
	for (int i = 0; i < num_extents; i++) {
		lba = fiemap->fm_extents[i].fe_physical / 512;
		len = fiemap->fm_extents[i].fe_length;
		csdvirt_load_raw(lba, device_buf, len);

		total_size += len;
		device_buf += len;
	}
	free(fiemap);

	if (total_size != count) {
		perror("File size is not match");
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[CSD_VIRT_LOAD_WITH_FS].insert_log(_getNanoSecondTime(&start, &end), count);

	return 0;
}

size_t CSDVirt::csdvirt_load_files(std::string *file_list, int nfiles, __u64 device_buf, size_t *actual_sizes)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int nentry = 0;
	size_t total_transfer_size = 0;
	struct source_range_entry *sources = (struct source_range_entry *)malloc(sizeof(struct source_range_entry) * 200);

	for (int file = 0; file < nfiles; file++) {
		/* Step 1. Extract file extent information */
		int fd = open(file_list[file].c_str(), O_RDWR | O_DIRECT | O_SYNC);
		if (fd == -1) {
			printf("Unable to open file: %s\n", file_list[file].c_str());
			exit(-1);
		}

		total_transfer_size += actual_sizes[file];

		struct fiemap *fiemap = (struct fiemap *)_getLBA(fd);

		size_t leftover_size = actual_sizes[file];
		size_t len = 0;
		size_t num_extents = fiemap->fm_mapped_extents;
		// printf("num_extents: %d, actual size: %d\n", num_extents, actual_sizes[file]);

		/* Step 2. Make SRE */
		for (int i = 0; i < num_extents; i++) {
			len = fiemap->fm_extents[i].fe_length;
			/* Handle 4K unalignment */
			if ((len > leftover_size)) {
				len = leftover_size;
			}

			sources[nentry].nsid = 1;
			sources[nentry].saddr = fiemap->fm_extents[i].fe_physical / 512;
			sources[nentry].nByte = len;
			// printf("nEntry: %d, sre LBA: %d, len: %d\n", nentry, sources[nentry].saddr, sources[nentry].nByte);
			nentry++;

			leftover_size -= len;

			if (leftover_size == 0) {
				break;
			}
		}
		free(fiemap);
		close(fd);
	}

	/* Step 3. SLMCPY */
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
	int ret = 0;

	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_copy);
	m_nvmeCmd->addr = (__u64)sources;
	m_nvmeCmd->data_len = sizeof(struct source_range_entry) * nentry;
	m_nvmeCmd->cdw2 = total_transfer_size & 0xFFFFFFFF;
	m_nvmeCmd->cdw3 = total_transfer_size >> 32;
	m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = device_buf >> 32;
	m_nvmeCmd->cdw12 = nentry;
	m_nvmeCmd->cdw15 = 1;

	// printf("%d %d %d\n", sourceLBA, sourceSize, destBuf);
	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
	if (ret < 0) {
		printf("loadInputData Fail : %d\n", ret);

		return -1;
	}

	free(sources);
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[CSD_VIRT_LOAD_WITH_FS].insert_log(_getNanoSecondTime(&start, &end), total_transfer_size);

	return 0;
}

size_t CSDVirt::csdvirt_load_part_of_file(std::string *file_list, int nfiles, __u64 device_buf, size_t *actual_sizes, size_t offset, int size)
{
	int nentry = 0;
	size_t total_transfer_size = 0;
	struct source_range_entry sources[200];

	// nfiles will always be 1
	// Align offset down to 512 bytes, adjust size to cover the original range
	size_t aligned_offset = (offset / 512) * 512;
	size_t aligned_size = ((size + (offset - aligned_offset) + 511) / 512) * 512;

	/* Step 1. Extract file extent information */
	int fd = open(file_list[0].c_str(), O_RDONLY);
	if (fd == -1) {
		printf("Unable to open file: %s\n", file_list[0].c_str());
		exit(-1);
	}

	static const int MAX_EXTENTS = 32;
	char fiemap_buf[sizeof(struct fiemap) + sizeof(struct fiemap_extent) * MAX_EXTENTS];
	struct fiemap *fiemap = (struct fiemap *)fiemap_buf;

	if (_getLBA_stack(fd, fiemap, MAX_EXTENTS) < 0) {
		close(fd);
		printf("Failed to get LBA information\n");
		return -1;
	}

	// Find the physical LBA that corresponds to the logical file offset
	size_t remaining_size = aligned_size;
	size_t file_offset_in_extent = aligned_offset;

	/* Step 2. Make SRE - extract LBA from offset and size */
	for (int i = 0; i < fiemap->fm_mapped_extents && remaining_size > 0; i++) {
		size_t extent_logical_start = fiemap->fm_extents[i].fe_logical;
		size_t extent_length = fiemap->fm_extents[i].fe_length;
		size_t extent_physical_start = fiemap->fm_extents[i].fe_physical;

		// Check if this extent contains our starting offset
		if (file_offset_in_extent >= extent_logical_start &&
		    file_offset_in_extent < extent_logical_start + extent_length) {
			// Calculate offset within this extent
			size_t offset_in_extent = file_offset_in_extent - extent_logical_start;
			size_t bytes_available_in_extent = extent_length - offset_in_extent;
			size_t bytes_to_read = (remaining_size < bytes_available_in_extent) ?
			                       remaining_size : bytes_available_in_extent;

			sources[nentry].nsid = 1;
			sources[nentry].saddr = (extent_physical_start + offset_in_extent) / 512;
			sources[nentry].nByte = bytes_to_read;

			total_transfer_size += bytes_to_read;
			nentry++;

			remaining_size -= bytes_to_read;
			file_offset_in_extent += bytes_to_read;
		}
	}

	close(fd);

	/* Step 3. SLMCPY */
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
	int ret = 0;

	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_copy);
	m_nvmeCmd->addr = (__u64)sources;
	m_nvmeCmd->data_len = sizeof(struct source_range_entry) * nentry;
	m_nvmeCmd->cdw2 = total_transfer_size & 0xFFFFFFFF;
	m_nvmeCmd->cdw3 = total_transfer_size >> 32;
	m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = device_buf >> 32;
	m_nvmeCmd->cdw12 = nentry;
	m_nvmeCmd->cdw15 = 1;

	// printf("%d %d %d\n", sourceLBA, sourceSize, destBuf);
	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
	if (ret < 0) {
		printf("loadInputData Fail : %d\n", ret);

		return -1;
	}

	return 0;
}

void *CSDVirt::csdvirt_make_and_get_file_extent(std::string output_file, size_t size)
{
	int fd = open(output_file.c_str(), O_RDWR | O_CREAT | O_SYNC | O_DIRECT, 0666);
	if (fd == -1) {
		printf("Unable to open file: %s\n", output_file.c_str());
	}

	fallocate(fd, FALLOC_FL_ZERO_RANGE, 0, size);

	// char* buf = (char*) memalign(4096, size);
	// memset(buf, 1, size);
	// int ret = pwrite(fd, buf, size, 0);
	// free(buf);

	struct fiemap *fiemap = (struct fiemap *)_getLBA(fd);

	// for (int i = 0; i<fiemap->fm_mapped_extents; i++) {
	//     printf("Extent %d: %lu %lu\n", i, fiemap->fm_extents[i].fe_physical, fiemap->fm_extents[i].fe_length);
	// }

	close(fd);
	return (void *)fiemap;
}

size_t CSDVirt::csdvirt_write_file(void *fiemap, __u64 device_buf, size_t offset, size_t *size)
{
	size_t lba;
	size_t io_size;

	/* csdvirt_namespace_copy assumes single SRE. Therefore, we do the calculation here */
	lba = _calculate_lba((struct fiemap *)fiemap, offset);
	io_size = _calculate_io_size((struct fiemap *)fiemap, offset, *size);
	*size = io_size; // due to csdvirt_namespace_copy assumming single SRC

	// printf("file offset:%lu, lba:%lu, size:%lu, device_buf:%lu\n", offset, lba, io_size, device_buf);

	return csdvirt_namespace_copy(lba, device_buf, io_size, 1);
}

void CSDVirt::csdvirt_truncate_file(std::string output_file, size_t size)
{
	int fd = open(output_file.c_str(), O_RDWR | O_SYNC);
	if (fd == -1) {
		printf("Unable to open file: %s\n", output_file.c_str());
	}

	ftruncate(fd, size);

	close(fd);
	return;
}

// Raw Device api
size_t CSDVirt::csdvirt_load_raw(size_t lba, __u64 device_buf, size_t size)
{
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	struct source_range_entry source;

	source.nsid = 1;
	source.saddr = lba;
	source.nByte = size;

	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_copy);
	m_nvmeCmd->addr = (__u64)&source;
	m_nvmeCmd->data_len = sizeof(struct source_range_entry);
	m_nvmeCmd->cdw2 = size & 0xFFFFFFFF;
	m_nvmeCmd->cdw3 = size >> 32;
	m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = device_buf >> 32;
	m_nvmeCmd->cdw12 = 1;
	m_nvmeCmd->cdw15 = 1;

	// printf("%d %d %d\n", sourceLBA, sourceSize, destBuf);
	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
	if (ret != 0) {
		printf("loadInputData Fail : %d\n", ret);

		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[LOAD_TIME].insert_log(_getNanoSecondTime(&start, &end), size);
	return ret;
}

size_t CSDVirt::csdvirt_namespace_copy(__u64 offset, __u64 slm_offset, size_t size, int control_flag)
{
	if (control_flag == nvme_cmd_write)
		assert(size <= (size_t)MAX_IO_SPLIT_SIZE); // check MDTS for SLM->NVM

	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	struct source_range_entry source;

	size_t device_lba = offset / 512;

	source.nsid = 1;
	source.saddr = device_lba;
	source.nByte = size;

	// printf("%d %d %d\n", device_lba, slm_offset, size);

	_initNvmeCommand(m_nvmeCmd, nvme_cmd_namespace_copy);
	m_nvmeCmd->addr = (__u64)&source;
	m_nvmeCmd->data_len = sizeof(struct source_range_entry);
	m_nvmeCmd->cdw2 = size & 0xFFFFFFFF;
	m_nvmeCmd->cdw3 = size >> 32;
	m_nvmeCmd->cdw10 = slm_offset & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = slm_offset >> 32;
	m_nvmeCmd->cdw12 = 1;
	m_nvmeCmd->cdw13 = control_flag;
	m_nvmeCmd->cdw15 = 1;

	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("csdvirt_namespace_copy Fail(%d) - ", errno);
		printf("Parameter : %lu %llu %lu\n", device_lba, slm_offset, size);

		return -1;
	} else if (ret == 0) {
		ret = m_nvmeCmd->result;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[NS_COPY_TIME].insert_log(_getNanoSecondTime(&start, &end), size);

	return ret;
}

size_t CSDVirt::csdvirt_read_slm(void *host_buf, __u64 device_buf, size_t size)
{
	if (size > (size_t)MAX_IO_SPLIT_SIZE) {
		printf("Request size : %lu, MDTS: %lu\n", size, MAX_IO_SPLIT_SIZE);
		assert(size <= (size_t)MAX_IO_SPLIT_SIZE);
	}
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_read);
	m_nvmeCmd->addr = (__u64)host_buf;
	m_nvmeCmd->data_len = size;
	m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = device_buf >> 32;
	m_nvmeCmd->cdw12 = size;

	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("csdvirt_read_slm Fail(%d) - ", errno);
		printf("Parameter : %lu %llu %lu\n", (size_t)host_buf, device_buf, size);

		return -1;
	} else if (ret == 0) {
		ret = m_nvmeCmd->result;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[READ_SLM_TIME].insert_log(_getNanoSecondTime(&start, &end), size);

	return ret;
}

size_t CSDVirt::csdvirt_write_slm(void *host_buf, __u64 device_buf, size_t size)
{
	assert(size <= (size_t)MAX_IO_SPLIT_SIZE);
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	_initNvmeCommand(m_nvmeCmd, nvme_cmd_memory_write);
	m_nvmeCmd->addr = (__u64)host_buf;
	m_nvmeCmd->data_len = size;
	m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
	m_nvmeCmd->cdw11 = device_buf >> 32;
	m_nvmeCmd->cdw12 = size;

	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("csdvirt_write_slm Fail(%d) - ", errno);
		printf("Parameter : %lu %llu %lu\n", (size_t)host_buf, device_buf, size);

		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[WRITE_SLM_TIME].insert_log(_getNanoSecondTime(&start, &end), size);

	return ret;
}

int CSDVirt::csdvirt_execute(int program_index, size_t input_buf, size_t output_buf, size_t len, void *user_params,
							 size_t param_size, void *result)
{
	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	int ret = 0;
	struct ccsd_parameter *param = (struct ccsd_parameter *)malloc(sizeof(struct ccsd_parameter));

	param->input_slm = input_buf;
	param->nByte = len;
	param->output_slm = output_buf;
	param->param_size = param_size;
	memcpy(&param->param, user_params, param_size);

	_initNvmeCommand(m_nvmeCmd, nvme_cmd_execute_program);
	m_nvmeCmd->addr = (__u64)param;
	m_nvmeCmd->data_len = sizeof(struct ccsd_parameter);
	m_nvmeCmd->cdw2 = program_index;
	m_nvmeCmd->cdw15 = sizeof(struct ccsd_parameter);

	ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
	free(param);

	if (ret < 0) {
		printf("[ioctl FAIL] execute : %d\n", errno);

		return -1;
	}

	*((__u32 *)result) = m_nvmeCmd->result;
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[EXECUTE_TIME].insert_log(_getNanoSecondTime(&start, &end), len);

	return ret;
}

int CSDVirt::csdvirt_magic_compaction(std::string *file_list_1, int nfiles_1, size_t *actual_sizes_1,
									  std::string *file_list_2, int nfiles_2, size_t *actual_sizes_2,
									  struct magic_params *_magic_params, void *result)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct ccsd_magic_parameter *magic_param;
	if (posix_memalign((void **)&magic_param, 4096, sizeof(struct ccsd_magic_parameter)) != 0) {
		printf("[posix_memalign FAIL] magic_compaction : %d\n", errno);
		return -1;
	}
	memset(magic_param, 0, sizeof(struct ccsd_magic_parameter));

	// Copy user params
	magic_param->param_size = MAX_CSSD_MAGIC_PARAM_SIZE;
	memcpy(&magic_param->param, _magic_params, MAX_CSSD_MAGIC_PARAM_SIZE);

	// Process first file list (first level inputs)
	int nentry_1 = 0;
	size_t total_size_1 = 0;
	for (int file = 0; file < nfiles_1; file++) {
		int fd = open(file_list_1[file].c_str(), O_RDWR | O_DIRECT | O_SYNC);
		if (fd == -1) {
			printf("Unable to open file: %s\n", file_list_1[file].c_str());
			free(magic_param);
			return -1;
		}

		total_size_1 += actual_sizes_1[file];

		struct fiemap *fiemap = (struct fiemap *)_getLBA(fd);
		size_t leftover_size = actual_sizes_1[file];
		size_t num_extents = fiemap->fm_mapped_extents;

		for (size_t i = 0; i < num_extents && nentry_1 < 100; i++) {
			size_t len = fiemap->fm_extents[i].fe_length;
			if (len > leftover_size) {
				len = leftover_size;
			}

			magic_param->sre_1[nentry_1].nsid = 1;
			magic_param->sre_1[nentry_1].saddr = fiemap->fm_extents[i].fe_physical / 512;
			magic_param->sre_1[nentry_1].nByte = len;
			nentry_1++;

			leftover_size -= len;
			if (leftover_size == 0) {
				break;
			}
		}
		free(fiemap);
		close(fd);
	}
	magic_param->sre_1_count = nentry_1;
	magic_param->file_1_size = total_size_1;

	// Process second file list (second level inputs)
	int nentry_2 = 0;
	size_t total_size_2 = 0;
	for (int file = 0; file < nfiles_2; file++) {
		int fd = open(file_list_2[file].c_str(), O_RDWR | O_DIRECT | O_SYNC);
		if (fd == -1) {
			printf("Unable to open file: %s\n", file_list_2[file].c_str());
			free(magic_param);
			return -1;
		}

		total_size_2 += actual_sizes_2[file];

		struct fiemap *fiemap = (struct fiemap *)_getLBA(fd);
		size_t leftover_size = actual_sizes_2[file];
		size_t num_extents = fiemap->fm_mapped_extents;

		for (size_t i = 0; i < num_extents && nentry_2 < 100; i++) {
			size_t len = fiemap->fm_extents[i].fe_length;
			if (len > leftover_size) {
				len = leftover_size;
			}

			magic_param->sre_2[nentry_2].nsid = 1;
			magic_param->sre_2[nentry_2].saddr = fiemap->fm_extents[i].fe_physical / 512;
			magic_param->sre_2[nentry_2].nByte = len;
			nentry_2++;

			leftover_size -= len;
			if (leftover_size == 0) {
				break;
			}
		}
		free(fiemap);
		close(fd);
	}
	magic_param->sre_2_count = nentry_2;
	magic_param->file_2_size = total_size_2;

	// Send magic command
	_initNvmeCommand(m_nvmeCmd, nvme_cmd_magic_compaction);
	m_nvmeCmd->addr = (__u64)magic_param;
	m_nvmeCmd->data_len = sizeof(struct ccsd_magic_parameter);
	m_nvmeCmd->cdw15 = sizeof(struct ccsd_magic_parameter);

	int ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("[ioctl FAIL] magic_compaction : %d\n", errno);
		free(magic_param);
		return -1;
	}

	// Read back the magic_info from the start of the returned buffer
	if (_magic_params != NULL) {
		struct magic_info *returned_info = (struct magic_info *)magic_param;
		_magic_params->info = *returned_info;
		if (returned_info->output_buf_size == 0) {
			printf("[ERROR] magic_compaction returned output_buf_size = 0, ret = %d\n", ret);
		}
	}

	free(magic_param);

	*((__u32 *)result) = m_nvmeCmd->result;
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[EXECUTE_TIME].insert_log(_getNanoSecondTime(&start, &end), total_size_1 + total_size_2);

	return ret;
}

int CSDVirt::csdvirt_magic_read(std::string *file_list, int nfiles, size_t *actual_sizes,
								struct magic_params *_magic_params, void *result)
{
	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	struct nvme_passthru_cmd nvmeCmd;
	struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

	struct ccsd_magic_read_parameter *magic_param;
	if (posix_memalign((void **)&magic_param, 4096, sizeof(struct ccsd_magic_read_parameter)) != 0) {
		printf("[posix_memalign FAIL] magic_read : %d\n", errno);
		return -1;
	}
	memset(magic_param, 0, sizeof(struct ccsd_magic_read_parameter));

	// Copy user params
	magic_param->param_size = MAX_CSSD_MAGIC_PARAM_SIZE;
	memcpy(&magic_param->param, _magic_params, MAX_CSSD_MAGIC_PARAM_SIZE);

	magic_param->num_files = nfiles;

	size_t total_size = 0;

	// Access read_params from the user-provided magic_params
	struct rocksdb_read_params *read_params = &_magic_params->read.read_params;

	// Process each file: get extents (LBAs) via fiemap
	for (int file = 0; file < nfiles && file < MAX_MAGIC_READ_FILES; file++) {
		int fd = open(file_list[file].c_str(), O_RDONLY);
		if (fd == -1) {
			printf("Unable to open file: %s\n", file_list[file].c_str());
			free(magic_param);
			return -1;
		}

		static const int MAX_EXTENTS = 32;
		char fiemap_buf[sizeof(struct fiemap) + sizeof(struct fiemap_extent) * MAX_EXTENTS];
		struct fiemap *fiemap = (struct fiemap *)fiemap_buf;

		if (_getLBA_stack(fd, fiemap, MAX_EXTENTS) < 0) {
			close(fd);
			printf("Failed to get LBA information\n");
			return -1;
		}
		size_t num_extents = fiemap->fm_mapped_extents;
		int nentry = 0;

		if (read_params->search_type[file] != 0) {
			// Partial load: only load the data block region
			size_t block_offset = read_params->block_offset[file];
			size_t block_size = read_params->block_size[file];
			size_t file_total_size = read_params->total_size[file];
			size_t aligned_offset = (block_offset / 512) * 512;
			size_t aligned_size = (((block_offset - aligned_offset) + block_size + 511) / 512) * 512;

			// Cap to file boundary: don't request more than what exists
			size_t max_available = file_total_size - aligned_offset;
			if (aligned_size > max_available) {
				aligned_size = ((max_available + 511) / 512) * 512;
			}

			magic_param->file_size[file] = aligned_size;
			total_size += aligned_size;

			size_t file_offset = aligned_offset;
			size_t remaining_size = aligned_size;

			for (size_t i = 0; i < num_extents && remaining_size > 0 &&
			     nentry < MAX_MAGIC_READ_SRE_PER_FILE; i++) {
				size_t extent_logical_start = fiemap->fm_extents[i].fe_logical;
				size_t extent_length = fiemap->fm_extents[i].fe_length;
				size_t extent_physical_start = fiemap->fm_extents[i].fe_physical;

				if (file_offset >= extent_logical_start &&
				    file_offset < extent_logical_start + extent_length) {
					size_t offset_in_extent = file_offset - extent_logical_start;
					size_t bytes_available = extent_length - offset_in_extent;
					size_t bytes_to_read = (remaining_size < bytes_available) ?
					                       remaining_size : bytes_available;

					magic_param->sre[file][nentry].nsid = 1;
					magic_param->sre[file][nentry].saddr =
						(extent_physical_start + offset_in_extent) / 512;
					magic_param->sre[file][nentry].nByte = bytes_to_read;
					nentry++;

					remaining_size -= bytes_to_read;
					file_offset += bytes_to_read;
				}
			}
		} else {
			// Full file load: load all extents
			magic_param->file_size[file] = actual_sizes[file];
			total_size += actual_sizes[file];

			size_t leftover_size = actual_sizes[file];
			for (size_t i = 0; i < num_extents && nentry < MAX_MAGIC_READ_SRE_PER_FILE; i++) {
				size_t len = fiemap->fm_extents[i].fe_length;
				if (len > leftover_size) {
					len = leftover_size;
				}

				magic_param->sre[file][nentry].nsid = 1;
				magic_param->sre[file][nentry].saddr = fiemap->fm_extents[i].fe_physical / 512;
				magic_param->sre[file][nentry].nByte = len;
				nentry++;

				leftover_size -= len;
				if (leftover_size == 0) {
					break;
				}
			}
		}
		magic_param->sre_count[file] = nentry;

		close(fd);
	}

	// Send magic read command
	_initNvmeCommand(m_nvmeCmd, nvme_cmd_magic_read);
	m_nvmeCmd->addr = (__u64)magic_param;
	m_nvmeCmd->data_len = sizeof(struct ccsd_magic_read_parameter);
	m_nvmeCmd->cdw15 = sizeof(struct ccsd_magic_read_parameter);

	int ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

	if (ret < 0) {
		printf("[ioctl FAIL] magic_read : %d\n", errno);
		free(magic_param);
		return -1;
	}

	// Read back magic_info from the returned buffer
	if (_magic_params != NULL) {
		struct magic_info *returned_info = (struct magic_info *)magic_param;
		_magic_params->info = *returned_info;
	}

	free(magic_param);

	*((__u32 *)result) = m_nvmeCmd->result;
	clock_gettime(CLOCK_REALTIME, &end);
	m_time_log[EXECUTE_TIME].insert_log(_getNanoSecondTime(&start, &end), total_size);

	return ret;
}

void CSDVirt::_initNvmeCommand(struct nvme_passthru_cmd *nvmeCmd, int opcode)
{
	memset(nvmeCmd, 0, sizeof(struct nvme_passthru_cmd));
	nvmeCmd->nsid = m_nsid;
	nvmeCmd->opcode = opcode;
	nvmeCmd->cdw14 = m_debug_info;
}

void *CSDVirt::_getLBA(int fd)
{
	struct fiemap *fiemap = (struct fiemap *)malloc(sizeof(struct fiemap));

	memset(fiemap, 0, sizeof(struct fiemap));
	fiemap->fm_start = 0;
	fiemap->fm_length = ~0; // All extents
	fiemap->fm_flags = 0;

	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		perror("Failed to execute FS_IOC_FIEMAP ioctl");
		return NULL;
	}

	int num_extents = fiemap->fm_mapped_extents;
	size_t extents_size = sizeof(struct fiemap_extent) * num_extents;

	/* Resize fiemap to allow us to read in the extents */
	if ((fiemap = (struct fiemap *)realloc(fiemap, sizeof(struct fiemap) + extents_size)) == NULL) {
		fprintf(stderr, "Out of memory allocating fiemap\n");
		return NULL;
	}
	memset(fiemap->fm_extents, 0, extents_size);
	fiemap->fm_extent_count = fiemap->fm_mapped_extents;

	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		perror("Failed to execute FS_IOC_FIEMAP second ioctl");
		return NULL;
	}

	return (void *)fiemap;
}

int CSDVirt::_getLBA_stack(int fd, struct fiemap *fiemap, int max_extents)
{
	memset(fiemap, 0, sizeof(struct fiemap) + sizeof(struct fiemap_extent) * max_extents);
	fiemap->fm_start = 0;
	fiemap->fm_length = ~0ULL;
	fiemap->fm_extent_count = max_extents;

	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		perror("Failed to execute FS_IOC_FIEMAP ioctl");
		return -1;
	}

	return 0;
}

size_t CSDVirt::_calculate_lba(struct fiemap *fiemap, size_t io_offset)
{
	int i = 0;
	size_t base_addr = 0;
	size_t local_offset = io_offset;

	if (io_offset == 0) {
		return fiemap->fm_extents[0].fe_physical;
	}

	for (i = 0; i < fiemap->fm_mapped_extents; i++) {
		base_addr = fiemap->fm_extents[i].fe_physical;
		if (local_offset < fiemap->fm_extents[i].fe_length) {
			break;
		}
		local_offset -= fiemap->fm_extents[i].fe_length;
	}

	return base_addr + local_offset;
}

size_t CSDVirt::_calculate_io_size(struct fiemap *fiemap, size_t io_offset, size_t io_size)
{
	int i = 0;
	size_t base_addr = 0;
	size_t local_offset = io_offset;

	if (io_offset == 0) {
		if (io_size < fiemap->fm_extents[0].fe_length) {
			return io_size;
		}
		return fiemap->fm_extents[0].fe_length;
	}

	for (i = 0; i < fiemap->fm_mapped_extents; i++) {
		base_addr = fiemap->fm_extents[i].fe_physical;
		if (local_offset < fiemap->fm_extents[i].fe_length) {
			break;
		}
		local_offset -= fiemap->fm_extents[i].fe_length;
	}

	if (local_offset + io_size > fiemap->fm_extents[i].fe_length) {
		return fiemap->fm_extents[i].fe_length - local_offset;
	}

	return io_size;
}

// Debug
void CSDVirt::csdvirt_init_stat()
{
	for (int i = TIME_TYPE_START; i < TIME_TYPE_COUNT; i++) {
		m_time_log[i].init();
	}
}

void CSDVirt::csdvirt_print_stat()
{
	printf("== CSDVirt Debugging Info (us) ==\n");
	printf(
		"load,execution,compound_exec,read_slm,read_nvm,write_nvm,memory_alloc,memory_release,file_open,file_close,load_with_fs,\n");

	unsigned long long total_time = 0L;
	for (int i = TIME_TYPE_START; i < TIME_TYPE_COUNT; i++) {
		total_time += m_time_log[i].totalTime;
	}

	if (total_time > 0) {
		for (int i = TIME_TYPE_START; i < TIME_TYPE_COUNT; i++) {
			printf("%.3lf,", (double)(m_time_log[i].totalTime) / 1000);
		}
		printf("\n");
	}
	printf("\n");
}

CSDVirt::TimeLog CSDVirt::TimeLog::insert_log(unsigned long long time, size_t size)
{
	this->totalTime = this->totalTime + time;
	if (this->maxTime < time) {
		this->maxTime = time;
	}
	if (this->minTime > time) {
		this->minTime = time;
	}
	this->size += size;

	return *this;
}

void CSDVirt::csdvirt_set_debugging_info(int value)
{
	m_debug_info = value;
}

unsigned long long CSDVirt::_getNanoSecondTime(struct timespec *tt1, struct timespec *tt2)
{
	unsigned long long start = (tt1->tv_sec * 1000000000L) + tt1->tv_nsec;
	unsigned long long end = (tt2->tv_sec * 1000000000L) + tt2->tv_nsec;

	return end - start;
}
