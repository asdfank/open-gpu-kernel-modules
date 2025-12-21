# GSP RPC Fuzz 方案一（Hook）完整实现指南

## 一、概述

本文档提供方案一（RPC路径插桩Hook）的**完整、详细、可执行**的实现流程，基于对 `GSP_RPC_FLOW_DOCUMENTATION.md` 中描述的完整ioctl执行流程。

### 1.1 目标

- **种子录制**：捕获通过完整RM栈的合法RPC调用
- **在线轻量Fuzz**：在Hook点进行少量in-vivo变异
- **不中断正常流程**：Hook不影响原有执行路径

### 1.2 核心Hook点

根据文档分析，推荐使用**多级Hook策略**：

1. **Hook点1（主要）**：`rmresControl_Prologue_IMPL` - 捕获经过完整RM栈的RPC调用
2. **Hook点2（扩展）**：`rpcRmApiControl_GSP` - 捕获所有RPC路径（包括绕过RM栈的）
3. **Hook点3（可选）**：`GspMsgQueueSendCommand` - 深度监控共享内存操作

### 1.3 文件路径验证

本文档中提到的所有文件路径已通过验证：

**新建文件**：
- ✅ `src/nvidia/inc/kernel/gpu/gsp/gsp_fuzz_hook.h` - 目录存在
- ✅ `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c` - 目录存在
- ✅ `kernel-open/nvidia/gsp_fuzz_ioctl.c` - 目录存在（IOCTL处理，使用Linux原生函数）
- ✅ `tools/gsp_fuzz_hook_user.c` - 目录存在

**需要修改的现有文件**：
- ❌ `src/common/inc/nv-ioctl.h` - **不存在**（错误路径，请勿使用）
- ✅ `src/nvidia/arch/nvalloc/unix/include/nv-ioctl.h` - **文件存在**（正确路径）
- ✅ `kernel-open/common/inc/nv-ioctl.h` - **文件存在**（备选路径）
- ✅ `src/nvidia/src/kernel/rmapi/resource.c` - 文件存在
- ✅ `src/nvidia/src/kernel/vgpu/rpc.c` - 文件存在
- ✅ `kernel-open/nvidia/nv.c` - 文件存在
- ✅ `src/nvidia/src/kernel/gpu/gpu.c` - 文件存在

**注意**：所有路径均基于 `open-gpu-kernel-modules` 项目根目录。

---

## 二、完整ioctl执行流程回顾

### 2.1 执行路径（满足RPC条件）

```
用户态: ioctl(fd, NV_ESC_RM_CONTROL, &params)
  ↓
P1阶段: nvidia_ioctl (nv.c:2419)
  ↓
P1阶段: Nv04ControlWithSecInfo (escape.c:759)
  ↓
P2阶段: rmapiControlWithSecInfo (control.c:1034)
  ↓
P2阶段: _rmapiRmControl (control.c:350)
  ├─ 获取命令标志: rmapiutilGetControlInfo → RMCTRL_FLAGS_ROUTE_TO_PHYSICAL
  └─ 初始化 RmCtrlParams 和 Cookie
  ↓
P2阶段: serverControl (rs_server.c:1453)
  ├─ 获取 Top Lock (API Lock)
  ├─ 获取 Client 锁并查找客户端
  ├─ 查找资源对象 (hSubDevice)
  └─ 设置 TLS 上下文
  ↓
P3阶段: resControl → gpuresControl_IMPL → resControl_IMPL
  ├─ 查找命令处理函数: subdeviceCtrlCmdGspGetFeatures_IMPL
  └─ 执行 Prologue 钩子
  ↓
P3阶段: rmresControl_Prologue_IMPL (resource.c:254) ⭐ **Hook点1**
  ├─ 检查: IS_GSP_CLIENT(pGpu) && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL
  ├─ ✅ 条件满足
  └─ 执行 RPC: NV_RM_RPC_CONTROL → pRmApi->Control()
      └─ 注意：pRmApi->Control 在 GSP 客户端模式下已被替换为 rpcRmApiControl_GSP
  ↓
P4阶段: rpcRmApiControl_GSP (rpc.c:10361) ⭐ **Hook点2**
  ├─ 准备 RPC 消息头
  ├─ 填充 RPC 参数结构
  ├─ 复制命令参数到 RPC 消息缓冲区
  └─ 调用 _issueRpcAndWait 发送并等待响应
  ↓
P4阶段: GspMsgQueueSendCommand (message_queue_cpu.c:446) ⭐ **Hook点3（可选）**
  ├─ 复制命令到共享内存队列
  ├─ 更新写指针
  └─ 触发硬件中断
  ↓
P5阶段: GSP 固件执行 → 写回结果
  ↓
P5阶段: GspMsgQueueReceiveStatus → 复制结果
  ↓
返回: rmresControl_Prologue_IMPL 返回 NV_WARN_NOTHING_TO_DO
  └─ resControl_IMPL 检测到 NV_WARN_NOTHING_TO_DO，跳过本地函数调用
```

### 2.2 关键决策点

**在 `rmresControl_Prologue_IMPL` 中的路由决策**：

```c
// resource.c:264-297
if (pGpu != NULL &&
    ((IS_VIRTUAL(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST))
     || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
{
    // ⭐ Hook点1：在这里插入Hook代码
    NV_RM_RPC_CONTROL(pGpu, pParams->hClient, pParams->hObject, pParams->cmd,
                      pParams->pParams, pParams->paramsSize, status);
    
    return (status == NV_OK) ? NV_WARN_NOTHING_TO_DO : status;
}
```

---

## 三、Hook实现详细步骤

### 3.1 数据结构设计

#### 3.1.1 种子记录结构

**文件**: `src/nvidia/inc/kernel/gpu/gsp/gsp_fuzz_hook.h` (新建)

**路径验证**：
- ✅ `src/nvidia/inc/kernel/gpu/gsp/` - 目录存在
- ✅ 该目录下已有其他GSP相关头文件（如 `kernel_gsp.h`、`message_queue.h` 等）

```c
#ifndef _GSP_FUZZ_HOOK_H_
#define _GSP_FUZZ_HOOK_H_

#include "nvtypes.h"
#include "nvstatus.h"

// 最大参数大小（64KB，可根据需要调整）
#define GSP_FUZZ_MAX_PARAMS_SIZE (64 * 1024)

// Hook配置标志
#define GSP_FUZZ_HOOK_ENABLED           0x00000001
#define GSP_FUZZ_HOOK_RECORD_SEED       0x00000002
#define GSP_FUZZ_HOOK_INLINE_FUZZ       0x00000004
#define GSP_FUZZ_HOOK_RECORD_RESPONSE   0x00000008

// 种子记录结构
typedef struct gsp_fuzz_seed_record
{
    // 基本信息
    NvHandle hClient;
    NvHandle hObject;
    NvU32    cmd;
    NvU32    paramsSize;
    NvU32    ctrlFlags;          // RMCTRL_FLAGS_*
    NvU32    ctrlAccessRight;     // 访问权限
    
    // 参数数据（固定大小，避免动态分配）
    NvU8     params[GSP_FUZZ_MAX_PARAMS_SIZE];
    
    // 元数据
    NvU64    timestamp;          // 时间戳（纳秒）
    NvU32    gpuInstance;          // GPU实例ID
    NvBool   bGspClient;          // 是否为GSP客户端模式
    
    // 响应数据（可选）
    NV_STATUS responseStatus;      // 响应状态码
    NvU32     responseParamsSize;  // 响应参数大小
    NvU8      responseParams[GSP_FUZZ_MAX_PARAMS_SIZE];  // 响应参数
    
    // 统计信息
    NvU64    latencyUs;           // 延迟（微秒）
    NvU32    sequence;             // 序列号（用于去重）
} GSP_FUZZ_SEED_RECORD;

// Hook统计信息
typedef struct gsp_fuzz_hook_stats
{
    NvU64 totalHooks;              // 总Hook次数
    NvU64 rpcHooks;                // RPC路径Hook次数
    NvU64 localHooks;               // 本地路径Hook次数
    NvU64 seedRecords;              // 种子记录数
    NvU64 inlineFuzzCount;         // 在线Fuzz次数
    NvU64 errors;                   // 错误次数
} GSP_FUZZ_HOOK_STATS;

// Hook配置
typedef struct gsp_fuzz_hook_config
{
    NvU32 flags;                   // 配置标志
    NvU32 maxSeedRecords;           // 最大种子记录数
    NvU32 inlineFuzzProbability;   // 在线Fuzz概率（0-100）
    NvU32 seedRecordBufferSize;    // 种子记录缓冲区大小
    void  *pSeedRecordBuffer;       // 种子记录缓冲区（内核地址）
} GSP_FUZZ_HOOK_CONFIG;

#endif // _GSP_FUZZ_HOOK_H_
```

**头文件函数声明补充**（实际实现已添加，需要在头文件中声明）：
```c
// IOCTL处理辅助函数（导出内部状态访问）
NvU32 gspFuzzHookGetSeedRecordIndex(void);
GSP_FUZZ_SEED_RECORD *gspFuzzHookGetSeedRecordBuffer(void);
PORT_SPINLOCK *gspFuzzHookGetSeedRecordLock(void);
GSP_FUZZ_HOOK_CONFIG *gspFuzzHookGetConfigPtr(void);
GSP_FUZZ_HOOK_STATS *gspFuzzHookGetStatsPtr(void);
void gspFuzzHookClearStats(void);

// ⭐ 新增：kernel-open层辅助函数（避免在kernel-open层直接使用nvport函数）
// 在锁保护下复制seed记录到用户提供的缓冲区
// 返回：实际复制的记录数
NvU32 gspFuzzHookCopySeedsLocked(
    NvU32 startIndex,         // 起始索引
    NvU32 requestedCount,     // 请求的记录数
    GSP_FUZZ_SEED_RECORD *pDestBuffer,  // 目标缓冲区（调用者分配）
    NvU32 destBufferCount     // 目标缓冲区可容纳的记录数
);
```

#### 3.1.2 用户态接口结构

**文件**: `kernel-open/common/inc/nv-ioctl.h` (添加到现有文件)

**路径验证**：
- ❌ `src/common/inc/nv-ioctl.h` - **不存在**（错误路径）
- ❌ `src/nvidia/arch/nvalloc/unix/include/nv-ioctl.h` - 此路径为src层，不适合添加kernel-open层的IOCTL定义
- ✅ `kernel-open/common/inc/nv-ioctl.h` - **存在**（正确路径，实际使用此文件）

**注意**：请使用 `kernel-open/common/inc/nv-ioctl.h`，因为GSP Fuzz Hook的IOCTL定义需要在kernel-open层可见。

**⭐ 修复：IOCTL控制面不闭环 - 命令号体系统一**

采用**方案A（推荐）**：只保留一个 `NV_ESC_GSP_FUZZ_HOOK`，payload里带subcmd（GET_CONFIG/SET_CONFIG/…），不再用19/20/21当ioctl nr做二级分发。

```c
// ⭐ 修复：定义子命令常量
#define GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG    1
#define GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG    2
#define GSP_FUZZ_HOOK_SUBCMD_GET_STATS     3
#define GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS     4
#define GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS   5

// ⭐ 修复：定义统一的请求结构
typedef struct nv_ioctl_gsp_fuzz_hook_request
{
    NvU32 subcmd;                   // 子命令
    union {
        nv_ioctl_gsp_fuzz_hook_config_t config;
        nv_ioctl_gsp_fuzz_hook_stats_t stats;
        nv_ioctl_gsp_fuzz_hook_get_seeds_t get_seeds;
    } u;
} nv_ioctl_gsp_fuzz_hook_request_t;

// ⭐ 修复：移除内核头文件include，只定义纯数据结构
// 注意：不再使用 __NV_IOWR/__NV_IOR/__NV_IO 宏，因为这些宏在用户态头文件中可能未定义
// 不再include "gpu/gsp/gsp_fuzz_hook.h"，因为这会引入内核类型/路径依赖

// 用户态配置结构（固定宽度类型）
typedef struct nv_ioctl_gsp_fuzz_hook_config
{
    NvU32 flags;
    NvU32 maxSeedRecords;
    NvU32 inlineFuzzProbability;
    NvU64 seedRecordBufferAddr;    // 用户态缓冲区地址
    NvU32 seedRecordBufferSize;    // 用户态缓冲区大小
} nv_ioctl_gsp_fuzz_hook_config_t;

// 用户态统计结构（固定宽度类型）
typedef struct nv_ioctl_gsp_fuzz_hook_stats
{
    NvU64 totalHooks;
    NvU64 rpcHooks;
    NvU64 localHooks;
    NvU64 seedRecords;
    NvU64 inlineFuzzCount;
    NvU64 errors;
} nv_ioctl_gsp_fuzz_hook_stats_t;

// 获取种子记录（固定宽度类型）
typedef struct nv_ioctl_gsp_fuzz_hook_get_seeds
{
    NvU32 startIndex;               // 起始索引
    NvU32 count;                    // 请求数量
    NvU64 seedRecordBufferAddr;     // 用户态缓冲区地址
    NvU32 seedRecordBufferSize;     // 用户态缓冲区大小
    NvU32 actualCount;               // 实际返回数量（输出）
} nv_ioctl_gsp_fuzz_hook_get_seeds_t;

// ⭐ 修复：定义用户态种子记录结构（与内核 GSP_FUZZ_SEED_RECORD 布局一致）
typedef struct nv_gsp_fuzz_seed_record
{
    // 基本信息
    NvHandle hClient;
    NvHandle hObject;
    NvU32    cmd;
    NvU32    paramsSize;
    NvU32    ctrlFlags;          // RMCTRL_FLAGS_*
    NvU32    ctrlAccessRight;     // 访问权限
    
    // 参数数据（固定大小，避免动态分配）
    NvU8     params[GSP_FUZZ_MAX_PARAMS_SIZE];
    
    // 元数据
    NvU64    timestamp;          // 时间戳（纳秒）
    NvU32    gpuInstance;          // GPU实例ID
    NvBool   bGspClient;          // 是否为GSP客户端模式
    
    // 响应数据（可选）
    NV_STATUS responseStatus;      // 响应状态码
    NvU32     responseParamsSize;  // 响应参数大小
    NvU8      responseParams[GSP_FUZZ_MAX_PARAMS_SIZE];  // 响应参数
    
    // 统计信息
    NvU64    latencyUs;           // 延迟（微秒）
    NvU32    sequence;             // 序列号（用于去重）
} nv_gsp_fuzz_seed_record_t;
```

