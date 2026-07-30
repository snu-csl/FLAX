#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/nvme_ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <ftw.h>
#include <time.h>
#include <malloc.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

#include <linux/fs.h>
#include <linux/fiemap.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include "CSDVirt.h"

int csdvirt_init_dev(struct CSDVirt* csdvirt, const char* path)
{
    csdvirt->m_fd = open(path, O_RDWR | O_DIRECT);
    if (csdvirt->m_fd <= 0) {
        printf("[Failed]\tOpen device file %d %d\n", csdvirt->m_fd, errno);
        return -1;
    } else {
        // printf("[success]\tDevice file opened (%s, %d)\n", path, m_fd);
    }

    sync();
    sync();
    sync();

    csdvirt->m_nsid = 1;
    csdvirt->m_cur_fd_index = 0;

    return 0;
}

void csdvirt_release_dev(struct CSDVirt* csdvirt)
{
    if (csdvirt->m_fd > 0) {
        close(csdvirt->m_fd);
    }
}

// Memory API
size_t csdvirt_alloc_memory(struct CSDVirt* csdvirt, size_t len)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

    struct memory_region_entry input_memory_entries;
    input_memory_entries.nByte = len;

    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_memory_management);
    m_nvmeCmd->cdw10 = (m_nvmeCmd->cdw10 & (~0xFF)) | (nvme_memory_range_create & 0xFF);
    m_nvmeCmd->cdw11 = 1;
    m_nvmeCmd->addr = (__u64) (&input_memory_entries);
    m_nvmeCmd->data_len = sizeof(struct memory_region_entry);
    int ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

    if (ret < 0)
    {
        printf("Memory allocation Fail : %d (%d)\n", ret, errno);

        return -1;
    }

    return input_memory_entries.saddr;
}

int csdvirt_release_memory(struct CSDVirt* csdvirt, size_t addr)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

    struct memory_region_entry input_memory_entries;
    input_memory_entries.saddr = addr;

    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_memory_management);
    m_nvmeCmd->cdw10 = (m_nvmeCmd->cdw10 & (~0xFF)) | (nvme_memory_range_delete & 0xFF);
    m_nvmeCmd->cdw11 = 1;
    m_nvmeCmd->addr = (__u64) (&input_memory_entries);
    m_nvmeCmd->data_len = sizeof(struct memory_region_entry);
    int ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

    if (ret < 0)
    {
        printf("Memory Release Fail : %d (%d)\n", ret, errno);

        return -1;
    }

    return ret;
}

// with filesystem api
int csdvirt_open(struct CSDVirt* csdvirt, const char* file_path)
{
    int fd = open(file_path, O_RDWR | O_DIRECT);
    if (fd == -1) {
        printf("Unable to open file: %s\n", file_path);
        exit(-1);
    }

    csdvirt->m_fd_map[csdvirt->m_cur_fd_index] = fd;
    if (csdvirt->m_cur_fd_index == MAX_OPEN_FILE_COUNT) {
        csdvirt->m_cur_fd_index = 0;
    }

    return csdvirt->m_cur_fd_index++;
}

int csdvirt_close(struct CSDVirt* csdvirt, int fd)
{
    int real_fd = csdvirt->m_fd_map[fd];
    if (real_fd == -1) {
        printf("Bad fd:%d", fd);
        exit(-1);
    }

    csdvirt->m_fd_map[fd] = INVALID_VALUE;
    close(real_fd);

    return 0;
}

size_t csdvirt_load(struct CSDVirt* csdvirt, int fd, __u64 device_buf, size_t count, off_t offset)
{
    int real_fd = csdvirt->m_fd_map[fd];
    
    size_t total_size = 0;
    size_t lba = 0;
    size_t len = 0;
    struct fiemap *fiemap = _getLBA(real_fd);
    size_t num_extents = fiemap->fm_mapped_extents;

    for (int i = 0; i < num_extents; i++) {
        lba = fiemap->fm_extents[i].fe_physical / 512;
        len = fiemap->fm_extents[i].fe_length;

        printf("[%d] lba:%lu(%lu), len:%lu\n", i, lba, lba / 8, len);
        csdvirt_load_raw(csdvirt, lba, device_buf, len);

        total_size += len;
        device_buf += len;
    }

    free(fiemap);

    if (total_size != count) {
        perror("File size is not match\n");
        return 1;
    }

    printf("\n\n##Returning load##\n\n");
    return 0;
}

