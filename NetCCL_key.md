# NetCCL 代码框架重构说明（聚焦三大设计要点）

本文聚焦 NetCCL 的核心工程化设计，围绕以下三点展开：
1) 抽象统一的工作流程，支持多种集合通信原语并复用同一数据路径；
2) 统一设计通信组状态，围绕 PSN（包序号）维护简化复杂度并保障正确性；
3) 提供显式资源分配与生命周期管理接口，隔离资源控制与数据面逻辑。

通过这三点，NetCCL 将“功能扩展成本、系统复杂度、运行时正确性”三者进行良好平衡。

---

## 1. 整体目标与系统分层

- 透明替换：对 PyTorch 用户透明，自动接管 torch.distributed 常用原语；
- 网内计算：P4 可编程交换机参与聚合，减少带宽/延迟；
- 智能回退：不满足 INC 条件时自动回退 NCCL；
- 性能优先：DPDK 用户态网络 + 批量收发 + NUMA/CUDA 亲和优化。

分层结构：
- 前端（frontend/）：Python 包装 + C++ API + 守护线程（异步内存拷贝、Future 完成态）；
- 后端（backend/）：主进程、负载均衡器、工作线程池（窗口/拥塞控制/重传）；
- 网络与交换机：DPDK 驱动、Flow Director、P4 交换机聚合。

---

## 2. 抽象统一的工作流程（多原语复用）

NetCCL 提供一个“统一的操作流水线”，不同集合原语通过 op_code 配置进入同一数据路径，极大减少新增原语的开发工作量。

统一抽象：
- 操作描述 op_t：携带 op_code、group、root、size、共享内存指针等；
- 分段与提交：前端固定分段（如 2MB）映射为子操作，生成 op_with_fut；
- 负载均衡：按工作线程数进行数据切片与聚合地址切分；
- 工作线程：滑动窗口发送、拥塞控制、ACK/重传、网内聚合/转发；
- 结果归并：工作线程完成后由负载均衡器汇总，前端 Future 标记完成。

统一工作流伪代码：
```python
def netccl_collective(op_code, tensor, group, root=None, async_op=False):
    # 1) 能力检查与智能回退
    if not check_inc_requirement(reduce_like=(op_code in [OP_ALL_REDUCE, OP_REDUCE]),
                                 tensor=tensor, op=op_code, group=group):
        return fallback(op_code, tensor, group, root, async_op)

    # 2) 可选量化（减少链路负载，交换机侧做 int32 聚合）
    qbuf, dequant = maybe_quantize(tensor, op_code, group)

    # 3) 分段并投递子操作（共享内存零拷贝路径）
    fut = post_collective_c(op_code=op_code, root=root or group.rank,
                            data=qbuf.ptr, size=qbuf.nbytes,
                            group=group, stream=tensor.stream, async_op=async_op)

    # 4) 完成与反量化
    if not async_op:
        fut.wait()
        dequant()
    return fut
```

C++ 提交（节选）：
```cpp
shared_ptr<PythonFutureWrapper> post_collective_c(op_code_t op_code, rank_t root,
    void *data, uint64_t size, group_t group, cudaStream_t stream, bool async_op) {
    for (uint64_t offset = 0; ; ) {
        uint32_t seg = FRONTEND_SEGMENT_SIZE;
        bool is_last = (size - offset <= seg);
        if (is_last) seg = size - offset;

        post_sub_collective(op_code, root, (char*)data + offset, seg,
                            group, stream, offset == 0, is_last,
                            /*generate_future*/ is_last && !async_op);

        offset += FRONTEND_SEGMENT_SIZE;
        if (!(offset < size)) break;
    }
    return last_future_if_needed;
}
```

负载均衡（节选）：
```cpp
// 将单次 collective 划分为 nworker 份，均衡到各工作线程
for (int i = 0; i < nworker; i++) {
    uint32_t sub_sz_i32 = size_in_int32 / nworker + ((uint32_t)i < size_in_int32 % nworker);
    uint32_t sub_sz = sub_sz_i32 * sizeof(int32_t);
    agg_size_t agg_len = op->group.agg_len / nworker;
    agg_size_t agg_addr = op->group.agg_addr + agg_len * i;

    sub_op_t *sub_op = new sub_op_t{ .op=op, .data=(char*)data+offset, .size=sub_sz,
                                     .agg_addr=agg_addr, .agg_len=agg_len };
    thread_ctx[i].qp->push_req(sub_op);
    offset += sub_sz;
}
```