### 3.2 Hook点1实现：rmresControl_Prologue_IMPL

#### 3.2.1 创建Hook模块文件

**文件**: `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c` (新建)

**路径验证**：
- ✅ `src/nvidia/src/kernel/gpu/gsp/` - 目录存在
- ✅ 该目录下已有其他GSP相关源文件（如 `kernel_gsp.c`、`message_queue_cpu.c` 等）

```c
#include "core/core.h"  // 已经包含了core/prelude.h，而prelude.h包含了nvport/nvport.h
#include "gpu/gpu.h"
#include "rmapi/resource.h"
#include "gpu/gsp/gsp_fuzz_hook.h"
#include "os/os.h"
#include "nv.h"
#include "nvmisc.h"  // For NV_MIN macro
#include "nvport/sync.h"  // For portSyncSpinlock* functions

// 全局Hook状态
static NvBool g_bHookEnabled = NV_FALSE;
// ⭐ 修复问题3：给g_hookConfig一个合理默认值（例如maxSeedRecords=1024）
static GSP_FUZZ_HOOK_CONFIG g_hookConfig = {
    0,  // flags
    1024,  // maxSeedRecords (默认值)
    0,  // inlineFuzzProbability
    0,  // seedRecordBufferSize
    NULL  // pSeedRecordBuffer
};
static GSP_FUZZ_HOOK_STATS g_hookStats = {0};
static NvU32 g_seedRecordIndex = 0;
static GSP_FUZZ_SEED_RECORD *g_pSeedRecordBuffer = NULL;
static PORT_SPINLOCK *g_pSeedRecordLock = NULL;

// 初始化Hook模块
NV_STATUS gspFuzzHookInit(void)
{
    // ⭐ 修复：使用portSyncSpinlockSize获取PORT_SPINLOCK大小（PORT_SPINLOCK是不完整类型）
    // ⭐ 修复：使用正确的函数名portSyncSpinlockInitialize
    extern NvLength portSyncSpinlockSize;
    NV_STATUS status;
    
    // 初始化自旋锁（必须先初始化锁，因为SetConfig需要锁）
    g_pSeedRecordLock = portMemAllocNonPaged(portSyncSpinlockSize);
    if (g_pSeedRecordLock == NULL)
    {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }
    status = portSyncSpinlockInitialize(g_pSeedRecordLock);
    if (status != NV_OK)
    {
        portMemFree(g_pSeedRecordLock);
        g_pSeedRecordLock = NULL;
        return status;
    }
    
    // 分配种子记录缓冲区（如果配置了maxSeedRecords）
    if (g_hookConfig.maxSeedRecords > 0)
    {
        NvU32 bufferSize = g_hookConfig.maxSeedRecords * sizeof(GSP_FUZZ_SEED_RECORD);
        g_pSeedRecordBuffer = portMemAllocNonPaged(bufferSize);
        if (g_pSeedRecordBuffer == NULL)
        {
            portMemFree(g_pSeedRecordLock);
            g_pSeedRecordLock = NULL;
            return NV_ERR_INSUFFICIENT_RESOURCES;
        }
        portMemSet(g_pSeedRecordBuffer, 0, bufferSize);
    }
    
    return NV_OK;
}

// 清理Hook模块
void gspFuzzHookCleanup(void)
{
    if (g_pSeedRecordLock != NULL)
    {
        portMemFree(g_pSeedRecordLock);
        g_pSeedRecordLock = NULL;
    }
    
    if (g_pSeedRecordBuffer != NULL)
    {
        portMemFree(g_pSeedRecordBuffer);
        g_pSeedRecordBuffer = NULL;
    }
}

// 记录种子（线程安全）
static NV_STATUS gspFuzzHookRecordSeed(
    OBJGPU *pGpu,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pParams,
    NvU32 paramsSize,
    NvU32 ctrlFlags,
    NvU32 ctrlAccessRight,
    NV_STATUS responseStatus,
    void *pResponseParams,
    NvU32 responseParamsSize,
    NvU64 latencyUs
)
{
    GSP_FUZZ_SEED_RECORD *pRecord = NULL;
    NvU32 index;
    
    if (!g_bHookEnabled || !(g_hookConfig.flags & GSP_FUZZ_HOOK_RECORD_SEED))
    {
        return NV_OK;
    }
    
    if (g_pSeedRecordBuffer == NULL || g_hookConfig.maxSeedRecords == 0)
    {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }
    
    // 参数大小检查并clamp（避免越界）
    NvU32 clampedParamsSize = NV_MIN(paramsSize, GSP_FUZZ_MAX_PARAMS_SIZE);
    NvU32 clampedResponseParamsSize = NV_MIN(responseParamsSize, GSP_FUZZ_MAX_PARAMS_SIZE);
    
    // ⭐ 修复问题5：统计计数并发保护（使用锁保护）
    // 获取锁并分配记录槽
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    
    if (paramsSize > GSP_FUZZ_MAX_PARAMS_SIZE)
    {
        // 参数过大，记录错误但继续处理（只记录能记录的部分）
        g_hookStats.errors++;
    }
    
    index = g_seedRecordIndex;
    g_seedRecordIndex = (g_seedRecordIndex + 1) % g_hookConfig.maxSeedRecords;
    
    pRecord = &g_pSeedRecordBuffer[index];
    
    // 填充记录
    portMemSet(pRecord, 0, sizeof(GSP_FUZZ_SEED_RECORD));
    pRecord->hClient = hClient;
    pRecord->hObject = hObject;
    pRecord->cmd = cmd;
    pRecord->paramsSize = clampedParamsSize;  // 记录实际记录的大小
    pRecord->ctrlFlags = ctrlFlags;
    pRecord->ctrlAccessRight = ctrlAccessRight;
    pRecord->gpuInstance = pGpu ? pGpu->gpuInstance : 0;
    pRecord->bGspClient = pGpu ? IS_GSP_CLIENT(pGpu) : NV_FALSE;
    // ⭐ 修复：使用osGetCurrentTime()计算时间戳（纳秒）
    // osGetCurrentTime()已确认存在且可见，返回秒和微秒
    NvU32 sec = 0, usec = 0;
    if (osGetCurrentTime(&sec, &usec) == NV_OK)
    {
        // 转换为纳秒：秒 * 10^9 + 微秒 * 10^3
        pRecord->timestamp = (NvU64)sec * 1000000000ULL + (NvU64)usec * 1000ULL;
    }
    else
    {
        pRecord->timestamp = 0;  // 获取失败时使用0
    }
    pRecord->responseStatus = responseStatus;
    pRecord->responseParamsSize = clampedResponseParamsSize;
    pRecord->latencyUs = latencyUs;
    pRecord->sequence = g_hookStats.seedRecords;
    
    // 复制参数数据（使用clamp后的长度）
    if (pParams != NULL && clampedParamsSize > 0)
    {
        portMemCopy(pRecord->params, GSP_FUZZ_MAX_PARAMS_SIZE, pParams, clampedParamsSize);
    }
    
    // 复制响应数据（如果启用，使用clamp后的长度）
    if ((g_hookConfig.flags & GSP_FUZZ_HOOK_RECORD_RESPONSE) &&
        pResponseParams != NULL && clampedResponseParamsSize > 0)
    {
        portMemCopy(pRecord->responseParams, GSP_FUZZ_MAX_PARAMS_SIZE,
                    pResponseParams, clampedResponseParamsSize);
    }
    
    g_hookStats.seedRecords++;
    
    portSyncSpinlockRelease(g_pSeedRecordLock);
    
    return NV_OK;
}

// 在线轻量Fuzz（参数变异）- 对临时拷贝进行变异，不修改原始参数
// 返回：变异后的临时缓冲区指针（需要调用者释放），如果未启用fuzz则返回NULL
static void *gspFuzzHookInlineFuzz(
    void *pOriginalParams,
    NvU32 paramsSize,
    NvU32 cmd,
    NvU32 *pMutatedSize
)
{
    void *pMutatedParams = NULL;
    NvU8 *pByteParams = NULL;
    NvU32 i;
    
    // ⭐ 修复：pParams可能为NULL时的健壮性检查
    if (pOriginalParams == NULL || paramsSize == 0)
    {
        if (pMutatedSize != NULL)
            *pMutatedSize = paramsSize;
        return NULL;
    }
    
    if (!g_bHookEnabled || !(g_hookConfig.flags & GSP_FUZZ_HOOK_INLINE_FUZZ))
    {
        if (pMutatedSize != NULL)
            *pMutatedSize = paramsSize;
        return NULL;  // 未启用fuzz，返回NULL表示使用原始参数
    }
    
    // ⭐ 修复问题5：inlineFuzzProbability clamp到[0,100]
    NvU32 clampedProbability = NV_MIN(g_hookConfig.inlineFuzzProbability, 100);
    
    // 简单的概率检查
    // ⭐ 修复：使用osGetCurrentTime()获取随机种子
    NvU32 sec = 0, usec = 0;
    NvU64 timeSeed = 0;
    if (osGetCurrentTime(&sec, &usec) == NV_OK)
    {
        timeSeed = (NvU64)sec * 1000000ULL + (NvU64)usec;
    }
    if ((timeSeed % 100) >= clampedProbability)
    {
        if (pMutatedSize != NULL)
            *pMutatedSize = paramsSize;
        return NULL;  // 本次不进行fuzz
    }
    
    // 分配临时缓冲区用于变异（使用非分页内存）
    pMutatedParams = portMemAllocNonPaged(paramsSize);
    if (pMutatedParams == NULL)
    {
        // ⭐ 修复：统计计数并发保护
        portSyncSpinlockAcquire(g_pSeedRecordLock);
        g_hookStats.errors++;
        portSyncSpinlockRelease(g_pSeedRecordLock);
        if (pMutatedSize != NULL)
            *pMutatedSize = paramsSize;
        return NULL;  // 分配失败，使用原始参数
    }
    
    // 复制原始参数到临时缓冲区
    portMemCopy(pMutatedParams, paramsSize, pOriginalParams, paramsSize);
    
    // 对临时缓冲区进行变异（不修改原始参数）
    pByteParams = (NvU8 *)pMutatedParams;
    
    // 简单的字节级变异（可以根据需要扩展）
    if (paramsSize > 0)
    {
        // 随机翻转一个字节
        // ⭐ 修复：使用osGetCurrentTime()获取随机种子
        NvU32 sec2 = 0, usec2 = 0;
        NvU64 timeSeed2 = 0;
        if (osGetCurrentTime(&sec2, &usec2) == NV_OK)
        {
            timeSeed2 = (NvU64)sec2 * 1000000ULL + (NvU64)usec2;
        }
        i = (timeSeed2 % paramsSize);
        pByteParams[i] ^= (1 << (timeSeed2 % 8));
    }
    
    // ⭐ 修复：统计计数并发保护
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    g_hookStats.inlineFuzzCount++;
    portSyncSpinlockRelease(g_pSeedRecordLock);
    
    if (pMutatedSize != NULL)
        *pMutatedSize = paramsSize;
    
    return pMutatedParams;  // 返回变异后的缓冲区
}

// Hook点1：rmresControl_Prologue_IMPL Hook函数
// 返回值：如果启用了inline fuzz，返回变异后的参数缓冲区指针（需要调用者释放），否则返回NULL
// 注意：此函数只做统计和准备变异参数，不执行RPC，latency统计在调用者中完成
void *gspFuzzHook_RmresControlPrologue(
    OBJGPU *pGpu,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pParams,
    NvU32 paramsSize,
    NvU32 ctrlFlags,
    NvU32 *pMutatedParamsSize
)
{
    void *pMutatedParams = NULL;
    
    if (!g_bHookEnabled)
    {
        if (pMutatedParamsSize != NULL)
            *pMutatedParamsSize = paramsSize;
        return NULL;  // Hook未启用，直接返回NULL（使用原始参数）
    }
    
    // ⭐ 修复问题5：统计计数并发（使用锁保护）
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    g_hookStats.totalHooks++;
    portSyncSpinlockRelease(g_pSeedRecordLock);
    
    // 检查是否为RPC路径
    if (pGpu != NULL && IS_GSP_CLIENT(pGpu) && 
        (ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))
    {
        // ⭐ 修复：统计计数并发保护
        portSyncSpinlockAcquire(g_pSeedRecordLock);
        g_hookStats.rpcHooks++;
        portSyncSpinlockRelease(g_pSeedRecordLock);
        
        // 如果启用在线Fuzz，对临时拷贝进行变异（不修改原始参数）
        // 注意：latency统计在调用者中完成（围绕NV_RM_RPC_CONTROL）
        if (g_hookConfig.flags & GSP_FUZZ_HOOK_INLINE_FUZZ)
        {
            pMutatedParams = gspFuzzHookInlineFuzz(pParams, paramsSize, cmd, pMutatedParamsSize);
        }
        else
        {
            if (pMutatedParamsSize != NULL)
                *pMutatedParamsSize = paramsSize;
        }
    }
    else
    {
        // ⭐ 修复：统计计数并发保护
        portSyncSpinlockAcquire(g_pSeedRecordLock);
        g_hookStats.localHooks++;
        portSyncSpinlockRelease(g_pSeedRecordLock);
        if (pMutatedParamsSize != NULL)
            *pMutatedParamsSize = paramsSize;
    }
    
    return pMutatedParams;  // 返回变异后的参数缓冲区（如果启用fuzz），否则返回NULL
}

// Hook点1：记录响应（在RPC返回后调用）
// 注意：pOriginalParams是原始未变异的参数，用于记录合法种子
void gspFuzzHook_RmresControlPrologueResponse(
    OBJGPU *pGpu,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pOriginalParams,  // 原始未变异的参数（用于记录合法种子）
    NvU32 originalParamsSize,
    NvU32 ctrlFlags,
    NvU32 ctrlAccessRight,
    NV_STATUS responseStatus,
    void *pResponseParams,
    NvU32 responseParamsSize,
    NvU64 latencyUs  // 真实的RPC往返延迟（由调用者测量）
)
{
    if (!g_bHookEnabled)
    {
        return;
    }
    
    // 记录种子（使用原始未变异的参数，保证种子的合法性）
    gspFuzzHookRecordSeed(
        pGpu, hClient, hObject, cmd,
        pOriginalParams, originalParamsSize,  // 使用原始参数记录
        ctrlFlags, ctrlAccessRight,
        responseStatus, pResponseParams, responseParamsSize,
        latencyUs
    );
}

// 获取统计信息
void gspFuzzHookGetStats(GSP_FUZZ_HOOK_STATS *pStats)
{
    if (pStats != NULL)
    {
        // ⭐ 修复问题9：检查锁是否已初始化
        if (g_pSeedRecordLock != NULL)
        {
            portSyncSpinlockAcquire(g_pSeedRecordLock);
            portMemCopy(pStats, sizeof(GSP_FUZZ_HOOK_STATS), &g_hookStats, sizeof(GSP_FUZZ_HOOK_STATS));
            portSyncSpinlockRelease(g_pSeedRecordLock);
        }
        else
        {
            // Hook未初始化，返回零值
            portMemSet(pStats, 0, sizeof(GSP_FUZZ_HOOK_STATS));
        }
    }
}

// 设置配置
// ⭐ 修复：gspFuzzHookSetConfig不会分配/resize seed ring buffer
NV_STATUS gspFuzzHookSetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig)
{
    GSP_FUZZ_SEED_RECORD *pNewBuffer = NULL;
    GSP_FUZZ_SEED_RECORD *pOldBuffer = NULL;
    NvU32 oldMaxRecords = 0;
    NvU32 newMaxRecords = 0;
    
    if (pConfig == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    
    // 保存旧的maxSeedRecords值
    oldMaxRecords = g_hookConfig.maxSeedRecords;
    newMaxRecords = pConfig->maxSeedRecords;
    
    // 更新配置（除了buffer相关字段）
    g_hookConfig.flags = pConfig->flags;
    g_hookConfig.inlineFuzzProbability = pConfig->inlineFuzzProbability;
    g_bHookEnabled = (g_hookConfig.flags & GSP_FUZZ_HOOK_ENABLED) != 0;
    
    // ⭐ 关键修复：检测maxSeedRecords变化并分配/resize buffer
    if (newMaxRecords != oldMaxRecords)
    {
        if (newMaxRecords > 0)
        {
            // 分配新buffer
            NvU32 bufferSize = newMaxRecords * sizeof(GSP_FUZZ_SEED_RECORD);
            pNewBuffer = portMemAllocNonPaged(bufferSize);
            if (pNewBuffer == NULL)
            {
                // 分配失败，恢复旧配置
                g_hookConfig.maxSeedRecords = oldMaxRecords;
                portSyncSpinlockRelease(g_pSeedRecordLock);
                return NV_ERR_INSUFFICIENT_RESOURCES;
            }
            portMemSet(pNewBuffer, 0, bufferSize);
        }
        
        // 保存旧buffer以便释放
        pOldBuffer = g_pSeedRecordBuffer;
        
        // 更新配置和buffer指针
        g_hookConfig.maxSeedRecords = newMaxRecords;
        g_pSeedRecordBuffer = pNewBuffer;
        g_seedRecordIndex = 0;  // 重置索引
        
        portSyncSpinlockRelease(g_pSeedRecordLock);
        
        // ⭐ 在锁外释放旧buffer（避免在锁内进行可能阻塞的操作）
        if (pOldBuffer != NULL)
        {
            portMemFree(pOldBuffer);
        }
    }
    else
    {
        // maxSeedRecords未变化，只更新其他配置
        portSyncSpinlockRelease(g_pSeedRecordLock);
    }
    
    return NV_OK;
}

// 获取配置
void gspFuzzHookGetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig)
{
    if (pConfig != NULL)
    {
        // ⭐ 修复问题9：检查锁是否已初始化
        if (g_pSeedRecordLock != NULL)
        {
            portSyncSpinlockAcquire(g_pSeedRecordLock);
            portMemCopy(pConfig, sizeof(GSP_FUZZ_HOOK_CONFIG), &g_hookConfig, sizeof(GSP_FUZZ_HOOK_CONFIG));
            portSyncSpinlockRelease(g_pSeedRecordLock);
        }
        else
        {
            // Hook未初始化，返回默认值
            portMemSet(pConfig, 0, sizeof(GSP_FUZZ_HOOK_CONFIG));
        }
    }
}

// 检查响应记录是否启用（供其他模块使用）
NvBool gspFuzzHookIsResponseRecordingEnabled(void)
{
    return g_bHookEnabled && (g_hookConfig.flags & GSP_FUZZ_HOOK_RECORD_RESPONSE);
}

// 检查Hook是否启用（供其他模块使用）
NvBool gspFuzzHookIsEnabled(void)
{
    return g_bHookEnabled;
}

// 清除统计信息（供IOCTL使用）
void gspFuzzHookClearStats(void)
{
    if (g_pSeedRecordLock != NULL)
    {
        portSyncSpinlockAcquire(g_pSeedRecordLock);
        portMemSet(&g_hookStats, 0, sizeof(g_hookStats));
        g_seedRecordIndex = 0;
        portSyncSpinlockRelease(g_pSeedRecordLock);
    }
}

// ⭐ 新增：在锁保护下复制seed记录到用户提供的缓冲区
// 这个函数封装了所有nvport操作，供kernel-open层的gsp_fuzz_ioctl.c使用
// 返回：实际复制的记录数
NvU32 gspFuzzHookCopySeedsLocked(
    NvU32 startIndex,
    NvU32 requestedCount,
    GSP_FUZZ_SEED_RECORD *pDestBuffer,
    NvU32 destBufferCount
)
{
    NvU32 actualCount = 0;
    NvU32 totalRecords;
    NvU32 availableRecords;
    NvU32 i;
    NvU32 srcIndex;
    
    // 参数验证
    if (pDestBuffer == NULL || destBufferCount == 0 || requestedCount == 0)
    {
        return 0;
    }
    
    // 检查内部状态
    if (g_pSeedRecordLock == NULL || g_pSeedRecordBuffer == NULL || 
        g_hookConfig.maxSeedRecords == 0)
    {
        return 0;
    }
    
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    
    // 计算实际可读取的数量
    totalRecords = g_hookStats.seedRecords;
    availableRecords = (totalRecords > g_hookConfig.maxSeedRecords) 
                      ? g_hookConfig.maxSeedRecords 
                      : totalRecords;
    
    if (startIndex >= availableRecords)
    {
        actualCount = 0;
    }
    else
    {
        // 计算实际要复制的数量
        actualCount = requestedCount;
        if (actualCount > (availableRecords - startIndex))
            actualCount = availableRecords - startIndex;
        if (actualCount > destBufferCount)
            actualCount = destBufferCount;
        
        // 从ring buffer中读取（考虑循环缓冲区）
        for (i = 0; i < actualCount; i++)
        {
            srcIndex = (g_seedRecordIndex - availableRecords + startIndex + i) 
                       % g_hookConfig.maxSeedRecords;
            portMemCopy(&pDestBuffer[i], sizeof(GSP_FUZZ_SEED_RECORD),
                       &g_pSeedRecordBuffer[srcIndex], sizeof(GSP_FUZZ_SEED_RECORD));
        }
    }
    
    portSyncSpinlockRelease(g_pSeedRecordLock);
    
    return actualCount;
}
```