// Raw Device api
size_t csdvirt_load_raw(struct CSDVirt* csdvirt, size_t lba, __u64 device_buf, size_t size)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

    int ret = 0;    
    struct source_range_entry source;

    source.nsid = 1;
    source.saddr = lba;
    source.nByte = size;

    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_memory_copy);
    m_nvmeCmd->addr = (__u64) &source;
    m_nvmeCmd->data_len = sizeof(struct source_range_entry);
    m_nvmeCmd->cdw2 = size & 0xFFFFFFFF;
    m_nvmeCmd->cdw3 = size >> 32;
    m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
    m_nvmeCmd->cdw11 = device_buf >> 32;
    m_nvmeCmd->cdw12 = 1;
    m_nvmeCmd->cdw15 = 1;

    // printf("%d %d %d\n", sourceLBA, sourceSize, destBuf);
    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
    if (ret < 0)
    {
        printf("loadInputData Fail : %d\n", ret);

        return -1;
    }
    
    return ret;
}

size_t csdvirt_namespace_copy(struct CSDVirt* csdvirt, __u64 offset, __u64 slm_offset, size_t size, int control_flag)
{
    if (control_flag == nvme_cmd_write)
        assert(size <= (size_t) MAX_IO_SPLIT_SIZE); // check MDTS for SLM->NVM

    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

    int ret = 0;    
    struct source_range_entry source;

    size_t device_lba = offset / 512;

    source.nsid = 1;
    source.saddr = device_lba;
    source.nByte = size;

    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_namespace_copy);
    m_nvmeCmd->addr = (__u64) &source;
    m_nvmeCmd->data_len = sizeof(struct source_range_entry);
    m_nvmeCmd->cdw2 = size & 0xFFFFFFFF;
    m_nvmeCmd->cdw3 = size >> 32;
    m_nvmeCmd->cdw10 = slm_offset & 0xFFFFFFFF;
    m_nvmeCmd->cdw11 = slm_offset >> 32;
    m_nvmeCmd->cdw12 = 1;
    m_nvmeCmd->cdw13 = control_flag;
    m_nvmeCmd->cdw15 = 1;

    // printf("%d %d %d\n", sourceLBA, sourceSize, destBuf);
    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
    if (ret < 0)
    {
        printf("namespaceCopy Fail : %d\n", ret);

        return -1;
    } else if (ret == 0) {
        ret = m_nvmeCmd->result;
    }
    
    return ret;
}

void* csdvirt_make_and_get_file_extent(struct CSDVirt* csdvirt, const char* file_path, size_t size)
{
    int fd = open(file_path, O_RDWR | O_CREAT | O_SYNC | O_DIRECT, 0666);
    if (fd == -1) {
        printf("Unable to open file: %s\n", file_path);
    }

    fallocate(fd, FALLOC_FL_ZERO_RANGE, 0, size);
    
    // char* buf = (char*) memalign(4096, size);
    // memset(buf, 1, size);
    // int ret = pwrite(fd, buf, size, 0);
    // free(buf);

    struct fiemap *fiemap = (struct fiemap *) _getLBA(fd);

    // for (int i = 0; i<fiemap->fm_mapped_extents; i++) {
    //     printf("Extent %d: %lu %lu\n", i, fiemap->fm_extents[i].fe_physical, fiemap->fm_extents[i].fe_length);
    // }

    close(fd);
    return (void*)fiemap;
}

size_t csdvirt_write_file(struct CSDVirt* csdvirt, void* fiemap, __u64 device_buf, size_t offset, size_t size)
{
    size_t lba;
    size_t io_size;

    /* csdvirt_namespace_copy assumes single SRE. Therefore, we do the calculation here */
    lba = _calculate_lba((struct fiemap*)fiemap, offset);
    io_size = _calculate_io_size((struct fiemap*)fiemap, offset, size);

    // printf("file offset:%lu, lba:%lu, size:%lu, device_buf:%lu\n", offset, lba, io_size, device_buf);

    return csdvirt_namespace_copy(csdvirt, lba, device_buf, io_size, 1);
}

size_t csdvirt_read_slm(struct CSDVirt* csdvirt, void* host_buf, __u64 device_buf, size_t size)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_memory_read);

    if (size > (size_t) MAX_IO_SPLIT_SIZE) {
        printf("I/O size error!\n");
    }

    int ret = 0;
    m_nvmeCmd->addr = (__u64) host_buf;
    m_nvmeCmd->data_len = size;
    m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
    m_nvmeCmd->cdw11 = device_buf >> 32;
    m_nvmeCmd->cdw12 = size;

    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

    if (ret < 0)
    {
        printf("csdvirt_read_slm Fail(%d) - ", errno);
        printf("Parameter : %lu %llu %lu\n", (size_t) host_buf, device_buf, size);

        return -1;
    }

    return ret;
}

