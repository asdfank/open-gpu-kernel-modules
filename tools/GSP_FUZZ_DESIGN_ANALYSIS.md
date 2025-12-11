# GSP RPC Fuzz 方案设计分析与优化建议

## 一、方案概述

本文档基于对 NVIDIA 驱动代码库的深入分析，对提出的 GSP RPC Fuzz 方案进行合理性检查和优化建议。

---

## 二、正常执行路径（P1–P5）分析

### 2.1 路径确认

根据代码分析，正常执行路径如下：

1. **P1 用户态 → 内核入口**
   - `ioctl(fd, NV_ESC_RM_CONTROL, &params)` → `nvidia_ioctl`
   - 位置：`kernel-open/nvidia/nv.c:2377`
   - 操作：参数大小验证、用户空间内存复制

2. **P2 RMAPI / ResServ**
   - `rmapiControlWithSecInfo` → `_rmapiRmControl`
   - 位置：`src/nvidia/src/kernel/rmapi/control.c:350`
   - 操作：IRQL 检查、锁绕过检查、参数一致性验证、参数大小匹配

3. **P3 对象多态分发**
   - `serverControl` → `resControl` → `resControlLookup` → `rmresControl_Prologue`
   - 位置：`src/nvidia/src/libraries/resserv/src/rs_server.c:1453`
   - 操作：句柄解析、虚表分发、命令查找、RPC 路由判断

4. **P4 RPC 路由 & 传输**
   - `rmresControl_Prologue_IMPL` → `NV_RM_RPC_CONTROL` → `rpcRmApiControl_GSP`
   - 位置：`src/nvidia/src/kernel/rmapi/resource.c:254`
   - 关键条件：`IS_FW_CLIENT(pGpu) && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL`
   - RPC 传输：`GspMsgQueueSendCommand` → `kgspSetCmdQueueHead_HAL`（触发中断）

5. **P5 固件端**
   - GSP 固件从共享内存队列读取 → 执行 → 写回状态队列
   - Host 轮询：`_kgspRpcRecvPoll` → `GspMsgQueueReceiveStatus`

**✅ 路径分析正确**

---

## 三、方案一（F1）：Hook 点选择分析

### 3.1 当前方案：Hook `rmresControl_Prologue_IMPL`

**优点**：
- ✅ 已完成参数验证和句柄解析，捕获的是"合法"的 RPC 调用
- ✅ 可以获取完整的上下文信息（`pGpu`, `hClient`, `hObject`, `cmd`, `params`）
- ✅ 适合作为种子生成器，保证种子质量

**缺点**：
- ❌ 只能捕获通过 RM 栈的 RPC 调用
- ❌ 如果未来有其他路径直接调用 RPC（如模式1），可能遗漏

### 3.2 优化建议：多级 Hook 策略

**建议采用分层 Hook 机制**：

#### Hook 点 1：`rmresControl_Prologue_IMPL`（保留）
- **用途**：种子录制、行为分析
- **位置**：`src/nvidia/src/kernel/rmapi/resource.c:289`
- **时机**：在调用 `NV_RM_RPC_CONTROL` 之前
- **记录内容**：
  ```c
  struct fuzz_seed_record {
      NvHandle hClient;
      NvHandle hObject;
      NvU32    cmd;
      NvU32    paramsSize;
      NvU8     params[FIXED_MAX_SIZE];  // 固定大小，避免动态分配
      NvU64    timestamp;
      NvU32    flags;  // ctrlFlags
  };
  ```

#### Hook 点 2：`rpcRmApiControl_GSP`（新增）
- **用途**：捕获所有 RPC 调用（包括模式1的直接调用）
- **位置**：`src/nvidia/src/kernel/vgpu/rpc.c:10977`
- **时机**：在参数序列化之后、发送到 GSP 之前
- **优势**：
  - ✅ 可以捕获所有 RPC 路径（包括绕过 RM 栈的）
  - ✅ 此时参数已经序列化，可以直接记录序列化后的 payload
  - ✅ 更接近实际的 GSP 交互