#### 3.2.2 修改rmresControl_Prologue_IMPL函数

**文件**: `src/nvidia/src/kernel/rmapi/resource.c`

**路径验证**：
- ✅ `src/nvidia/src/kernel/rmapi/resource.c` - 文件存在

**修改位置**: 在 `rmresControl_Prologue_IMPL` 函数中插入Hook调用

```c
// 在文件顶部添加include
#include "gpu/gsp/gsp_fuzz_hook.h"

// 修改 rmresControl_Prologue_IMPL 函数
NV_STATUS
rmresControl_Prologue_IMPL
(
    RmResource *pResource,
    CALL_CONTEXT *pCallContext,
    RS_RES_CONTROL_PARAMS_INTERNAL *pParams
)
{
    NV_STATUS status = NV_OK;
    OBJGPU *pGpu = gpumgrGetGpu(pResource->rpcGpuInstance);
    NvU64 latencyUs = 0;
    void *pMutatedParams = NULL;  // 变异后的参数缓冲区（如果启用inline fuzz）
    void *pOriginalParamsCopy = NULL;  // ⭐ 修复：原始参数快照（复制到临时缓冲区）
    NvU32 originalParamsSize = 0;
    NvU32 mutatedParamsSize = 0;
    void *pResponseParams = NULL;
    void *pResponseParamsCopy = NULL;  // ⭐ 修复：响应参数快照（如果记录响应）
    NvU32 responseParamsSize = 0;
    NvU32 ctrlAccessRight = 0;

    // ⭐ Hook点1：在RPC路由检查之前调用Hook（准备变异参数，但不修改原始参数）
    if (pGpu != NULL && IS_GSP_CLIENT(pGpu) && 
        (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))
    {
        // ⭐ 修复：原始请求参数记录错误 - 在RPC调用前对原始请求参数进行快照
        originalParamsSize = pParams->paramsSize;
        if (pParams->pParams != NULL && originalParamsSize > 0)
        {
            NvU32 copySize = NV_MIN(originalParamsSize, GSP_FUZZ_MAX_PARAMS_SIZE);
            pOriginalParamsCopy = portMemAllocNonPaged(copySize);
            if (pOriginalParamsCopy != NULL)
            {
                portMemCopy(pOriginalParamsCopy, copySize, pParams->pParams, copySize);
                if (originalParamsSize > GSP_FUZZ_MAX_PARAMS_SIZE)
                {
                    originalParamsSize = GSP_FUZZ_MAX_PARAMS_SIZE;
                }
            }
            // 如果分配失败，降级处理：只记录元数据，不记录params（在记录函数中处理）
        }
        
        // 获取访问权限（如果可用）
        if (pCallContext != NULL && pCallContext->pResourceRef != NULL)
        {
            // 可以从pCallContext中获取访问权限信息
            // 这里简化处理
        }
        
        // 调用Hook函数，获取变异后的参数（如果启用inline fuzz）
        // 注意：此函数不修改原始参数，只返回变异后的临时缓冲区
        pMutatedParams = gspFuzzHook_RmresControlPrologue(
            pGpu,
            pParams->hClient,
            pParams->hObject,
            pParams->cmd,
            pParams->pParams,
            pParams->paramsSize,
            pParams->pCookie->ctrlFlags,
            &mutatedParamsSize
        );
    }

    if (pGpu != NULL &&
        ((IS_VIRTUAL(pGpu)    && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST)
        ) || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
    {
        //
        // GPU lock is required to protect the RPC buffers.
        // However, some controls have  ROUTE_TO_PHYSICAL + NO_GPUS_LOCK flags set.
        // This is not valid in offload mode, but is in monolithic.
        // In those cases, just acquire the lock for the RPC
        //
        GPU_MASK gpuMaskRelease = 0;
        if (!rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
        {
            //
            // Log any case where the above assumption is not true, but continue
            // anyway. Use SAFE_LOCK_UPGRADE to try and recover in these cases.
            //
            NV_ASSERT(pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_NO_GPUS_LOCK);
            NV_ASSERT_OK_OR_RETURN(rmGpuGroupLockAcquire(pGpu->gpuInstance,
                                   GPU_LOCK_GRP_SUBDEVICE,
                                   GPU_LOCK_FLAGS_SAFE_LOCK_UPGRADE,
                                   RM_LOCK_MODULES_RPC,
                                   &gpuMaskRelease));
        }

        // ⭐ 记录RPC调用开始时间（真实RPC延迟测量）
        // ⭐ 修复：使用osGetCurrentTime()计算时间戳（该函数已确认存在且可见）
        NvU32 sec = 0, usec = 0;
        NvU64 rpcStartUs = 0;
        if (osGetCurrentTime(&sec, &usec) == NV_OK)
        {
            rpcStartUs = (NvU64)sec * 1000000ULL + (NvU64)usec;
        }

        // ⭐ 使用变异后的参数（如果启用inline fuzz），否则使用原始参数
        void *pRpcParams = (pMutatedParams != NULL) ? pMutatedParams : pParams->pParams;
        NvU32 rpcParamsSize = (pMutatedParams != NULL) ? mutatedParamsSize : pParams->paramsSize;

        NV_RM_RPC_CONTROL(pGpu, pParams->hClient, pParams->hObject, pParams->cmd,
                          pRpcParams, rpcParamsSize, status);

        // ⭐ 计算真实RPC延迟（从发送到接收的完整往返时间）
        // ⭐ 修复：使用osGetCurrentTime()计算结束时间
        sec = usec = 0;
        NvU64 rpcEndUs = rpcStartUs;  // 默认值，如果获取失败则使用开始时间
        if (osGetCurrentTime(&sec, &usec) == NV_OK)
        {
            rpcEndUs = (NvU64)sec * 1000000ULL + (NvU64)usec;
        }
        latencyUs = (rpcEndUs > rpcStartUs) ? (rpcEndUs - rpcStartUs) : 0;

        // ⭐ 修复：响应记录在inline fuzz时逻辑错误
        // 如果启用了inline fuzz，禁用响应记录（seed是合法的原始请求，响应是fuzz后的，天然不匹配）
        if (gspFuzzHookIsResponseRecordingEnabled() && pMutatedParams == NULL)
        {
            // 只有在未启用inline fuzz时才记录响应
            if (pParams->pParams != NULL && pParams->paramsSize > 0)
            {
                NvU32 copySize = NV_MIN(pParams->paramsSize, GSP_FUZZ_MAX_PARAMS_SIZE);
                pResponseParamsCopy = portMemAllocNonPaged(copySize);
                if (pResponseParamsCopy != NULL)
                {
                    portMemCopy(pResponseParamsCopy, copySize, pParams->pParams, copySize);
                    pResponseParams = pResponseParamsCopy;
                    responseParamsSize = copySize;
                }
                else
                {
                    // ⭐ 修复问题7：分配失败时清零size，避免"伪seed"
                    responseParamsSize = 0;
                }
            }
        }

        // ⭐ Hook点1：记录响应（在RPC返回后）
        // 注意：使用原始未变异的参数记录种子，保证种子的合法性
        if (pGpu != NULL && IS_GSP_CLIENT(pGpu))
        {
            gspFuzzHook_RmresControlPrologueResponse(
                pGpu,
                pParams->hClient,
                pParams->hObject,
                pParams->cmd,
                pOriginalParamsCopy,  // ⭐ 使用原始参数快照记录种子
                originalParamsSize,
                pParams->pCookie->ctrlFlags,
                ctrlAccessRight,
                status,
                pResponseParams,  // 响应参数快照（如果记录响应）
                responseParamsSize,
                latencyUs  // 真实的RPC往返延迟
            );
        }

        // ⭐ 释放变异参数缓冲区（如果分配了）
        if (pMutatedParams != NULL)
        {
            portMemFree(pMutatedParams);
            pMutatedParams = NULL;
        }
        
        // ⭐ 释放响应参数快照缓冲区（如果分配了）
        if (pResponseParamsCopy != NULL)
        {
            portMemFree(pResponseParamsCopy);
            pResponseParamsCopy = NULL;
        }
        
        // ⭐ 修复问题6：RPC路径下释放原始参数快照（避免内存泄漏）
        // 必须在return前释放，否则每次RPC调用都会泄漏内存
        if (pOriginalParamsCopy != NULL)
        {
            portMemFree(pOriginalParamsCopy);
            pOriginalParamsCopy = NULL;
        }

        if (gpuMaskRelease != 0)
        {
            rmGpuGroupLockRelease(gpuMaskRelease, GPUS_LOCK_FLAGS_NONE);
        }

        return (status == NV_OK) ? NV_WARN_NOTHING_TO_DO : status;
    }
    
    // 如果未进入RPC路径，也需要释放变异参数缓冲区和原始参数快照（如果分配了）
    if (pMutatedParams != NULL)
    {
        portMemFree(pMutatedParams);
    }
    
    if (pOriginalParamsCopy != NULL)
    {
        portMemFree(pOriginalParamsCopy);
    }

    return NV_OK;
}
```

