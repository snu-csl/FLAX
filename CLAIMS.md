# Artifact Claims
## Notes
We evaluated FLAX on a server with 256 cores. Because each core on this machine shows relatively low `memcpy` performance, we had to dedicate more I/O worker and SLM worker cores to reach the performance required by the NVMeVirt's 970 PRO performance model. On machines with fewer cores, FLAX can still be evaluated with reduced number of workers. The benchmark numbers may not exactly match those reported in the paper, but the overall trends should be similar.

However, if FLAX is evaluated with fewer compute cores (we used 16), both YCSB workers must be adjusted to avoid queueing on the in-device compute cores. In this case, the overall benefit of FLAX may reduce, since neither the host nor the in-device cores use resources as aggressively.

## Expected results & claims
### 1. Write Performance (Section 6.2)
**Claims:**
- Host-managed CSD improves performance over Host, and FLAX improves performance over both CSD and Host.
- FLAX improves both throughput and tail latency.
- FLAX's performance advantage holds regardless of host memory.
- FLAX's lightweight but essential techniques enable it to fully exploit the CSD's potential.

**Expected results:**
- In Figure 7(a) and (c), FLAX outperforms both Host and CSD.
- In Figure 7(b), FLAX shows lower tail latency than both Host and CSD.
- In Figure 8, A outperforms CSD, AB performs similarly to A, ABC is slightly better than AB, and FLAX outperforms ABC.

### 2. Read Performance (Section 6.3)
**Claims:**
- Under tight host memory, FLAX shows higher read throughput, while CSD performs similarly to Host under a uniform access pattern.
- However, when host memory is sufficient or the access pattern is skewed, Host outperforms both CSD and FLAX.
- The COMPOUND command effectively improves performance over standard commands.
- Regardless of the configuration, FLAX outperforms CSD.

**Expected results:**
- In Figure 9(a), FLAX outperforms both Host and CSD.
- In Figure 9(b) and (d), Host outperforms both CSD and FLAX.
- In Figure 9, FLAX-STD consistently outperforms CSD, and FLAX-CMP consistently outperforms FLAX-STD.

### 3. YCSB Performance (Section 6.4)
**Claims:**
- FLAX outperforms both Host and CSD across all workloads except E, in terms of throughput, read latency, compaction latency, and tail latencies.
- FLAX outperforms FLAX-C and FLAX-R regardless of value sizes and workloads.

**Expected results:**
- In Figure 10, FLAX outperforms both Host and CSD.
- In Figure 10(a), FLAX-R shows comparable performance to FLAX.
- In Figure 10(b), FLAX-R shows comparable performance to FLAX in read-dominated workloads (YCSB B, C, D).
- In Figure 10(b), FLAX-C shows higher performance compared to Host and FLAX-R in write-dominated workloads (YCSB A, F).