size_t csdvirt_write_slm(struct CSDVirt* csdvirt, void* host_buf, __u64 device_buf, size_t size)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_memory_write);

    if (size > (size_t) MAX_IO_SPLIT_SIZE) {
        printf("I/O size error!\n");
    }

    int ret = 0;
    m_nvmeCmd->addr = (__u64) host_buf;
    m_nvmeCmd->data_len = size;
    m_nvmeCmd->cdw10 = device_buf & 0xFFFFFFFF;
    m_nvmeCmd->cdw11 = device_buf >> 32;
    m_nvmeCmd->cdw12 = size;

    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

    if (ret < 0)
    {
        printf("csdvirt_write_slm Fail(%d) - ", errno);
        printf("Parameter : %lu %llu %lu\n", (size_t) host_buf, device_buf, size);

        return -1;
    }

    return ret;
}

int csdvirt_execute(struct CSDVirt* csdvirt, int program_index, size_t input_buf, size_t output_buf, size_t len, void* user_params, size_t param_size, void* result)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;

    int ret = 0;    
    struct ccsd_parameter* param = (struct ccsd_parameter *) malloc(sizeof(struct ccsd_parameter));

    param->input_slm = input_buf;
    param->nByte = len;
    param->output_slm = output_buf;
    param->param_size = param_size;
    memcpy(&param->param, user_params, param_size);
    
    _initNvmeCommand(csdvirt, m_nvmeCmd, nvme_cmd_execute_program);
    m_nvmeCmd->addr = (__u64) param;
    m_nvmeCmd->data_len = sizeof(struct ccsd_parameter);
    m_nvmeCmd->cdw2 = program_index;
    m_nvmeCmd->cdw15 = sizeof(struct ccsd_parameter);
    
    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);
    free(param);

    if (ret < 0)
    {
        printf("[ioctl FAIL] execute : %d\n", errno);

        return -1;
    }

    *((__u32 *) result) = m_nvmeCmd->result;

    return ret;
}

void _initNvmeCommand(struct CSDVirt* csdvirt, struct nvme_passthru_cmd* nvme_cmd, int opcode)
{
    memset(nvme_cmd, 0, sizeof(struct nvme_passthru_cmd));
    nvme_cmd->nsid = csdvirt->m_nsid;
    nvme_cmd->opcode = opcode;
}

void* _getLBA(int fd)
{
    struct fiemap* fiemap = malloc(sizeof(struct fiemap));
    
    memset(fiemap, 0, sizeof(struct fiemap));
    fiemap->fm_start = 0;
    fiemap->fm_length = ~0;  // All extents
    fiemap->fm_flags = FIEMAP_FLAG_SYNC;

    if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
        perror("Failed to execute FS_IOC_FIEMAP ioctl");
        return NULL;
    }

    int num_extents = fiemap->fm_mapped_extents;
    size_t extents_size = sizeof(struct fiemap_extent) * num_extents;
    
    /* Resize fiemap to allow us to read in the extents */
	if ((fiemap = (struct fiemap*)realloc(fiemap, sizeof(struct fiemap) + extents_size)) == NULL) {
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

size_t _calculate_lba(struct fiemap* fiemap, size_t io_offset)
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

size_t _calculate_io_size(struct fiemap* fiemap, size_t io_offset, size_t io_size)
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

int nvm_write(struct CSDVirt* csdvirt, char* hostBuf, __u64 lba, size_t len)
{
    int m_fd = csdvirt->m_fd;
    struct nvme_passthru_cmd nvmeCmd;
    struct nvme_passthru_cmd *m_nvmeCmd = &nvmeCmd;
    _initNvmeCommand(csdvirt, m_nvmeCmd, 0x1);

    if (len > (size_t) MAX_IO_SPLIT_SIZE) {
        printf("I/O size error!\n");
    }

    int ret = 0;
    m_nvmeCmd->addr = (__u64) hostBuf;
    m_nvmeCmd->data_len = len;
    m_nvmeCmd->cdw10 = lba & 0xFFFFFFFF;
    m_nvmeCmd->cdw11 = lba >> 32;
    m_nvmeCmd->cdw12 = len / 512 - 1;

    ret = ioctl(m_fd, NVME_IOCTL_IO_CMD, m_nvmeCmd);

    if (ret < 0)
    {
        printf("nvm_write Fail(%u) - ", errno);
        printf("nvm_write Fail(%llu) - ", lba);

        return -1;
    }

    return ret;
}