**注意**：需要添加辅助函数：

```c
// 在 gsp_fuzz_hook.c 中添加
NvBool gspFuzzHookIsResponseRecordingEnabled(void)
{
    return g_bHookEnabled && (g_hookConfig.flags & GSP_FUZZ_HOOK_RECORD_RESPONSE);
}

// 检查Hook是否启用（供其他模块使用）
NvBool gspFuzzHookIsEnabled(void)
{
    return g_bHookEnabled;
}
```

### 3.3 Hook点2实现：rpcRmApiControl_GSP（可选扩展）

**文件**: `src/nvidia/src/kernel/vgpu/rpc.c`

**路径验证**：
- ✅ `src/nvidia/src/kernel/vgpu/rpc.c` - 文件存在

在 `rpcRmApiControl_GSP` 函数中添加Hook调用：

```c
// 在文件顶部添加include
#include "gpu/gsp/gsp_fuzz_hook.h"

NV_STATUS rpcRmApiControl_GSP
(
    RM_API *pRmApi,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pParamStructPtr,
    NvU32 paramsSize
)
{
    // ... 原有代码 ...
    
    // ⭐ Hook点2：在准备RPC消息之前
    if (gspFuzzHookIsEnabled())
    {
        gspFuzzHook_RpcRmApiControl(
            pGpu, hClient, hObject, cmd, pParamStructPtr, paramsSize
        );
    }
    
    // ... 继续原有代码 ...
    
    // ⭐ Hook点2：在RPC发送之后（可选）
    // 可以在这里记录序列化后的RPC消息
}
```

### 3.4 IOCTL处理函数实现

**文件**: 
- 新建 `kernel-open/nvidia/gsp_fuzz_ioctl.c` - IOCTL处理函数实现（使用Linux原生函数）
- 修改 `kernel-open/nvidia/nv.c` - 添加IOCTL case处理

**路径验证**：
- ✅ `kernel-open/nvidia/nv.c` - 文件存在
- ✅ `kernel-open/nvidia/` - 目录存在

**重要说明**：
`gsp_fuzz_ioctl.c` 放在 `kernel-open/` 层而不是 `src/nvidia/` 层，因为：
- 它需要使用 Linux 原生函数（如 `kzalloc`, `kfree`, `NV_COPY_TO_USER`）
- `src/nvidia/` 层是 OS-agnostic 的，只能使用 nvport 函数
- 此架构与 NVIDIA 驱动的分层设计保持一致

**实现说明**：
实际实现分为两部分：
1. 在 `gsp_fuzz_ioctl.c` 中实现具体的IOCTL处理逻辑
2. 在 `nv.c` 中添加case分支调用处理函数

**⭐ 关键实现差异说明**：

由于 `gsp_fuzz_ioctl.c` 位于 `kernel-open/` 层，实际实现与下面的示例代码有以下差异：

| 文档示例 | 实际实现 | 原因 |
|-----------|----------|------|
| `portMemAllocNonPaged()` | `kzalloc()` | kernel-open层使用Linux原生函数 |
| `portMemFree()` | `kfree()` | kernel-open层使用Linux原生函数 |
| 直接访问spinlock和ring buffer | `gspFuzzHookCopySeedsLocked()` | 辅助函数封装在OS-agnostic层 |
| `#include "gpu/gsp/gsp_fuzz_hook.h"` | `#include "kernel/gpu/gsp/gsp_fuzz_hook.h"` | Kbuild路径配置不同 |

**3.4.1 IOCTL处理函数实现**

**文件**: `kernel-open/nvidia/gsp_fuzz_ioctl.c` (新建)