#### Hook 点 3：`GspMsgQueueSendCommand`（可选，深度监控）
- **用途**：监控共享内存队列操作、检测队列溢出
- **位置**：`src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c:456`
- **时机**：在写入共享内存之前
- **记录内容**：
  - 消息长度
  - 队列状态
  - 序列号

**推荐方案**：**同时使用 Hook 点 1 和 Hook 点 2**
- Hook 点 1：用于种子生成和语义分析
- Hook 点 2：用于全面覆盖和深度 fuzz

---

## 四、方案二（F2）：模式1 跳过检查分析

### 4.1 当前方案：模式1 直接调用 `NV_RM_RPC_CONTROL`

**代码路径**：
```c
// 模式1 伪代码
NV_ESC_GSP_FUZZ (mode=1) → nvidia_ioctl_gsp_fuzz
  → 直接调用 NV_RM_RPC_CONTROL(pGpu, hClient, hObject, cmd, params, paramsSize)
  → rpcRmApiControl_GSP → GspMsgQueueSendCommand → GSP FW
```

### 4.2 跳过的检查分析

根据代码分析，模式1 会跳过以下检查：

#### ✅ **可以安全跳过的检查**：

1. **RMAPI 层检查**（`_rmapiRmControl`）：
   - ❌ NULL 命令检查
   - ❌ IRQL 级别验证
   - ❌ 锁绕过验证
   - ❌ 参数指针和大小一致性验证（在 RMAPI 层）
   - **影响**：可能发送非法命令或参数，但这是 fuzz 的目的

2. **ResServ 层检查**（`serverControl`）：
   - ❌ `hClient` 存在性验证
   - ❌ `hObject` 存在性验证
   - ❌ 资源有效性验证
   - **影响**：可能使用无效句柄，但 GSP 固件会自己验证

3. **导出表检查**（`resControlLookup`）：
   - ❌ 命令支持性验证（`objGetExportedMethodDef`）
   - ❌ 参数大小与导出表匹配验证
   - **影响**：可能发送未定义的命令，但 GSP 固件会拒绝

#### ⚠️ **必须保留的检查**：

1. **GPU 锁检查**（`rpcRmApiControl_GSP:11012`）：
   ```c
   if (!rmDeviceGpuLockIsOwner(pGpu->gpuInstance)) {
       // 必须获取锁，否则共享内存可能被并发修改
       rmGpuGroupLockAcquire(...);
   }
   ```
   - **原因**：保护共享内存队列，防止并发写入导致数据损坏
   - **建议**：**必须保留**

2. **参数序列化检查**（`rpcRmApiControl_GSP:11059`）：
   ```c
   status = serverSerializeCtrlDown(pCallContext, cmd, &pParamStructPtr, &paramsSize, &resCtrlFlags);
   if (status != NV_OK)
       goto done;
   ```
   - **原因**：确保参数格式正确，否则 RPC 协议层会失败
   - **建议**：**可以放宽，但需要基本格式检查**

3. **RPC 消息大小检查**（`rpcRmApiControl_GSP:11097`）：
   ```c
   if (total_size > pRpc->maxRpcSize) {
       // 需要分片传输
   }
   ```
   - **原因**：防止消息过大导致队列溢出
   - **建议**：**必须保留，但可以提高上限用于 fuzz**

4. **句柄有效性基本检查**：
   - **建议**：虽然跳过 ResServ 的详细验证，但应该检查：
     - `hClient != 0`（避免空指针解引用）
     - `hObject != 0`（同上）
     - `pGpu != NULL`（必须）

### 4.3 优化建议

**建议在模式1 中添加最小安全检查**：

```c
// 模式1 的最小安全检查
static NV_STATUS gsp_fuzz_mode1_sanity_check(
    OBJGPU *pGpu,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    NvU32 paramsSize
) {
    // 1. 基本指针检查
    if (pGpu == NULL)
        return NV_ERR_INVALID_OBJECT;
    
    // 2. 句柄非零检查（避免明显的空指针）
    if (hClient == 0 || hObject == 0)
        return NV_ERR_INVALID_OBJECT;
    
    // 3. 参数大小上限检查（防止栈溢出）
    if (paramsSize > GSP_FUZZ_MAX_PARAMS_SIZE)  // 例如 64KB
        return NV_ERR_INVALID_ARGUMENT;
    
    // 4. 命令 ID 范围检查（避免明显的无效值）
    if (cmd == 0 || cmd == 0xFFFFFFFF)
        return NV_ERR_INVALID_ARGUMENT;
    
    return NV_OK;
}
```

