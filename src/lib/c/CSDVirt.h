#ifndef __CSD_VIRT_H__
#define __CSD_VIRT_H__
#include <linux/nvme_ioctl.h>
#include <time.h>
#include <pthread.h>

#include "../../nvme_csd.h"

#define __DEBUG__                   (0)

#define u32 __u32
#define u64 __u64
#define uint64 __u64

static const int INVALID_VALUE = -1;
static const size_t MAX_IO_SPLIT_SIZE = 256 * 1024;
static const int MAX_OPEN_FILE_COUNT = 8;

struct CSDVirt {
    int m_fd;
    int m_nsid;
    
    // with file system
    int m_cur_fd_index;
    int m_fd_map[8];
};

int csdvirt_init_dev(struct CSDVirt* csdvirt, const char* path);
void csdvirt_release_dev(struct CSDVirt* csdvirt);

// Memory API
size_t csdvirt_alloc_memory(struct CSDVirt* csdvirt, size_t len);
int csdvirt_release_memory(struct CSDVirt* csdvirt, size_t addr);

// with filesystem api
int csdvirt_open(struct CSDVirt* csdvirt, const char* file_path);
int csdvirt_close(struct CSDVirt* csdvirt, int fd);
size_t csdvirt_load(struct CSDVirt* csdvirt, int fd, __u64 device_buf, size_t count, off_t offset);
void* csdvirt_make_and_get_file_extent(struct CSDVirt* csdvirt, const char* file_path, size_t size);
size_t csdvirt_write_file(struct CSDVirt* csdvirt, void* fiemap, __u64 device_buf, size_t offset, size_t size);

// Raw Device api
size_t csdvirt_load_raw(struct CSDVirt* csdvirt, size_t lba, __u64 device_buf, size_t size);
size_t csdvirt_namespace_copy(struct CSDVirt* csdvirt, __u64 offset, __u64 slm_offset, size_t size, int control_flag);

size_t csdvirt_read_slm(struct CSDVirt* csdvirt, void* host_buf, __u64 device_buf, size_t size);
size_t csdvirt_write_slm(struct CSDVirt* csdvirt, void* host_buf, __u64 device_buf, size_t size);
int csdvirt_execute(struct CSDVirt* csdvirt, int program_index, size_t input_buf, size_t output_buf, size_t len, void* user_params, size_t param_size, void* result);

void _initNvmeCommand(struct CSDVirt* csdvirt, struct nvme_passthru_cmd* nvme_cmd, int opcode);
void* _getLBA(int fd);
size_t _calculate_lba(struct fiemap* fiemap, size_t offset);
size_t _calculate_io_size(struct fiemap* fiemap, size_t offset, size_t size);


int nvm_write(struct CSDVirt* csdvirt, char* hostBuf, __u64 lba, size_t len);

#endif