```c
#include "nv.h"
#include "nv-linux.h"  // For NV_IS_SUSER, NV_COPY_TO_USER, Linux error codes (EPERM, EINVAL, etc.)
#include "nvmisc.h"  // For NV_MIN macro
#include "nv-ioctl.h"  // ⭐ 修复：包含kernel-open层的nv-ioctl.h（已包含GSP Fuzz Hook定义）
// ⭐ 修复：包含OS-agnostic层的Hook模块头文件（用于访问内核Hook函数）
// 通过Kbuild设置的include路径访问（-I$(src)/../src/nvidia/inc/kernel）
#include "kernel/gpu/gsp/gsp_fuzz_hook.h"

// ⭐ 修复：不再包含nvport头文件，使用Linux原生函数
// #include "nvport/nvport.h"  // 已移除

// ⭐ 修复：IOCTL控制面不闭环 - 统一接口
// 处理GSP Fuzz Hook IOCTL（从nv.c调用）
// 函数签名改为：int nvidia_ioctl_gsp_fuzz_hook(void *arg_copy, size_t arg_size)
// 根据 req->subcmd 分发，而不是 _IOC_NR(cmd)
int nvidia_ioctl_gsp_fuzz_hook(
    void *arg_copy,
    size_t arg_size
)
{
    int ret = 0;
    nv_ioctl_gsp_fuzz_hook_request_t *req = NULL;
    
    // ⭐ 权限检查：只有root或CAP_SYS_ADMIN可以操作Hook
    if (!NV_IS_SUSER())
    {
        return -EPERM;
    }
    
    if (arg_size < sizeof(nv_ioctl_gsp_fuzz_hook_request_t))
    {
        return -EINVAL;
    }
    
    req = (nv_ioctl_gsp_fuzz_hook_request_t *)arg_copy;
    
    // ⭐ 修复：根据 subcmd 分发，而不是 _IOC_NR(cmd)
    switch (req->subcmd)
    {
        case GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG:  // 1
        {
            nv_ioctl_gsp_fuzz_hook_config_t *userConfig = (nv_ioctl_gsp_fuzz_hook_config_t *)arg_copy;
            GSP_FUZZ_HOOK_CONFIG kernelConfig;
            
            if (arg_size != sizeof(nv_ioctl_gsp_fuzz_hook_config_t))
            {
                return -EINVAL;
            }
            
            gspFuzzHookGetConfig(&kernelConfig);
            
            // 转换内核配置到用户态配置
            userConfig->flags = kernelConfig.flags;
            userConfig->maxSeedRecords = kernelConfig.maxSeedRecords;
            userConfig->inlineFuzzProbability = kernelConfig.inlineFuzzProbability;
            // ⭐ 修复问题8：不要返回内核指针给用户态（安全/语义问题）
            // 真正的数据读取走GET_SEEDS（由内核copy_to_user到用户提供的buffer）
            userConfig->seedRecordBufferAddr = 0;  // 不暴露内核地址
            userConfig->seedRecordBufferSize = 0;   // 不暴露内核buffer大小
            
            break;
        }
        
        case GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG:  // 2
        {
            nv_ioctl_gsp_fuzz_hook_config_t *userConfig = &req->u.config;
            GSP_FUZZ_HOOK_CONFIG kernelConfig;
            
            if (arg_size != sizeof(nv_ioctl_gsp_fuzz_hook_config_t))
            {
                return -EINVAL;
            }
            
            // 转换用户态配置到内核配置
            kernelConfig.flags = userConfig->flags;
            kernelConfig.maxSeedRecords = userConfig->maxSeedRecords;
            kernelConfig.inlineFuzzProbability = userConfig->inlineFuzzProbability;
            // 注意：缓冲区地址需要特殊处理（用户态地址转换为内核地址）
            // 这里暂时不处理，因为缓冲区是在内核中分配的
            kernelConfig.pSeedRecordBuffer = NULL;
            kernelConfig.seedRecordBufferSize = 0;
            
            ret = gspFuzzHookSetConfig(&kernelConfig);
            if (ret != NV_OK)
            {
                return -EINVAL;
            }
            
            break;
        }
        
        case GSP_FUZZ_HOOK_SUBCMD_GET_STATS:  // 3
        {
            nv_ioctl_gsp_fuzz_hook_stats_t *userStats = &req->u.stats;
            GSP_FUZZ_HOOK_STATS kernelStats;
            
            if (arg_size != sizeof(nv_ioctl_gsp_fuzz_hook_stats_t))
            {
                return -EINVAL;
            }
            
            gspFuzzHookGetStats(&kernelStats);
            
            // 转换内核统计到用户态统计
            userStats->totalHooks = kernelStats.totalHooks;
            userStats->rpcHooks = kernelStats.rpcHooks;
            userStats->localHooks = kernelStats.localHooks;
            userStats->seedRecords = kernelStats.seedRecords;
            userStats->inlineFuzzCount = kernelStats.inlineFuzzCount;
            userStats->errors = kernelStats.errors;
            
            break;
        }
        
        case GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS:  // 4
        {
            nv_ioctl_gsp_fuzz_hook_get_seeds_t *getSeedsReq = &req->u.get_seeds;
            GSP_FUZZ_SEED_RECORD *pKernelSeeds = NULL;
            NvU32 actualCount = 0;
            
            // 参数验证
            if (getSeedsReq->count == 0 || getSeedsReq->count > 1000)  // 限制单次最多读取1000条
            {
                return -EINVAL;
            }
            
            if (getSeedsReq->seedRecordBufferAddr == 0 || getSeedsReq->seedRecordBufferSize == 0)
            {
                return -EINVAL;
            }
            
            // ⭐ 注意：用户态使用nv_gsp_fuzz_seed_record_t，内核使用GSP_FUZZ_SEED_RECORD
            if (getSeedsReq->seedRecordBufferSize < getSeedsReq->count * sizeof(GSP_FUZZ_SEED_RECORD))
            {
                return -EINVAL;
            }
            
            // ⭐ 修复：使用Linux原生函数kzalloc分配内存（而不是portMemAllocNonPaged）
            pKernelSeeds = kzalloc(getSeedsReq->count * sizeof(GSP_FUZZ_SEED_RECORD), GFP_KERNEL);
            if (pKernelSeeds == NULL)
            {
                return -ENOMEM;
            }
            
            // ⭐ 修复：使用新的辅助函数，避免直接使用nvport spinlock函数
            // 该函数在OS-agnostic层实现，内部处理了加锁和数据复制
            actualCount = gspFuzzHookCopySeedsLocked(
                getSeedsReq->startIndex,
                getSeedsReq->count,
                pKernelSeeds,
                getSeedsReq->count
            );
            
            // 复制到用户态缓冲区
            if (actualCount > 0)
            {
                if (NV_COPY_TO_USER((void *)(uintptr_t)getSeedsReq->seedRecordBufferAddr,
                                 pKernelSeeds, actualCount * sizeof(GSP_FUZZ_SEED_RECORD)))
                {
                    kfree(pKernelSeeds);  // ⭐ 修复：使用Linux原生函数kfree
                    return -EFAULT;
                }
            }
            
            // 更新实际返回数量
            getSeedsReq->actualCount = actualCount;
            
            kfree(pKernelSeeds);  // ⭐ 修复：使用Linux原生函数kfree
            break;
        }
        
        case GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS:  // 5
        {
            // 清除统计信息（使用提供的函数）
            gspFuzzHookClearStats();
            break;
        }
        
        default:
            return -EINVAL;
    }
    
    return ret;
}

```

**3.4.2 在nv.c中集成IOCTL处理**

**文件**: `kernel-open/nvidia/nv.c` (修改)

在 `nvidia_ioctl` 函数的 `switch (arg_cmd)` 语句中添加：

```c
case NV_ESC_GSP_FUZZ_HOOK:
{
    // ⭐ 修复：IOCTL控制面不闭环 - 统一接口
    // GSP Fuzz Hook IOCTL处理
    // 根据 req->subcmd 分发，不再使用 _IOC_NR(cmd)
    {
        // 声明处理函数（实际实现在gsp_fuzz_ioctl.c中）
        extern int nvidia_ioctl_gsp_fuzz_hook(void *arg_copy, size_t arg_size);
        
        status = nvidia_ioctl_gsp_fuzz_hook(arg_copy, arg_size);
        if (status != 0)
        {
            goto done;
        }
    }
    break;
}
```

**注意**：
- `nv.c` 中的 `arg_copy` 已经是内核空间的副本，不需要 `copy_from_user`
- 处理函数返回后，`nv.c` 会自动将结果复制回用户空间（如果需要）

### 3.5 模块初始化和清理

**文件**: `src/nvidia/src/kernel/gpu/gpu.c` (修改)

**路径验证**：
- ✅ `src/nvidia/src/kernel/gpu/gpu.c` - 文件存在

**实现说明**：
Hook模块是全局的，不是per-GPU的，所以在 `gpuPostConstruct_IMPL` 中使用静态变量确保只初始化一次。

**在文件顶部添加include**：
```c
#include "gpu/gsp/gsp_fuzz_hook.h"
```

**在 `gpuPostConstruct_IMPL` 函数末尾添加**：
```c
    gpuRefreshRecoveryAction_KERNEL(pGpu, NV_TRUE);

    // 初始化GSP Fuzz Hook（如果尚未初始化）
    // 注意：这里只初始化一次，因为Hook是全局的，不是per-GPU的
    static NvBool g_bGspFuzzHookInitialized = NV_FALSE;
    if (!g_bGspFuzzHookInitialized)
    {
        NV_STATUS hookStatus = gspFuzzHookInit();
        if (hookStatus == NV_OK)
        {
            g_bGspFuzzHookInitialized = NV_TRUE;
        }
        // 如果初始化失败，记录错误但不阻止GPU初始化
    }

    return NV_OK;
}
```

**⭐ 修复：cleanup没接入模块退出**

在 `nv_module_exit` 中调用 `gspFuzzHookCleanup()`：

**文件**: `kernel-open/nvidia/nv.c` (修改)

```c
static void
nv_module_exit(nv_stack_t *sp)
{
    // ... 其他清理 ...
    
    // ⭐ 修复：清理Hook模块
    {
        extern void gspFuzzHookCleanup(void);
        gspFuzzHookCleanup();
    }
    
    // ... 其他清理 ...
}
```

**注意**：
- Hook模块在第一个GPU初始化时初始化
- 如果初始化失败，不会阻止GPU的正常初始化流程
- ⭐ 修复：清理逻辑已在模块卸载时调用 `gspFuzzHookCleanup()`

---

## 四、用户态工具实现

### 4.1 用户态库接口

**文件**: `tools/gsp_fuzz_hook_user.c` (新建用户态工具)

**路径验证**：
- ✅ `tools/` - 目录存在（当前文档就在此目录下）

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "nv-ioctl.h"

#define NVIDIA_DEVICE_PATH "/dev/nvidia0"

// 打开NVIDIA设备
static int open_nvidia_device(void)
{
    int fd = open(NVIDIA_DEVICE_PATH, O_RDWR);
    if (fd < 0)
    {
        perror("Failed to open NVIDIA device");
        return -1;
    }
    return fd;
}

// 设置Hook配置
int gsp_fuzz_hook_set_config(
    int fd,
    NvU32 flags,
    NvU32 maxSeedRecords,
    NvU32 inlineFuzzProbability
)
{
    struct nv_ioctl_gsp_fuzz_hook_config config = {0};
    int ret;
    
    config.flags = flags;
    config.maxSeedRecords = maxSeedRecords;
    config.inlineFuzzProbability = inlineFuzzProbability;
    
    ret = ioctl(fd, NV_IOCTL_GSP_FUZZ_HOOK_SET_CONFIG, &config);
    if (ret < 0)
    {
        perror("Failed to set hook config");
        return -1;
    }
    
    return 0;
}

// 获取统计信息
int gsp_fuzz_hook_get_stats(int fd, struct nv_ioctl_gsp_fuzz_hook_stats *pStats)
{
    int ret;
    
    ret = ioctl(fd, NV_IOCTL_GSP_FUZZ_HOOK_GET_STATS, pStats);
    if (ret < 0)
    {
        perror("Failed to get hook stats");
        return -1;
    }
    
    return 0;
}

// 获取种子记录
int gsp_fuzz_hook_get_seeds(
    int fd,
    NvU32 startIndex,
    NvU32 count,
    GSP_FUZZ_SEED_RECORD *pSeeds,
    NvU32 *pActualCount
)
{
    struct nv_ioctl_gsp_fuzz_hook_get_seeds req = {0};
    int ret;
    
    req.startIndex = startIndex;
    req.count = count;
    req.seedRecordBufferAddr = (NvU64)(uintptr_t)pSeeds;
    req.seedRecordBufferSize = count * sizeof(GSP_FUZZ_SEED_RECORD);
    
    ret = ioctl(fd, NV_IOCTL_GSP_FUZZ_HOOK_GET_SEEDS, &req);
    if (ret < 0)
    {
        perror("Failed to get seeds");
        return -1;
    }
    
    if (pActualCount != NULL)
    {
        *pActualCount = req.actualCount;
    }
    
    return 0;
}