工作线程关键点统一：
- gen_pkt/fill_pkt 对不同原语复用一套封包逻辑；
- need_tx/need_rx 决定载荷方向与大小；
- 对于聚合类原语（all_reduce/reduce），载荷按交换机聚合要求进行大端序转换；
- rate/窗口/重传策略对所有原语一致。

结论：新增一个集合原语通常只需
- 定义 op_code；
- 在 Python 前端完成覆盖/回退/量化策略；
- 若需要交换机侧聚合，定义 P4 逻辑与数据面字段解释；
- 其他传输/可靠性/性能优化自动复用。

---

## 3. 统一的通信组状态（以 PSN 维护为核心）

为保证语义正确与实现简洁，NetCCL 将“通信正确性”固化在“通信组状态”的统一设计中，核心是 PSN（包序号）的持续维护和窗口协同。

统一 group_t（节选）：
```cpp
struct group_t {
    group_id_t group_id;
    rank_t size, rank;
    agg_size_t agg_addr, agg_len;
    uint32_t switch_ip;
};
```

窗口与 PSN（节选）：
```cpp
struct window_t {
    ts_t last_ack_time_stamp;
    win_size_t to_send, to_ack;
    psn_t left_psn, right_psn;
    cc_t cc;                       // 拥塞控制状态
    pkt_state_t state[MAX_WIN_LEN];
} ALIGN64;
```

PSN 维护要点：
- 单组内 PSN 单调递增，贯穿“分段 + 多线程 + 重传”的全流程；
- 负载均衡切片不会破坏 PSN 全局顺序，线程以子区间内连续 PSN 发送；
- 组切换需满足“窗口清空或边界可切换”条件，禁止跨组乱序；
- 接收侧 ACK 按 PSN 驱动窗口滑动，保证重传定位简单、可靠；
- PSN 与拥塞控制、快速重传/全重传触发条件天然耦合，便于统一调度。

示例：生成数据包时分配 PSN（节选）
```cpp
if (!sub_op) {
    ctx->qp->front_req(sub_op);
    auto &psn = ctx->group_ctx[sub_op->op->group.group_id].psn;
    start_psn = psn;
    pkt_cnt = ceil_div(sub_op->size, PAYLOAD_SIZE);
    psn += pkt_cnt;            // 预留连续 PSN 区间
    pkt_sent = 0;
}
// 每个包的最终 PSN = start_psn + pkt_sent
```

正确性不变式：
- Invariant-1：同组内，PSN 严格单调且唯一；
- Invariant-2：窗口内任意未确认包可由 PSN 唯一定位，重传不跨组；
- Invariant-3：ACK 推动窗口左边界前进，任何越界 ACK 只计入冗余统计；
- Invariant-4：组切换仅发生在窗口边界或满足安全条件的时刻。

这套统一的组状态 + PSN 模型，使得：
- 多原语共享一套可靠传输与拥塞控制机制；
- 负载均衡、批量处理、重传都围绕 PSN 展开，逻辑简单、可验证；
- 跨模块（前端/后端/线程）对齐行为，降低维护成本。

---

## 4. 显式资源分配与管理接口

NetCCL 将“资源控制”从“数据面逻辑”中剥离，提供显式 API 与生命周期管理，便于部署、观测与调优。

接口分层：
- Python 侧：创建/销毁通信组、查询能力、控制量化/回退；
- 控制面（gRPC）：组成员注册、交换机聚合地址分配、策略下发；
- 数据面（C++）：共享内存、队列、DPDK 端口/队列、窗口资源、线程绑定。

示例（Python → 控制器）：
```python
def remote_create_group(local_ip_list):
    req = netccl_pb2.CreateGroupRequest()
    for ip in local_ip_list:
        req.Member.append(netccl_pb2.MemberInfo(IP=ip))
    resp = stub.CreateGroup(req)
    # 返回显式资源：GroupID、交换机聚合地址与长度、交换机IP等
    return resp.GroupID, resp.AggAddr, resp.AggLen, resp.SwitchIP
```

