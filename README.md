# Flash-Level Asynchronous Execution for In-Storage LSM-Tree Operations (FLAX)
This repository contains the source code and benchmarks needed to reproduce the key results in the FLAX paper.

FLAX is built on top of [NVMeVirt](https://github.com/snu-csl/nvmevirt/) and uses [ForestDB-Bench](https://github.com/ForestDB-KVStore/forestdb-bench) for evaluation.

FLAX consists of four major components:
- **CSD-Virt** — A standard-compliant NVMe CSD emulator that supports FLAX
- **Modified Linux kernel** (version 6.0.10)
- **Modified RocksDB** with compaction and GET offloading
- **Modified ForestDB-Bench** for running benchmarks

## System Requirements
FLAX requires three hardware features:
1. **At least six CPU cores**\
Your system must have at least six CPU cores for FLAX to function. In our evaluation setup, we used a total of 35 cores: 9 for NVMeVirt functions, 8 for NAND→SLM data transfer, 2 for CSD management, and 16 to emulate a 16-core ARM processor. We ran FLAX in a NUMA environment to avoid cross-interference. Specifically, NUMA node 0 was used for applications, while NUMA node 1 was dedicated to FLAX.

2. **CPU frequency scaling**\
FLAX's compute cores rely on CPU frequency scaling to emulate the ARM processor. First, determine which CPUs belong to which NUMA node (for example, with `numactl --hardware` or `lscpu`) and choose the cores you will use as compute cores. Then edit `src/perf.sh` to apply frequency scaling to those cores (For instance, in our setup, cores 200–215 are set to 1 GHz).  Before running `src/perf.sh`, install `cpufrequtils` (`sudo apt install cpufrequtils`). On Intel servers, you also need to add the `intel_pstate=disable` parameter to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`.



3. **Sufficient memory**\
Like NVMeVirt, FLAX requires a sufficient amount of host memory reserved for backend storage. In our evaluation, we reserved 350 GB of host memory and used 4 GB of it as SLM (compute memory).

### Tested Environment
- 2 x AMD EPYC 9754 128-Core Processor
- 768 GB DRAM
- Ubuntu 24.04.3 LTS
- Linux kernel version: 6.0.10

## Installation

### 0. Required packages
The following packages are required to build RocksDB and ForestDB-Bench. For more details, see `rocksdb/INSTALL.md` or `bench/INSTALL.md`.
```
sudo apt-get install cmake
sudo apt-get install cgroup-tools
sudo apt-get install libgflags-dev
sudo apt-get install libsnappy-dev
sudo apt-get install zlib1g-dev
sudo apt-get install libbz2-dev
sudo apt-get install liblz4-dev
sudo apt-get install libzstd-dev
```

### 1. Reserving physical memory
Part of the main memory must be reserved as storage for the emulated NVMe device. To reserve a chunk of physical memory, add the following option to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`.
```
GRUB_CMDLINE_LINUX="memmap=350G\\\$400G"
```
This reserves a 350 GB chunk of physical memory (out of 768 GB total) starting at a 400 GB offset. Adjust these values to match your available physical memory and the desired storage capacity.

After editing `/etc/default/grub`, update GRUB and reboot.

```
$ sudo update-grub
$ sudo reboot
```

### 2. Clone the repository
```
$ git clone https://github.com/jmattshim/FLAX
$ cd FLAX
$ git submodule update --init
```

### 3. Build the linux kernel
```
$ cd linux
$ sudo ./build_and_install.sh
(modify /etc/default/grub)
$ sudo update-grub
$ sudo reboot
```

### 4. Build and load FLAX
First, edit the `init_nvmev.sh` script for your environment. Below is the configuration used in our evaluation: reserved memory starts at a 400 GB offset, with 350 GB reserved in total and an SLM size of 4 GB.
```
sudo insmod nvmev.ko \
	memmap_start=400 memmap_size=358400 slm_size=4096 \
	cpus=128,131,134,137,140,143,146,149,152 \
	slm_cpus=155,158,161,164,167,170,173,176 \
	csd_cpus=182,185,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215
```
Then edit the `mount.sh` script to set the directory to mount. Our evaluation uses `/mnt/nvme` and `/mnt/ssd`.
```
sudo mkdir /mnt/nvme
sudo mkdir /mnt/ssd
```

Finally, build and load FLAX.
```
$ cd src
$ make
$ ./init_nvmev.sh
```
After loading, you may see some `dmesg` warnings. As long as they concern `__irq_msi_compose_msg` and `sudo nvme list` shows `CSL_Virt_SN_01`, the device is ready to use.

### 5. Build RocksDB
Before building RocksDB, edit lines 198–200 of `rocksdb/CMakeLists.txt` to
point to your local FLAX installation path.
```
$ cd rocksdb
$ mkdir build && cd build
$ cmake -DCMAKE_BUILD_TYPE=Release -DROCKSDB_BUILD_SHARED=ON ../
$ make
```

### 6. Build ForestDB-Bench
```
$ cd bench
$ mkdir build && cd build
$ cmake -DCMAKE_INCLUDE_PATH=/path/rocksdb/include -DCMAKE_LIBRARY_PATH=/path/rocksdb/build ../
$ make rocksdb_bench
$ mkdir results_YCSB		# dir for result collection
$ mkdir ycsb_logs			# dir for log collection
```

## Evaluations
Below are the guidelines for reproducing results mentioned in the paper from scratch. For claims and expectations of the artifact, please refer to [CLAIMS.md](CLAIMS.md).

### 0. Before running benchmarks
- For every experiment except §6.2 (Write Performance with FLAX), we used pre-loaded, stabilized DBs and copied them before each benchmark run.
   - This ensures a fair comparison across configurations and prevents load-phase compactions from occurring during the evaluation runs.
   - Because it also cuts the total evaluation time, we recommend preparing three base DBs: one for 128 B values, one for 4 KB values, and one for 128 B values with a Bloom filter.
   - We will explain how to load the base DBs below.
- For every experiment, we limited the available host memory using cgroups.
- For every experiment, we pin the host CPU cores for stable results. To modify these cores, three places must be modified
	- cgroup scripts (`set_cgroup_4G/25G.sh`)
	- `bench/bench/couch_bench.cc`: L571, L2035
	- `rocksdb/util/threadpool_imp.cc`: L379, L404
- We differentiate FLAX and host-managed CSD with the configuration in `src/csd_dispatcher.h`. Therefore, you must re-load CSD-Virt to enable host-managed CSD related evaluations.
	- `#define SUPPORT_ASYNC (0)` denotes host-managed CSD
	- `#define SUPPORT_ASYNC (1)` denotes FLAX
- We use mount directory `/mnt/nvme` and `/mnt/ssd`. After `mkdir`, change the directory ownership to user account.

### 1. Copy scripts
```
$ cd bench/build
$ ../scripts/copy-scripts.sh
```

### 2. Prepare base DBs
We recommend to use an additional SSD to mount and store base DBs.
In our setup, we used 960 GB SSD and mounted to `/mnt/ssd`.
```
$ ./prepare_db.sh 128
$ ./prepare_db.sh 4096
$ ./prepare_db.sh bloom
```

### 3. Write-only workload (Figure 7)
In this experiment, we test three configurations: Host, CSD, and FLAX.
This will run write-only workload with 16 B keys while varying value sizes from 128 B to 4 KB.
```
$ ./compaction_driver.sh host
$ ./compaction_driver.sh csd
$ ./compaction_driver.sh flax
```

### 4. Ablation study (Figure 8)
In this experiment, we test six configurations: Host, CSD, A, AB, ABC, FLAX.
This will run write-only workload with 16 B keys and 4 KB values.
Since we can reuse the host, CSD, and FLAX results from the previous evaluation, we only need to run A, AB, and ABC here. However, ABC requires modifying the RocksDB code, and since it shows only a small difference and does not affect our claims, we omit it. If you wish, you can edit `db/compaction/compaction_job.cc` to reduce the number of CRC offloading cores from 2 to 1.
```
$ ./ablation_driver.sh A
$ ./ablation_driver.sh AB
```

### 5. Read-only workload (Figure 9)
In this experiment, we test seven configurations: Host, CSD, FLAX-STD, FLAX-CMP, 8:2, 6:4, 4:6, 2:8.
This will run read-only workload with 16 B keys and 128 B values. Note that the CSD configuration is commented out in the script, since enabling it requires reloading CSD-Virt.
```
$ ./read-only_driver.sh 4G		# Figure 9(a)
$ ./read-only_driver.sh 25G		# Figure 9(b)
$ ./read-only_driver.sh bloom	# Figure 9(c)
$ ./read-only_driver.sh skew	# Figure 9(d)
```

### 6. Read-only workload with index block caching (Table 2)
In this experiment, we test three configurations: Host, CSD, FLAX.
We need to change the benchmark code to enable index block caching configuration.
```
$ cp ../wrappers/index_block_caching.cc ../wrappers/couch_rocksdb.cc
$ make rocksdb_bench
$ ./demand-load_driver.sh host
$ ./demand-load_driver.sh csd
$ ./demand-load_driver.sh flax
$ cp ../wrappers/default.cc ../wrappers/couch_rocksdb.cc # reset code
$ make rocksdb_bench
```

### 7. YCSB (Figure 10)
In this experiment, we test five configurations: Host, CSD, FLAX-C, FLAX-R, FLAX
This will run YCSB workloads A-F across 128 B values and 4 KB values
```
(128 B evaluation - Figure 10(a))
$ ./ycsb_driver.sh host 128
$ ./ycsb_driver.sh csd 128
$ ./ycsb_driver.sh compaction-only 128
$ ./ycsb_driver.sh read-only 128
$ ./ycsb_driver.sh flax 128

(4 KB evaluation - Figure 10(b))
$ ./ycsb_driver.sh host 4096
$ ./ycsb_driver.sh csd 4096
$ ./ycsb_driver.sh compaction-only 4096
$ ./ycsb_driver.sh read-only 4096
$ ./ycsb_driver.sh flax 4096
```