// 示例：启用Hook并记录种子
int main(int argc, char *argv[])
{
    int fd;
    struct nv_ioctl_gsp_fuzz_hook_stats stats;
    
    fd = open_nvidia_device();
    if (fd < 0)
    {
        return 1;
    }
    
    // 启用Hook并配置
    gsp_fuzz_hook_set_config(
        fd,
        GSP_FUZZ_HOOK_ENABLED | GSP_FUZZ_HOOK_RECORD_SEED,
        1000,  // 最多记录1000个种子
        10     // 10%概率进行在线Fuzz
    );
    
    printf("GSP Fuzz Hook enabled. Waiting for RPC calls...\n");
    
    // 定期打印统计信息
    while (1)
    {
        sleep(5);
        gsp_fuzz_hook_get_stats(fd, &stats);
        printf("Total hooks: %lu, RPC hooks: %lu, Seeds: %lu\n",
               stats.totalHooks, stats.rpcHooks, stats.seedRecords);
    }
    
    close(fd);
    return 0;
}
```

---

## 五、实施步骤总结

### 5.1 阶段1：基础Hook实现 ✅ 已完成

1. ✅ **创建头文件**：`src/nvidia/inc/kernel/gpu/gsp/gsp_fuzz_hook.h`
2. ✅ **创建Hook模块**：`src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c`
3. ✅ **修改resource.c**：在 `rmresControl_Prologue_IMPL` 中插入Hook调用
4. ✅ **添加IOCTL处理**：创建 `gsp_fuzz_ioctl.c` 并在 `nv.c` 中集成
5. ⚠️ **编译测试**：需要集成到构建系统后进行编译测试

### 5.2 阶段2：功能完善 ✅ 已完成

1. ✅ **实现种子记录功能**：`gspFuzzHookRecordSeed` 函数已完善
2. ✅ **实现在线Fuzz**：`gspFuzzHookInlineFuzz` 函数已完善（使用临时拷贝）
3. ✅ **实现用户态接口**：用户态工具 `gsp_fuzz_hook_user.c` 已创建
4. ⚠️ **测试验证**：需要在实际环境中进行测试验证

### 5.3 阶段3：扩展Hook点（可选） ⏸️ 未实现

1. ⏸️ **实现Hook点2**：在 `rpcRmApiControl_GSP` 中添加Hook（可选扩展）
2. ⏸️ **实现Hook点3**：在 `GspMsgQueueSendCommand` 中添加Hook（可选）
3. ⏸️ **优化性能**：减少Hook开销，优化锁竞争

### 5.4 阶段4：集成和优化 ✅ 部分完成

1. ✅ **集成到构建系统**：已更新构建文件，添加新文件到构建系统
   - ✅ `gsp_fuzz_hook.c` 已添加到 `src/nvidia/srcs.mk`（第557行）
   - ✅ `gsp_fuzz_ioctl.c` 已添加到 `kernel-open/nvidia/nvidia-sources.Kbuild`（第63行）
2. ⚠️ **添加配置选项**：可以在配置系统中添加开关（可选）
3. ⚠️ **性能优化**：需要在实际使用中根据性能测试结果进行优化
4. ✅ **文档完善**：本文档已更新，包含完整的实现说明
5. ✅ **构建和测试指南**：已创建 `GSP_FUZZ_HOOK_BUILD_AND_TEST_GUIDE.md`

### 5.5 实际完成情况

**已完成的核心功能**：
- ✅ Hook点1完整实现（`rmresControl_Prologue_IMPL`）
- ✅ 种子记录功能（带参数clamp和线程安全）
- ✅ 在线Fuzz功能（使用临时拷贝，不破坏原始参数）
- ✅ 真实RPC延迟测量
- ✅ IOCTL接口完整实现（5个命令）
- ✅ 权限检查和安全控制
- ✅ 模块初始化集成

**待完成的工作**：
- ✅ 构建系统集成（srcs.mk已更新）✅ 已完成
- ⚠️ 编译验证（需要实际编译测试）
- ⚠️ 实际环境测试（在已安装闭源驱动的情况下测试）
- ⏸️ Hook点2和3（可选扩展）
- ⚠️ 性能优化（基于实际测试结果）

---

## 六、注意事项

### 6.1 线程安全

- 使用自旋锁保护共享数据结构
- 避免在Hook中持有锁过长时间
- 注意中断上下文中的锁使用

### 6.2 内存管理

- 使用 `portMemAllocNonPaged` 分配内核内存
- 避免在Hook中分配大块内存
- ⭐ **关键**：注意内存泄漏问题
  - RPC路径return前必须释放所有分配的临时缓冲区（包括 `pOriginalParamsCopy`）
  - 快照分配失败时，必须将对应的size清零，避免记录"伪seed"

### 6.3 性能影响

- Hook应该尽可能轻量
- 避免在关键路径上进行复杂操作
- 考虑使用条件编译禁用Hook（生产环境）

### 6.4 错误处理

- Hook失败不应该影响正常流程
- 记录错误但不中断执行
- 提供错误统计信息
- ⭐ **关键**：所有IOCTL函数必须检查Hook是否已初始化（锁是否已分配）

### 6.5 安全性

- ⭐ **关键**：GET_CONFIG等接口不得返回内核指针给用户态
- 所有用户态可见的数据必须通过copy_to_user传递
- IOCTL必须进行权限检查（CAP_SYS_ADMIN）

### 6.5 关键安全原则

- **永远先记录原始参数**：种子记录必须使用未变异的原始参数，保证种子的合法性
- **变异使用临时拷贝**：inline fuzz必须对临时缓冲区进行变异，不能修改原始参数
- **参数大小clamp**：所有参数拷贝必须使用clamp，避免越界
- **权限控制**：IOCTL操作必须检查CAP_SYS_ADMIN权限
- **真实延迟测量**：latency统计必须在围绕NV_RM_RPC_CONTROL的调用中测量

---

## 七、测试验证

### 7.1 单元测试

- 测试Hook启用/禁用
- 测试种子记录功能
- 测试在线Fuzz功能
- 测试统计信息收集

### 7.2 集成测试

- 使用真实RPC调用测试
- 验证种子记录的正确性
- 验证在线Fuzz的效果
- 性能测试（Hook开销）

### 7.3 压力测试

- 高并发RPC调用场景
- 长时间运行稳定性
- 内存使用情况
- CPU使用情况

---

## 八、总结

本实现指南提供了方案一（Hook）的完整实现流程，包括：

1. ✅ **完整的数据结构设计**
2. ✅ **详细的Hook实现代码**
3. ✅ **IOCTL接口设计**
4. ✅ **用户态工具实现**
5. ✅ **实施步骤和注意事项**

**关键点**：
- Hook点1（`rmresControl_Prologue_IMPL`）是核心，捕获经过完整RM栈的RPC调用
- Hook点2（`rpcRmApiControl_GSP`）是扩展，捕获所有RPC路径
- 种子记录功能是核心功能，需要线程安全和高效实现
- **在线Fuzz必须使用临时拷贝，不能修改原始参数**，保证种子的合法性
- **参数拷贝必须使用clamp**，避免越界风险
- **Latency统计必须在围绕NV_RM_RPC_CONTROL的调用中测量**，保证准确性
- **IOCTL必须进行权限检查**，防止未授权访问

**下一步**：
1. ✅ 按照阶段1-4逐步实施（已完成）
2. ⚠️ **构建系统集成**（需要完成）
   - 修改 `src/nvidia/srcs.mk` 添加新文件 ✅ 已完成
   - 验证编译通过
3. ⚠️ **测试验证**（需要完成）
   - 在已安装闭源驱动的情况下测试开源驱动
   - 功能测试
   - 稳定性测试
4. 根据实际使用情况优化和调整

**详细构建和测试指南**：请参考 `tools/GSP_FUZZ_HOOK_BUILD_AND_TEST_GUIDE.md`

---

## 九、关键问题修复（基于代码审查）

### 9.1 修复内容总结

根据代码审查，发现并修复了以下关键问题，这些问题会导致Hook功能无法正常工作或记录到错误的种子数据。所有修复已整合到本实现指南中：

#### 9.1.1 问题1：原始请求参数记录错误（关键）✅ 已修复

**问题描述**：
在 `rmresControl_Prologue_IMPL` 中，只保存了原始参数的指针（`pOriginalParams = pParams->pParams`），但很多RM control的params buffer是in/out复用的。RPC返回后，`pParams->pParams` 可能已经被写成"响应内容/输出结构"，导致记录的不是原始请求，而是响应后的buffer。

**修复方案**：
在RPC调用前，对原始请求参数进行快照（复制到临时缓冲区）：
- 分配临时缓冲区：`pOriginalParamsCopy = portMemAllocNonPaged(copySize)`
- 复制原始参数：`portMemCopy(pOriginalParamsCopy, copySize, pParams->pParams, copySize)`
- 如果分配失败，降级处理：只记录元数据，不记录params，并增加错误计数
- 在记录种子时使用快照：`gspFuzzHook_RmresControlPrologueResponse(..., pOriginalParamsCopy, ...)`
- 记录完成后立即释放快照缓冲区

**修改位置**：3.2.2节 - `rmresControl_Prologue_IMPL` 函数

#### 9.1.2 问题2：响应记录在inline fuzz时逻辑错误（关键）✅ 已修复

**问题描述**：
如果启用了inline fuzz，RPC的输出可能写在 `pMutatedParams` 上，而不是 `pParams->pParams`。但代码在记录响应前就把 `pMutatedParams` free了，导致响应记录要么是旧数据，要么丢失。

**修复方案**：
采用方案1（最省事、语义更一致）：
- **如果启用了inline fuzz，禁用响应记录**
- 原因：seed是合法的原始请求，响应是fuzz后的，天然不匹配
- 只有在未启用inline fuzz时才记录响应（响应数据在 `pParams->pParams` 中）
- 如果记录响应，也需要在RPC返回后立即复制响应数据到临时缓冲区

**修改位置**：3.2.2节 - `rmresControl_Prologue_IMPL` 函数

#### 9.1.3 问题3：gspFuzzHookSetConfig不会分配/resize seed ring buffer（关键）✅ 已修复

**问题描述**：
- 全局 `g_hookConfig = {0}`，默认 `maxSeedRecords = 0`
- `gspFuzzHookInit()` 只有在 `maxSeedRecords > 0` 时才分配 `g_pSeedRecordBuffer`
- 但 `gspFuzzHookSetConfig()` 只是memcpy配置并设置enable标志，**完全不做buffer分配/resize**
- 结果：即使通过ioctl设置了 `maxSeedRecords=xxx`，`g_pSeedRecordBuffer` 仍然是NULL，导致 `gspFuzzHookRecordSeed()` 直接返回 `NV_ERR_INSUFFICIENT_RESOURCES`

**修复方案**：
1. **给 `g_hookConfig` 一个合理默认值**：`maxSeedRecords = 1024`
2. **在 `gspFuzzHookSetConfig()` 中检测 `maxSeedRecords` 变化并分配/resize ring buffer**：
   - 如果 `newMaxRecords != oldMaxRecords`，分配新buffer
   - 保存旧buffer以便释放
   - 更新配置和buffer指针
   - 重置 `g_seedRecordIndex = 0`
   - 在锁外释放旧buffer（避免在锁内进行可能阻塞的操作）

**修改位置**：3.2.1节 - `gspFuzzHookSetConfig` 函数

#### 9.1.4 问题4：IOCTL控制面不闭环（关键）✅ 已修复

**问题4.1：命令号体系不一致**

**问题描述**：
- `nv-ioctl-numbers.h`: `NV_ESC_GSP_FUZZ_HOOK = NV_IOCTL_BASE + 19`（也就是219）
- `nv-ioctl.h`: `NV_IOCTL_GSP_FUZZ_HOOK_GET_CONFIG __NV_IOWR(19, ...)` 等
- `nv.c` 中的 `case NV_ESC_GSP_FUZZ_HOOK (219)` 和 `gsp_fuzz_ioctl.c` 中的 `case 19/20/21/22/23` 不匹配

**修复方案**：
采用**方案A（推荐）**：只保留一个 `NV_ESC_GSP_FUZZ_HOOK`，payload里带subcmd（GET_CONFIG/SET_CONFIG/…），不再用19/20/21当ioctl nr做二级分发。

**修改位置**：3.1.2节、3.4节 - IOCTL接口定义和处理函数

**问题4.2：头文件问题**

**问题描述**：
- `nv-ioctl.h` 中使用了 `__NV_IOWR/__NV_IOR/__NV_IO`，但这些宏在用户态头文件中可能未定义
- `nv-ioctl.h` include了 `"gpu/gsp/gsp_fuzz_hook.h"`，这对用户态头文件不合适（会引入内核类型/路径依赖）

**修复方案**：
1. **移除内核头文件include**：从 `nv-ioctl.h` 中移除 `#include "gpu/gsp/gsp_fuzz_hook.h"`
2. **只定义纯数据结构**：在 `nv-ioctl.h` 中只定义用户态可见的数据结构（固定宽度类型）
3. **定义用户态常量**：在 `nv-ioctl.h` 中定义Hook配置标志和子命令常量（与内核保持一致）
4. **定义用户态种子记录结构**：`nv_gsp_fuzz_seed_record_t`（与内核 `GSP_FUZZ_SEED_RECORD` 布局一致）

**修改位置**：3.1.2节 - 用户态接口结构定义

#### 9.1.5 问题5：其他中等问题 ✅ 已修复

**5.1 inlineFuzzProbability未clamp到[0,100]**

**修复**：在 `gspFuzzHookInlineFuzz` 中添加clamp：
```c
NvU32 clampedProbability = NV_MIN(g_hookConfig.inlineFuzzProbability, 100);
```

**修改位置**：3.2.1节 - `gspFuzzHookInlineFuzz` 函数

**5.2 pParams可能为NULL时的健壮性**

**修复**：在 `gspFuzzHookInlineFuzz` 开头添加检查：
```c
if (pOriginalParams == NULL || paramsSize == 0)
{
    if (pMutatedSize != NULL)
        *pMutatedSize = paramsSize;
    return NULL;
}
```

**修改位置**：3.2.1节 - `gspFuzzHookInlineFuzz` 函数

**5.3 统计计数并发**

**修复**：所有统计计数操作都使用锁保护：
```c
portSpinLockAcquire(g_pSeedRecordLock);
g_hookStats.totalHooks++;
portSyncSpinlockRelease(g_pSeedRecordLock);
```

**修改位置**：3.2.1节 - 所有统计计数操作

**5.4 cleanup没接入模块退出**

**修复**：在 `nv_module_exit` 中调用 `gspFuzzHookCleanup()`：
```c
static void
nv_module_exit(nv_stack_t *sp)
{
    // ... 其他清理 ...
    
    // 清理Hook模块
    {
        extern void gspFuzzHookCleanup(void);
        gspFuzzHookCleanup();
    }
    
    // ... 其他清理 ...
}
```