**关键点**：
- ✅ 保留 GPU 锁检查（必须）
- ✅ 保留 RPC 消息大小检查（必须，但可提高上限）
- ✅ 添加最小句柄和参数检查（防止明显的崩溃）
- ❌ 跳过语义层面的检查（让 GSP 固件自己处理）

---

## 五、代码覆盖率评估方案

### 5.1 问题：无法直接获取固件代码覆盖率

**原因**：
- GSP 固件是闭源二进制
- 无法插桩或修改固件代码
- 无法使用传统覆盖率工具（如 gcov、LLVM Coverage）

### 5.2 间接覆盖率评估方案

#### 方案 A：RPC 响应时间分析

**原理**：不同代码路径可能有不同的执行时间

**实现**：
```c
// 在 Hook 点记录时间戳
struct fuzz_coverage_metric {
    NvU32 cmd;
    NvU64 send_time;
    NvU64 recv_time;
    NvU64 latency_us;  // 响应延迟
    NV_STATUS status;
};

// 分析：
// - 不同 cmd 的延迟分布
// - 相同 cmd 不同 params 的延迟差异
// - 异常延迟可能表示触发了新路径
```

**优点**：
- ✅ 无需修改固件
- ✅ 可以检测到执行时间差异

**缺点**：
- ❌ 无法精确定位覆盖的代码行
- ❌ 可能误报（相同路径在不同负载下延迟不同）

#### 方案 B：GSP 日志分析

**原理**：GSP 固件会输出日志到共享内存，分析日志内容可以推断执行路径

**实现**：
```c
// 在每次 RPC 后读取 GSP 日志
NV_STATUS gsp_fuzz_collect_logs(OBJGPU *pGpu, KernelGsp *pKernelGsp) {
    // 调用 kgspDumpGspLogs 或类似函数
    // 分析日志中的函数名、错误码、状态信息
    // 建立 cmd -> log_pattern 的映射
}
```

**位置**：
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:1159` - `kgspDumpGspLogs`
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c:1160` - 日志转储

**优点**：
- ✅ 可以获取函数级别的覆盖信息
- ✅ 可以检测错误路径

**缺点**：
- ❌ 需要解析日志格式
- ❌ 可能不完整（某些路径不输出日志）

#### 方案 C：RPC 响应状态码分析

**原理**：不同代码路径可能返回不同的状态码

**实现**：
```c
// 记录每个 cmd + params 组合的响应状态码
struct fuzz_status_coverage {
    NvU32 cmd;
    NV_STATUS status;
    NvU32 status_count[NV_STATUS_MAX];  // 统计不同状态码的出现次数
};

// 分析：
// - 新出现的状态码可能表示触发了新路径
// - 状态码分布可以推断路径覆盖情况
```

**优点**：
- ✅ 简单易实现
- ✅ 可以检测错误路径

**缺点**：
- ❌ 粒度较粗
- ❌ 可能无法区分相似路径

#### 方案 D：共享内存状态分析

**原理**：监控共享内存队列的状态变化

**实现**：
```c
// 在每次 RPC 前后检查共享内存状态
struct fuzz_memory_coverage {
    NvU32 queue_head_before;
    NvU32 queue_tail_before;
    NvU32 queue_head_after;
    NvU32 queue_tail_after;
    NvU32 checksum_before;
    NvU32 checksum_after;
};

// 分析：
// - 队列状态变化可以推断 GSP 的处理模式
// - 异常状态可能表示触发了边界条件
```

**优点**：
- ✅ 可以检测队列溢出、数据损坏等问题

**缺点**：
- ❌ 无法直接反映代码覆盖率