C++ 共享内存/队列（节选）：
```cpp
void shm_init() {
    segment = new managed_shared_memory(create_only, shm_name.data(), SHM_SIZE);
    shm = new(segment->allocate_aligned(sizeof(shm_t), alignof(shm_t))) shm_t();
    numa_interleave_memory(shm->copy_buffer.buffer,
                           sizeof(shm->copy_buffer.buffer),
                           numa_all_nodes_ptr);
    CUDA_CHECK(cudaHostRegister(shm->copy_buffer.buffer,
                                sizeof(shm->copy_buffer.buffer),
                                cudaHostRegisterDefault));
}
```

DPDK 资源绑定（节选）：
```cpp
void dpdk_init(string ip) {
    string dev = get_dev_by_ip(ip);
    string pci = get_pci_by_dev(dev);
    int node = get_socket_by_pci(pci);

    auto cpu_list = cpu_list_on_node(node, nthread);
    auto lcores_arg = make_lcore_binding(cpu_list); // (0-N)@(cpu ids)
    vector<string>eal_parameters = {"netccl", "--in-memory", "--lcores", lcores_arg};
    rte_eal_init(...);

    // 每工作线程独立 mempool/队列、独立 Flow Director 分类
}
```

资源生命周期与可观测：
- 组创建/销毁：GroupID、agg_addr/len 显式持有，禁止隐式共享；
- 线程/队列：与 NUMA/端口/队列一一绑定，避免跨 NUMA 抖动；
- 环境变量/指标输出：提供易用的部署/调试入口与性能可视化。

---

## 5. 数据路径与关键算法

发送路径（统一）：
1) 前端量化（可选）→ 分段 → 共享内存复制（D2H）；
2) 后端负载均衡切片 → 工作线程窗口发送；
3) 交换机聚合 → ACK 返回；
4) 前端 H2D（接收类）→ 反量化（可选）→ Future 完成。

接收路径（统一）：
- 根据 TOS + 自定义协议字段分类；
- PSN 定位重组/ACK 推进窗口；
- 按需进行大端序转换与聚合结果写回。

量化/反量化（节选）：
```python
max_abs = tensor.abs().max()
fallback_all_reduce(max_abs, dist.ReduceOp.SUM, group)
max_abs_value = max_abs.item()
if max_abs_value != 0:
    factor = 0x7fffff80 / max_abs_value
    tensor.mul_(factor)
    tensor_int32 = tensor.type(torch.int32)
# 传输 int32，交换机按 32 位进行聚合
# 完成后根据原语语义进行反量化
```

拥塞控制与重传（统一）：
- DCQCN 风格目标窗口跟随，ECN/丢包触发减小，AI/HI 增长；
- 快速重传：局部缺失 PSN 触发范围重发；
- 超时重传：窗口长时间不前进触发全重传与 cc 状态重置。

---

## 6. 多原语的最小改动扩展法

已支持原语：all_reduce、reduce、broadcast、reduce_scatter、all_gather。

新增原语步骤：
- 定义新 op_code 与 need_tx/need_rx 规则；
- 前端注册覆盖与回退策略，若需要量化配置转换策略；
- 如需交换机聚合，扩展 P4 与协议头语义；
- 复用统一的数据路径、窗口、拥塞控制与可靠性机制，无需重写传输栈。

示例：协议头（节选）
```cpp
struct netccl_inc_hdr {
    uint8_t  coll_type;     // op_code
    group_id_t group_id;
    rank_t  rank;
    rank_t  root;
    agg_size_t agg_addr;
    psn_t   psn;
} PACKED;
```

---

## 7. 接口与部署

Python 使用示例：
```python
import torch, torch.distributed as dist, netccl
dist.init_process_group(backend="nccl", enable_inc=True)
x = torch.randn(1<<24, device="cuda", dtype=torch.float32)
dist.all_reduce(x)   # NetCCL 自动接管与加速
```

关键环境变量：
```bash
export NETCCL_BIND_ADDR="192.168.1.100"
export NETCCL_CONTROLLER_ADDR="192.168.1.1:50051"
export NETCCL_WORKER_LOOPBACK=1   # 回环/压测选项
export NETCCL_PACKET_LOOPBACK=1
export NETCCL_FRONTEND_LOOPBACK=1
```

---

## 8. 总结

- 统一的工作流程抽象让多原语共享同一路径，极大降低开发与维护成本；
- 以 PSN 为核心的通信组状态统一设计，将正确性与复杂度控制在明确边界内；
- 显式资源分配/管理接口把资源与数据面解耦，强化可部署性与可观测性。

围绕这三大设计要点，NetCCL 在保证透明集成的同时，达成高性能、强鲁棒与易扩展的工程目标。