**修改位置**：3.5节 - 模块初始化和清理

#### 9.1.6 问题6：resource.c RPC路径下 `pOriginalParamsCopy` 内存泄漏（关键）✅ 已修复

**问题描述**：
在RPC路径中，代码会释放 `pMutatedParams` 和 `pResponseParamsCopy`，但**没有释放 `pOriginalParamsCopy`**，而且函数在RPC路径会直接 `return ...`。这意味着：只要走到RPC分支（也就是最关心的主链路），每次控制调用都会泄漏一块快照内存。长时间跑fuzz/业务回放会把nonpaged内存吃爆。

**修复方案**：
在RPC分支return前释放 `pOriginalParamsCopy`：
- 位置：放在 `gspFuzzHook_RmresControlPrologueResponse(...)` 调用之后、return之前
- 与 `pResponseParamsCopy` 的释放对齐

**修改位置**：3.2.2节 - `rmresControl_Prologue_IMPL` 函数

**关键代码变化**：
```c
// 释放响应参数快照（如果分配了）
if (pResponseParamsCopy != NULL)
{
    portMemFree(pResponseParamsCopy);
}

// ⭐ 修复：RPC路径下释放原始参数快照（避免内存泄漏）
if (pOriginalParamsCopy != NULL)
{
    portMemFree(pOriginalParamsCopy);
    pOriginalParamsCopy = NULL;
}

return (status == NV_OK) ? NV_WARN_NOTHING_TO_DO : status;
```

#### 9.1.7 问题7：快照分配失败时size未清零 ✅ 已修复

**问题描述**：
快照分配失败时：`pOriginalParamsCopy == NULL`，但 `originalParamsSize` 仍然保留原始size，可能造成"记录有size但params全0/垃圾"的假象。虽然 `gspFuzzHookRecordSeed` 里copy时会检查 `pParams != NULL`，不会crash，但记录结果会变得**难以判读**：`paramsSize>0` 但params内容没意义。

**修复方案**：
- 如果 `pOriginalParamsCopy == NULL`：直接 `originalParamsSize = 0;`
- response copy同理：分配失败就保持 `responseParamsSize = 0`

**修改位置**：3.2.2节 - `rmresControl_Prologue_IMPL` 函数

**关键代码变化**：
```c
// 修复后：分配失败时清零size
pOriginalParamsCopy = portMemAllocNonPaged(copySize);
if (pOriginalParamsCopy != NULL)
{
    portMemCopy(pOriginalParamsCopy, copySize, pParams->pParams, copySize);
    // ...
}
else
{
    // ⭐ 修复：分配失败时清零size，避免"伪seed"（有size但params全0/垃圾）
    originalParamsSize = 0;
}
```

#### 9.1.8 问题8：GET_CONFIG返回内核指针的安全问题 ✅ 已修复

**问题描述**：
在 `gsp_fuzz_ioctl.c` 的 GET_CONFIG 中返回了：`seedRecordBufferAddr = (NvU64)(uintptr_t)kernelConfig.pSeedRecordBuffer`。这有两个问题：
- **信息泄露**：把内核指针暴露给用户态（即使只允许root，也不建议）
- **语义误导**：用户态拿到这个地址也不能直接读（不是映射地址）

**修复方案**：
GET_CONFIG里把这两个字段（addr/size）返回0，明确表示"不暴露内核地址"。真正的数据读取走已有的GET_SEEDS（由内核copy_to_user到用户提供的buffer）。

**修改位置**：3.4.1节 - `gsp_fuzz_ioctl.c` 中的 GET_CONFIG case

**关键代码变化**：
```c
// 修复前：返回内核指针（不安全）
userConfig->seedRecordBufferAddr = (NvU64)(uintptr_t)kernelConfig.pSeedRecordBuffer;
userConfig->seedRecordBufferSize = kernelConfig.seedRecordBufferSize;

// 修复后：不暴露内核地址
userConfig->seedRecordBufferAddr = 0;  // 不暴露内核地址
userConfig->seedRecordBufferSize = 0;   // 不暴露内核buffer大小
```

#### 9.1.9 问题9：SetConfig/GetConfig/GetStats缺少锁的空指针检查 ✅ 已修复

**问题描述**：
在 `SetConfig` / `GetStats` / `GetConfig` 中直接 `portSpinLockAcquire(g_pSeedRecordLock)`，这要求SetConfig调用前必须确保 `gspFuzzHookInit()` 已经跑过并初始化锁。如果未来有人把IOCTL路径放到init之前，或者init失败后仍可触发ioctl，会直接NULL deref。

**修复方案**：
SetConfig / GetStats / GetConfig / ClearStats 都加一层空指针检查：
- `if (g_pSeedRecordLock == NULL) return NV_ERR_INVALID_STATE;` 或者返回默认值

**修改位置**：3.2.1节 - `gspFuzzHookSetConfig` / `gspFuzzHookGetConfig` / `gspFuzzHookGetStats` 函数

**关键代码变化**：
```c
// 修复后：检查锁是否已初始化
NV_STATUS gspFuzzHookSetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig)
{
    // ...
    
    // ⭐ 修复：检查锁是否已初始化（避免NULL deref）
    if (g_pSeedRecordLock == NULL)
    {
        return NV_ERR_INVALID_STATE;  // Hook未初始化
    }
    
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    // ...
}

void gspFuzzHookGetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig)
{
    if (pConfig != NULL)
    {
        // ⭐ 修复：检查锁是否已初始化
        if (g_pSeedRecordLock != NULL)
        {
            portSyncSpinlockAcquire(g_pSeedRecordLock);
            // ...
            portSyncSpinlockRelease(g_pSeedRecordLock);
        }
        else
        {
            // Hook未初始化，返回默认值
            portMemSet(pConfig, 0, sizeof(GSP_FUZZ_HOOK_CONFIG));
        }
    }
}
```

### 9.2 修复后的实现状态

### ✅ 已修复的关键问题
1. ✅ 原始请求参数快照（问题1）
2. ✅ 响应记录逻辑（问题2）
3. ✅ SetConfig中buffer分配/resize（问题3）
4. ✅ IOCTL命令号体系统一（问题4.1）
5. ✅ 头文件问题（问题4.2）
6. ✅ 其他中等问题（问题5）
7. ✅ **RPC路径内存泄漏（问题6）** ⭐ 新增
8. ✅ **快照分配失败时size清零（问题7）** ⭐ 新增
9. ✅ **GET_CONFIG安全修复（问题8）** ⭐ 新增
10. ✅ **锁的空指针检查（问题9）** ⭐ 新增

### ✅ 功能完整性
- ✅ Hook核心功能（seed ring、inline fuzz临时拷贝、clamp、clearStats）
- ✅ `rmresControl_Prologue_IMPL` 注入 + 围绕 `NV_RM_RPC_CONTROL` 计时
- ✅ IOCTL控制面（kernel handler + 编号体系 + 用户态可编译头文件）
- ✅ 种子记录正确性（原始请求快照 + 响应记录逻辑）

### ⚠️ 待验证
- ⚠️ 实际环境测试
- ⚠️ 构建系统集成
- ⚠️ 性能测试

---

## 十、关键修改说明（基于评审反馈）

### 10.1 修改内容总结

根据详细评审，对实现指南进行了以下关键修改：

#### A) Inline Fuzz实现修正（最重要）

**问题**：原实现直接修改原始参数，会破坏正常业务和合法种子。

**修改**：
- ✅ 改为对临时拷贝进行变异，不修改原始参数
- ✅ 先记录原始参数作为合法种子，再对临时拷贝进行变异
- ✅ 使用变异后的临时缓冲区调用RPC，RPC返回后立即释放
- ✅ 记录种子时使用原始未变异的参数

**关键代码变化**：
```c
// 修改前：直接修改原始参数（错误）
gspFuzzHookInlineFuzz(pParams, paramsSize, cmd);  // 直接修改pParams

// 修改后：对临时拷贝变异（正确）
pMutatedParams = gspFuzzHookInlineFuzz(pParams, paramsSize, cmd, &mutatedParamsSize);
// 使用pMutatedParams调用RPC，记录种子时使用pOriginalParams
```

#### B) 参数拷贝Clamp修正

**问题**：请求参数拷贝未做clamp，存在越界风险。

**修改**：
- ✅ 请求参数和响应参数都使用clamp
- ✅ 记录实际记录的大小（clamp后的值）
- ✅ 参数过大时记录错误但继续处理（只记录能记录的部分）

**关键代码变化**：
```c
// 修改前：直接使用paramsSize（可能越界）
portMemCopy(pRecord->params, GSP_FUZZ_MAX_PARAMS_SIZE, pParams, paramsSize);

// 修改后：使用clamp后的长度
NvU32 clampedParamsSize = NV_MIN(paramsSize, GSP_FUZZ_MAX_PARAMS_SIZE);
portMemCopy(pRecord->params, GSP_FUZZ_MAX_PARAMS_SIZE, pParams, clampedParamsSize);
pRecord->paramsSize = clampedParamsSize;  // 记录实际记录的大小
```

#### C) Latency统计位置修正

**问题**：在hook函数内部计时，测不到真实RPC延迟。

**修改**：
- ✅ 移除hook函数内部的latency计算
- ✅ 在`rmresControl_Prologue_IMPL`中围绕`NV_RM_RPC_CONTROL`测量真实延迟
- ✅ 将测量到的真实延迟传递给响应记录函数

**关键代码变化**：
```c
// 修改前：在hook函数内部计时（错误）
startTime = osGetCurrentTimeNs();
// ... hook代码 ...
endTime = osGetCurrentTimeNs();
*pLatencyUs = (endTime - startTime) / 1000;  // 只测到hook执行时间

// 修改后：在调用者中围绕RPC计时（正确）
NvU64 rpcStartTime = osGetCurrentTimeNs();
NV_RM_RPC_CONTROL(...);  // 真实RPC调用
NvU64 rpcEndTime = osGetCurrentTimeNs();
latencyUs = (rpcEndTime - rpcStartTime) / 1000;  // 真实RPC往返时间
```

#### D) IOCTL安全实现完善

**问题**：缺少权限检查、GET_SEEDS未实现、并发控制不完整。

**修改**：
- ✅ 添加CAP_SYS_ADMIN权限检查
- ✅ 完整实现GET_SEEDS的copy_to_user/copy_from_user流程
- ✅ 处理ring buffer的循环读取逻辑
- ✅ 完善统计信息清除的并发控制

**关键代码变化**：
```c
// 添加权限检查
if (!capable(CAP_SYS_ADMIN))
    return -EPERM;

// 完整实现GET_SEEDS
// - 参数验证
// - 分配内核缓冲区
// - 加锁读取ring buffer
// - copy_to_user复制到用户态
// - 释放内核缓冲区
```

### 9.2 修改后的设计原则

1. **永远先记录原始参数**：保证种子的合法性和高质量
2. **变异使用临时拷贝**：不破坏正常业务路径
3. **参数大小clamp**：避免越界和断言失败
4. **真实延迟测量**：在正确位置测量RPC往返时间
5. **权限控制**：防止未授权访问敏感功能

### 9.3 评审意见确认

评审意见**完全正确**，所有指出的问题都已修正：

- ✅ **A) Inline fuzz问题**：已改为临时拷贝变异，先记录原始参数
- ✅ **B) 参数clamp问题**：请求和响应都使用clamp
- ✅ **C) Latency统计问题**：改为在正确位置测量真实RPC延迟
- ✅ **D) IOCTL安全问题**：添加权限检查，完成GET_SEEDS实现

修改后的实现方案现在：
- ✅ **安全**：不会破坏正常业务，不会产生非法种子
- ✅ **准确**：latency统计反映真实RPC延迟
- ✅ **完整**：IOCTL接口完整实现，包含必要的安全检查

---

## 十一、实际实现完成情况

### 11.1 实现状态总结

**已完成部分**（✅）：

1. **3.1.1 种子记录结构** (`src/nvidia/inc/kernel/gpu/gsp/gsp_fuzz_hook.h`)
   - ✅ 数据结构定义完整
   - ✅ 函数声明已添加

2. **3.2.1 Hook模块文件** (`src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c`)
   - ✅ 核心Hook逻辑实现
   - ✅ 种子记录功能实现
   - ✅ 在线Fuzz功能实现
   - ✅ 统计信息收集实现
   - ✅ **修复**：添加了 `#include "nvmisc.h"` 以获取 `NV_MIN` 宏
   - ✅ **新增**：添加了内部状态访问函数（供IOCTL使用）

3. **3.2.2 修改rmresControl_Prologue_IMPL** (`src/nvidia/src/kernel/rmapi/resource.c`)
   - ✅ Hook调用已集成
   - ✅ 真实RPC延迟测量已实现
   - ✅ 临时参数缓冲区管理已实现