### 5.3 推荐方案：组合使用

**建议采用多维度评估**：

1. **主要指标**：RPC 响应时间分析（方案 A）
2. **辅助指标**：GSP 日志分析（方案 B）
3. **验证指标**：RPC 响应状态码分析（方案 C）

**实现框架**：
```c
typedef struct {
    // 时间维度
    NvU64 latency_us;
    
    // 日志维度
    char log_snippet[256];  // 日志片段
    
    // 状态维度
    NV_STATUS status;
    
    // 内存维度
    NvU32 queue_state;
    
    // 综合评分
    NvU32 coverage_score;  // 基于多维度计算
} fuzz_coverage_metric_t;
```

---

## 六、健康检查机制设计

### 6.1 现有健康检查机制

根据代码分析，驱动中已有以下健康检查：

#### A. `kgspHealthCheck_HAL`（主要健康检查）

**位置**：`src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c:1022`

**功能**：
- ✅ 检查 CrashCat 报告（固件崩溃报告）
- ✅ 检查 Watchdog 报告（超时报告）
- ✅ 标记致命错误（`pKernelGsp->bFatalError = NV_TRUE`）
- ✅ 触发 GPU 重置（`gpuMarkDeviceForReset`）

**调用时机**：
- GSP 中断服务例程（`kgspService_TU102:1160`）
- GSP 启动失败后（`kgspBootstrap:4025`）
- RPC 超时后（间接调用）

#### B. RPC 超时检测

**位置**：`src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:2167`

**功能**：
- ✅ 检测 RPC 响应超时（`_kgspRpcIncrementTimeoutCountAndRateLimitPrints`）
- ✅ 连续 3 次超时后触发 GPU 重置（`RPC_TIMEOUT_GPU_RESET_THRESHOLD`）
- ✅ 记录 RPC 历史到 NOCAT

**超时阈值**：
- 单个 RPC 超时：由 `gpuSetTimeout` 设置（通常几秒）
- 连续超时阈值：`RPC_TIMEOUT_GPU_RESET_THRESHOLD = 3`

#### C. 消息队列健康检查

**位置**：`src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c:377`

**功能**：
- ✅ 在发送命令前检查 GSP 健康状态
- ✅ 如果 `kgspHealthCheck_HAL` 返回 `NV_FALSE`，返回 `NV_ERR_RESET_REQUIRED`

### 6.2 Fuzz 专用健康检查扩展

#### 扩展 1：轻量级健康检查（每次 RPC 后）

**目的**：快速检测 GSP 是否还活着，避免长时间等待

**实现**：
```c
static NvBool gsp_fuzz_lightweight_health_check(OBJGPU *pGpu, KernelGsp *pKernelGsp) {
    // 1. 检查致命错误标志
    if (pKernelGsp->bFatalError)
        return NV_FALSE;
    
    // 2. 检查 GPU 是否被标记为重置
    if (pGpu->getProperty(pGpu, PDB_PROP_GPU_IS_LOST))
        return NV_FALSE;
    
    // 3. 检查 RPC 超时计数
    OBJRPC *pRpc = GPU_GET_RPC(pGpu);
    if (pRpc && pRpc->timeoutCount >= RPC_TIMEOUT_GPU_RESET_THRESHOLD)
        return NV_FALSE;
    
    // 4. 快速检查共享内存队列状态（可选）
    // 如果队列完全卡住，可能表示 GSP hang
    
    return NV_TRUE;
}
```

**调用时机**：
- 每次 RPC 发送后（在等待响应前）
- 每次 RPC 接收后（在处理响应前）

#### 扩展 2：深度健康检查（定期或异常后）

**目的**：全面检查 GSP 状态，用于崩溃分析