4. **3.1.2 用户态接口结构**
   - ✅ 在 `kernel-open/common/inc/nv-ioctl-numbers.h` 中添加了 `NV_ESC_GSP_FUZZ_HOOK` 命令号
   - ✅ 在 `kernel-open/common/inc/nv-ioctl.h` 中添加了完整的IOCTL定义和用户态结构

5. **3.4 IOCTL处理函数实现**
   - ✅ 创建了 `kernel-open/nvidia/gsp_fuzz_ioctl.c` 实现所有IOCTL处理（使用Linux原生函数）
   - ✅ 在 `kernel-open/nvidia/nv.c` 中添加了 `NV_ESC_GSP_FUZZ_HOOK` case处理
   - ✅ 实现了所有5个IOCTL命令的处理逻辑
   - ✅ 实现了权限检查（使用 `NV_IS_SUSER()`）

6. **3.5 模块初始化和清理**
   - ✅ 在 `src/nvidia/src/kernel/gpu/gpu.c` 的 `gpuPostConstruct_IMPL` 中添加了Hook初始化调用
   - ✅ 添加了 `gspFuzzHookClearStats` 函数用于清除统计信息

7. **4.1 用户态库接口** (`tools/gsp_fuzz_hook_user.c`)
   - ✅ 用户态工具实现完整

### 10.2 关键实现细节

#### 10.2.1 头文件修复

**问题**：`gsp_fuzz_hook.c` 中使用了 `NV_MIN` 宏，但未包含定义该宏的头文件。

**修复**：
```c
// 在 gsp_fuzz_hook.c 中添加
#include "nvmisc.h"  // For NV_MIN macro
```

#### 10.2.2 内部状态访问函数

**问题**：IOCTL处理函数需要访问Hook模块的静态全局变量，但这些变量是 `static` 的，无法直接访问。

**解决方案**：在 `gsp_fuzz_hook.c` 中添加了访问函数：

```c
// 导出访问函数（供IOCTL处理使用）
NvU32 gspFuzzHookGetSeedRecordIndex(void);
GSP_FUZZ_SEED_RECORD *gspFuzzHookGetSeedRecordBuffer(void);
PORT_SPINLOCK *gspFuzzHookGetSeedRecordLock(void);
GSP_FUZZ_HOOK_CONFIG *gspFuzzHookGetConfigPtr(void);
GSP_FUZZ_HOOK_STATS *gspFuzzHookGetStatsPtr(void);
void gspFuzzHookClearStats(void);
```

这些函数在 `gsp_fuzz_hook.h` 中声明，供IOCTL处理函数使用。

#### 10.2.3 IOCTL命令定义

**实现位置**：
- `kernel-open/common/inc/nv-ioctl-numbers.h`：添加了 `NV_ESC_GSP_FUZZ_HOOK` 命令号（NV_IOCTL_BASE + 19 = 219）
- `kernel-open/common/inc/nv-ioctl.h`：添加了完整的IOCTL定义和用户态结构

**IOCTL命令映射**（使用subcmd分发）：
- `GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG` → subcmd = 1
- `GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG` → subcmd = 2
- `GSP_FUZZ_HOOK_SUBCMD_GET_STATS` → subcmd = 3
- `GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS` → subcmd = 4
- `GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS` → subcmd = 5

**注意**：所有IOCTL命令都通过统一的 `NV_ESC_GSP_FUZZ_HOOK` 入口，然后在 `nvidia_ioctl_gsp_fuzz_hook` 函数中根据 `req->subcmd` 进一步分发。

#### 10.2.4 IOCTL处理集成

**在 `nv.c` 中的集成**：

```c
case NV_ESC_GSP_FUZZ_HOOK:
{
    // GSP Fuzz Hook IOCTL处理
    // 根据req->subcmd进一步分发到具体的子命令
    extern int nvidia_ioctl_gsp_fuzz_hook(void *arg_copy, size_t arg_size);
    
    status = nvidia_ioctl_gsp_fuzz_hook(arg_copy, arg_size);
    if (status != 0)
    {
        goto done;
    }
    break;
}
```

**权限检查**：在 `gsp_fuzz_ioctl.c` 的 `nvidia_ioctl_gsp_fuzz_hook` 函数开头使用 `NV_IS_SUSER()` 进行检查。

#### 10.2.5 模块初始化

**在 `gpu.c` 中的集成**：

```c
// 在 gpuPostConstruct_IMPL 函数末尾添加
// 初始化GSP Fuzz Hook（如果尚未初始化）
// 注意：这里只初始化一次，因为Hook是全局的，不是per-GPU的
static NvBool g_bGspFuzzHookInitialized = NV_FALSE;
if (!g_bGspFuzzHookInitialized)
{
    NV_STATUS hookStatus = gspFuzzHookInit();
    if (hookStatus == NV_OK)
    {
        g_bGspFuzzHookInitialized = NV_TRUE;
    }
    // 如果初始化失败，记录错误但不阻止GPU初始化
}
```

**注意**：Hook模块是全局的，不是per-GPU的，所以使用静态变量确保只初始化一次。

### 10.3 文件清单

**新建文件**：
1. ✅ `src/nvidia/inc/kernel/gpu/gsp/gsp_fuzz_hook.h` - Hook模块头文件
2. ✅ `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c` - Hook模块实现
3. ✅ `kernel-open/nvidia/gsp_fuzz_ioctl.c` - IOCTL处理函数（使用Linux原生函数）
4. ✅ `tools/gsp_fuzz_hook_user.c` - 用户态工具

**修改的现有文件**：
1. ✅ `kernel-open/common/inc/nv-ioctl-numbers.h` - 添加IOCTL命令号
2. ✅ `kernel-open/common/inc/nv-ioctl.h` - 添加IOCTL定义和结构
3. ✅ `src/nvidia/src/kernel/rmapi/resource.c` - 集成Hook调用
4. ✅ `kernel-open/nvidia/nv.c` - 添加IOCTL case处理
5. ✅ `src/nvidia/src/kernel/gpu/gpu.c` - 添加模块初始化

### 10.4 编译注意事项

1. **构建系统集成**：✅ **已完成**
   - ✅ `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c` 已添加到 `src/nvidia/srcs.mk`（第557行）
   - ✅ `kernel-open/nvidia/gsp_fuzz_ioctl.c` 已添加到 `kernel-open/nvidia/nvidia-sources.Kbuild`（第63行）

2. **头文件路径**：✅ **已验证正确**
   - ✅ `gpu/gsp/gsp_fuzz_hook.h` - 使用 `#include "gpu/gsp/gsp_fuzz_hook.h"`（相对于 `inc/kernel`）
   - ✅ `kernel/gpu/gsp/gsp_fuzz_hook.h` - 在kernel-open层使用 `#include "kernel/gpu/gsp/gsp_fuzz_hook.h"`
   - ✅ `nv-ioctl.h` - 使用 `#include "nv-ioctl.h"`（相对于 `kernel-open/common/inc`）
   - ✅ Makefile/Kbuild中已包含必要的include路径

3. **用户态工具编译**：`tools/gsp_fuzz_hook_user.c` 需要：
   - 包含 `nv-ioctl.h`（需要指定include路径：`-I ../kernel-open/common/inc`）
   - 可能需要定义用户态常量（已在 `nv-ioctl.h` 中定义）

4. **编译命令**：
   ```bash
   # 编译内核模块
   cd /home/lyh/Desktop/open-gpu-kernel-modules
   make modules -j$(nproc)
   
   # 编译用户态工具
   cd tools
   gcc -o gsp_fuzz_hook_user gsp_fuzz_hook_user.c \
       -I../kernel-open/common/inc
   ```

5. **编译结果位置**：✅ **已验证**
   
   编译成功后，内核模块位于 `kernel-open/` 目录下：
   ```
   /home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia.ko          # 主驱动模块
   /home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-modeset.ko  # 显示模式设置模块
   /home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-uvm.ko      # 统一内存模块
   /home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-drm.ko      # DRM 模块
   /home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-peermem.ko  # Peer 内存模块
   ```

6. **一键部署脚本**：
   ```bash
   # 使用部署脚本（需要 sudo 权限）
   cd /home/lyh/Desktop/open-gpu-kernel-modules/tools/make_install
   sudo ./fuzz_deploy.sh
   ```
   
   脚本将自动执行：
   - Step 1: 编译驱动模块
   - Step 2: 卸载旧模块
   - Step 3: 安装新模块到系统目录
   - Step 4: 加载新驱动
   - Step 5: 验证状态

**详细构建和测试步骤**：请参考 `tools/GSP_FUZZ_HOOK_BUILD_AND_TEST_GUIDE.md`

### 10.5 测试建议

1. **编译测试**：确保所有文件可以编译通过
2. **基本功能测试**：
   - 测试Hook启用/禁用
   - 测试种子记录功能
   - 测试IOCTL接口
3. **集成测试**：使用真实RPC调用测试完整流程
4. **性能测试**：测量Hook对RPC性能的影响

### 10.6 已知问题和限制

1. **用户态头文件**：`gsp_fuzz_hook_user.c` 中使用的 `GSP_FUZZ_SEED_RECORD` 和 `GSP_FUZZ_HOOK_ENABLED` 定义在内核头文件中。可能需要：
   - 创建用户态版本的头文件
   - 或者确保用户态工具可以访问内核头文件

2. **模块清理**：当前实现中，Hook模块在GPU初始化时初始化，但清理逻辑尚未实现。如果需要，可以在模块卸载时调用 `gspFuzzHookCleanup()`。

3. **多GPU支持**：当前实现中，Hook是全局的，所有GPU共享同一个Hook实例。如果需要per-GPU的Hook，需要修改设计。

---

## 十二、实施检查清单

### 12.1 代码实现检查

- [x] 3.1.1 种子记录结构定义
- [x] 3.1.2 用户态接口结构定义
- [x] 3.2.1 Hook模块实现
- [x] 3.2.2 resource.c集成
- [x] 3.4 IOCTL处理函数实现
- [x] 3.5 模块初始化
- [x] 4.1 用户态工具实现

### 12.2 关键修复检查

- [x] Inline Fuzz使用临时拷贝
- [x] 参数拷贝使用clamp
- [x] Latency统计在正确位置
- [x] IOCTL权限检查
- [x] GET_SEEDS完整实现
- [x] 头文件包含修复
- [x] 原始请求参数快照（问题1）
- [x] 响应记录逻辑（问题2）
- [x] SetConfig中buffer分配/resize（问题3）
- [x] IOCTL命令号体系统一（问题4.1）
- [x] 头文件问题（问题4.2）
- [x] inlineFuzzProbability clamp（问题5.1）
- [x] pParams为NULL检查（问题5.2）
- [x] 统计计数并发保护（问题5.3）
- [x] cleanup接入模块退出（问题5.4）

### 12.3 构建系统检查

- [x] **新文件添加到构建系统** ✅ 已完成
  - [x] `gsp_fuzz_hook.c` 已添加到 `src/nvidia/srcs.mk`（第557行）
  - [x] `gsp_fuzz_ioctl.c` 已添加到 `kernel-open/nvidia/nvidia-sources.Kbuild`（第63行）
- [x] **头文件路径验证** ✅ 已验证
  - [x] `gpu/gsp/gsp_fuzz_hook.h` - 路径正确（相对于 `inc/kernel`）
  - [x] `nv-ioctl.h` - 路径正确（相对于 `arch/nvalloc/unix/include`）
  - [x] Makefile中已包含必要的include路径
- [x] **编译选项配置** ✅ 已配置
  - [x] Makefile中已包含所有必要的include路径
  - [x] 无需额外编译选项
- [ ] **用户态工具编译配置** ⚠️ 需要测试
  - [ ] 验证用户态工具可以编译
  - [ ] 验证头文件在用户态可用

### 12.4 构建系统检查

- [x] `gsp_fuzz_hook.c` 已添加到 `src/nvidia/srcs.mk`（第557行）
- [x] `gsp_fuzz_ioctl.c` 已添加到 `kernel-open/nvidia/nvidia-sources.Kbuild`（第63行）
- [x] 所有头文件路径正确
- [x] 编译测试通过 ✅ **2024-12-15 已验证**

### 12.5 测试检查

- [x] 编译测试通过 ✅
- [ ] 基本功能测试
- [ ] 集成测试
- [ ] 性能测试

### 12.6 编译结果

✅ **编译成功**（2024-12-15）

编译后的内核模块位于：
```
kernel-open/nvidia.ko
kernel-open/nvidia-modeset.ko
kernel-open/nvidia-uvm.ko
kernel-open/nvidia-drm.ko
kernel-open/nvidia-peermem.ko
```

**部署方法**：
```bash
cd tools/make_install
sudo ./fuzz_deploy.sh
```