**实现**：
```c
typedef struct {
    NvBool bFatalError;
    NvBool bWatchdogTriggered;
    NvU32  timeoutCount;
    NvU32  crashReportCount;
    NvU64  lastResponseTime;
    NvBool bQueueHealthy;
} gsp_fuzz_health_status_t;

static NV_STATUS gsp_fuzz_deep_health_check(
    OBJGPU *pGpu,
    KernelGsp *pKernelGsp,
    gsp_fuzz_health_status_t *pStatus
) {
    // 1. 调用官方健康检查
    pStatus->bFatalError = !kgspHealthCheck_HAL(pGpu, pKernelGsp);
    
    // 2. 检查 Watchdog 报告
    pStatus->bWatchdogTriggered = (pKernelGsp->pWatchdogReport != NULL);
    
    // 3. 检查 RPC 超时计数
    OBJRPC *pRpc = GPU_GET_RPC(pGpu);
    if (pRpc) {
        pStatus->timeoutCount = pRpc->timeoutCount;
        pStatus->lastResponseTime = pRpc->rpcHistory[pRpc->rpcHistoryCurrent].ts_start;
    }
    
    // 4. 检查队列状态
    MESSAGE_QUEUE_INFO *pMQI = pRpc->pMessageQueueInfo;
    if (pMQI) {
        // 检查队列是否可写
        pStatus->bQueueHealthy = (msgqTxGetWriteBuffer(pMQI->hQueue, 0) != NULL);
    }
    
    // 5. 检查 GPU 状态
    pStatus->bGpuLost = pGpu->getProperty(pGpu, PDB_PROP_GPU_IS_LOST);
    
    return NV_OK;
}
```

**调用时机**：
- 每次检测到异常后（超时、错误状态码等）
- 定期检查（例如每 100 次 RPC）
- Fuzz 会话结束前

#### 扩展 3：健康检查回调机制

**目的**：允许用户态 fuzzer 注册健康检查回调

**实现**：
```c
// 在 NV_ESC_GSP_FUZZ 中添加健康检查模式
#define GSP_FUZZ_MODE_HEALTH_CHECK  2

struct gsp_fuzz_health_check_req {
    NvU32 mode;  // GSP_FUZZ_MODE_HEALTH_CHECK
    // 输出
    gsp_fuzz_health_status_t status;
};

// 用户态可以定期调用
ioctl(fd, NV_ESC_GSP_FUZZ, &health_check_req);
```

### 6.3 健康检查集成到 Fuzz 流程

**建议的 Fuzz 循环**：

```c
for (each test case) {
    // 1. 发送前检查
    if (!gsp_fuzz_lightweight_health_check(pGpu, pKernelGsp)) {
        log_crash(test_case);
        trigger_gpu_reset_if_needed();
        continue;
    }
    
    // 2. 发送 RPC
    status = send_rpc(...);
    
    // 3. 等待响应（带超时）
    response = wait_for_response(timeout);
    
    // 4. 接收后检查
    if (!gsp_fuzz_lightweight_health_check(pGpu, pKernelGsp)) {
        log_crash(test_case);
        trigger_gpu_reset_if_needed();
        continue;
    }
    
    // 5. 记录覆盖率指标
    record_coverage_metric(...);
    
    // 6. 定期深度检查
    if (rpc_count % 100 == 0) {
        gsp_fuzz_deep_health_check(...);
    }
}
```

---

## 七、方案优化总结

### 7.1 Hook 点优化

| 方案 | 当前 | 优化后 |
|------|------|--------|
| Hook 点 1 | `rmresControl_Prologue` | ✅ 保留（种子生成） |
| Hook 点 2 | 无 | ✅ 新增 `rpcRmApiControl_GSP`（全面覆盖） |
| Hook 点 3 | 无 | ⚪ 可选 `GspMsgQueueSendCommand`（深度监控） |

### 7.2 模式1 安全检查优化

| 检查项 | 当前 | 优化后 |
|--------|------|--------|
| GPU 锁 | ❌ 未明确 | ✅ **必须保留** |
| 参数序列化 | ❌ 未明确 | ⚠️ **可以放宽，但需基本格式检查** |
| RPC 消息大小 | ❌ 未明确 | ✅ **必须保留，但可提高上限** |
| 句柄有效性 | ❌ 完全跳过 | ✅ **添加最小检查**（非零、非空） |
| 语义检查 | ❌ 完全跳过 | ✅ **保持跳过**（让 GSP 处理） |

### 7.3 覆盖率评估优化

| 方案 | 优先级 | 实现难度 |
|------|--------|----------|
| RPC 响应时间分析 | 🔴 高 | 低 |
| GSP 日志分析 | 🟡 中 | 中 |
| RPC 响应状态码分析 | 🟡 中 | 低 |
| 共享内存状态分析 | 🟢 低 | 中 |

**推荐**：组合使用方案 A + B + C

### 7.4 健康检查优化

| 检查类型 | 频率 | 用途 |
|----------|------|------|
| 轻量级健康检查 | 每次 RPC | 快速检测崩溃 |
| 深度健康检查 | 每 100 次 RPC 或异常后 | 全面状态分析 |
| 健康检查回调 | 用户态主动调用 | 外部监控集成 |

---

## 八、实施建议

### 8.1 分阶段实施

**阶段 1：基础 Hook 和种子生成**
- 实现 Hook 点 1（`rmresControl_Prologue`）
- 实现种子录制功能
- 实现轻量级健康检查

**阶段 2：模式0 实现**
- 实现 `NV_ESC_GSP_FUZZ` 模式0
- 实现完整的 RM 栈调用路径
- 验证与原生路径的一致性

**阶段 3：模式1 实现**
- 实现模式1 的直接 RPC 调用
- 实现最小安全检查
- 集成健康检查机制

**阶段 4：覆盖率评估**
- 实现 RPC 响应时间分析
- 实现 GSP 日志分析
- 实现综合覆盖率评分

**阶段 5：优化和扩展**
- 实现 Hook 点 2（可选）
- 优化健康检查性能
- 添加更多监控指标

### 8.2 安全建议

1. **环境隔离**：
   - ✅ 必须在独立 VM + GPU 直通环境中运行
   - ✅ 避免在生产环境使用

2. **资源限制**：
   - ✅ 限制并发 RPC 数量
   - ✅ 限制单个测试用例的执行时间
   - ✅ 实现自动重置机制

3. **监控和日志**：
   - ✅ 记录所有 RPC 调用和响应
   - ✅ 记录所有健康检查结果
   - ✅ 实现崩溃自动报告

---

## 九、结论

### 9.1 方案合理性评估

| 方面 | 评估 | 说明 |
|------|------|------|
| **整体架构** | ✅ 合理 | 双方案互补，覆盖不同场景 |
| **Hook 点选择** | ⚠️ 可优化 | 建议增加 `rpcRmApiControl_GSP` Hook |
| **模式1 设计** | ⚠️ 需完善 | 需要添加最小安全检查 |
| **覆盖率评估** | ⚠️ 需补充 | 需要实现间接评估方案 |
| **健康检查** | ⚠️ 需扩展 | 需要添加 fuzz 专用检查 |

### 9.2 关键优化点

1. ✅ **Hook 点**：建议在 `rpcRmApiControl_GSP` 增加 Hook，捕获所有 RPC 路径
2. ✅ **模式1 安全**：必须保留 GPU 锁和消息大小检查，添加最小句柄检查
3. ✅ **覆盖率**：采用多维度间接评估（时间 + 日志 + 状态码）
4. ✅ **健康检查**：实现轻量级 + 深度检查的组合机制

### 9.3 最终建议

**方案整体合理，但需要以下优化**：

1. **增加 Hook 点 2**：在 `rpcRmApiControl_GSP` 处 Hook，实现全面覆盖
2. **完善模式1 安全检查**：保留必要的锁和大小检查，添加最小句柄验证
3. **实现覆盖率评估**：采用多维度间接评估方案
4. **扩展健康检查**：实现轻量级 + 深度检查的组合机制

**实施优先级**：
1. 🔴 高优先级：模式1 安全检查、轻量级健康检查
2. 🟡 中优先级：Hook 点 2、覆盖率评估
3. 🟢 低优先级：深度健康检查、高级监控

---

*文档生成时间: 2025-01-XX*  
*基于代码库: open-gpu-kernel-modules*  
*分析范围: GSP RPC 流程、健康检查机制、覆盖率评估方案*

