# GSP Fuzz Hook 关键问题修复总结

## 修复日期
2024-12-15

## 编译状态

✅ **编译成功**

编译后的内核模块位于：
```
/home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia.ko
/home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-modeset.ko
/home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-uvm.ko
/home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-drm.ko
/home/lyh/Desktop/open-gpu-kernel-modules/kernel-open/nvidia-peermem.ko
```

### 部署方法
```bash
cd /home/lyh/Desktop/open-gpu-kernel-modules/tools/make_install
sudo ./fuzz_deploy.sh
```

**注意**：`fuzz_deploy.sh` 需要 **sudo 权限** 来安装模块到系统目录。

## 架构说明

此实现遵循 NVIDIA 驱动的分层架构：

```
┌─────────────────────────────────────────────────────────────┐
│                     kernel-open 层                          │
│  gsp_fuzz_ioctl.c - Linux 原生函数 (kzalloc, kfree)         │
│  调用 OS-agnostic 层函数 ↓                                  │
└─────────────────────────────────────────────────────────────┘
                                   │
                                   ↓
┌─────────────────────────────────────────────────────────────┐
│                   OS-agnostic 层 (nv-kernel.o)              │
│  gsp_fuzz_hook.c - nvport 函数 (portSyncSpinlock*)          │
│  resource.c - Hook点1 调用                                  │
└─────────────────────────────────────────────────────────────┘
```

**文件位置说明**：
- `gsp_fuzz_hook.c` 在 `src/nvidia/` 层（OS-agnostic），使用 nvport 函数
- `gsp_fuzz_ioctl.c` 在 `kernel-open/` 层，使用 Linux 原生函数
- 符号通过 `exports_link_command.txt` 导出，避免被死代码消除优化删除

**跨层调用辅助函数**：
- `gspFuzzHookCopySeedsLocked()` - 在OS-agnostic层实现，封装了spinlock操作和ring buffer读取
- kernel-open层通过调用此函数避免直接使用nvport spinlock函数

## 修复概述

根据代码审查，发现并修复了以下关键问题，这些问题会导致Hook功能无法正常工作或记录到错误的种子数据。

---

## 问题1：原始请求参数记录错误（关键）✅ 已修复

### 问题描述
在 `rmresControl_Prologue_IMPL` 中，只保存了原始参数的指针（`pOriginalParams = pParams->pParams`），但很多RM control的params buffer是in/out复用的。RPC返回后，`pParams->pParams` 可能已经被写成"响应内容/输出结构"，导致记录的不是原始请求，而是响应后的buffer。

### 修复方案
在RPC调用前，对原始请求参数进行快照（复制到临时缓冲区）：
- 分配临时缓冲区：`pOriginalParamsCopy = portMemAllocNonPaged(copySize)`
- 复制原始参数：`portMemCopy(pOriginalParamsCopy, copySize, pParams->pParams, copySize)`
- 如果分配失败，降级处理：只记录元数据，不记录params，并增加错误计数
- 在记录种子时使用快照：`gspFuzzHook_RmresControlPrologueResponse(..., pOriginalParamsCopy, ...)`
- 记录完成后立即释放快照缓冲区

### 修改文件
- `src/nvidia/src/kernel/rmapi/resource.c`

### 关键代码变化
```c
// 修复前：只保存指针
pOriginalParams = pParams->pParams;
originalParamsSize = pParams->paramsSize;

// 修复后：复制原始参数快照
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
}
```

---

## 问题2：响应记录在inline fuzz时逻辑错误（关键）✅ 已修复

### 问题描述
如果启用了inline fuzz，RPC的输出可能写在 `pMutatedParams` 上，而不是 `pParams->pParams`。但代码在记录响应前就把 `pMutatedParams` free了，导致响应记录要么是旧数据，要么丢失。

### 修复方案
采用方案1（最省事、语义更一致）：
- **如果启用了inline fuzz，禁用响应记录**
- 原因：seed是合法的原始请求，响应是fuzz后的，天然不匹配
- 只有在未启用inline fuzz时才记录响应（响应数据在 `pParams->pParams` 中）
- 如果记录响应，也需要在RPC返回后立即复制响应数据到临时缓冲区

### 修改文件
- `src/nvidia/src/kernel/rmapi/resource.c`

### 关键代码变化
```c
// 修复后：如果启用了inline fuzz，禁用响应记录
void *pResponseParamsCopy = NULL;
NvU32 responseParamsCopySize = 0;

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
    }
}
```

---

## 问题3：gspFuzzHookSetConfig不会分配/resize seed ring buffer（关键）✅ 已修复

### 问题描述
- 全局 `g_hookConfig = {0}`，默认 `maxSeedRecords = 0`
- `gspFuzzHookInit()` 只有在 `maxSeedRecords > 0` 时才分配 `g_pSeedRecordBuffer`
- 但 `gspFuzzHookSetConfig()` 只是memcpy配置并设置enable标志，**完全不做buffer分配/resize**
- 结果：即使通过ioctl设置了 `maxSeedRecords=xxx`，`g_pSeedRecordBuffer` 仍然是NULL，导致 `gspFuzzHookRecordSeed()` 直接返回 `NV_ERR_INSUFFICIENT_RESOURCES`

### 修复方案
1. **给 `g_hookConfig` 一个合理默认值**：`maxSeedRecords = 1024`
2. **在 `gspFuzzHookSetConfig()` 中检测 `maxSeedRecords` 变化并分配/resize ring buffer**：
   - 如果 `newMaxRecords != oldMaxRecords`，分配新buffer
   - 保存旧buffer以便释放
   - 更新配置和buffer指针
   - 重置 `g_seedRecordIndex = 0`
   - 在锁外释放旧buffer（避免在锁内进行可能阻塞的操作）

### 修改文件
- `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c`

### 关键代码变化
```c
// 修复前：只更新配置
portMemCopy(&g_hookConfig, sizeof(GSP_FUZZ_HOOK_CONFIG), pConfig, sizeof(GSP_FUZZ_HOOK_CONFIG));
g_bHookEnabled = (g_hookConfig.flags & GSP_FUZZ_HOOK_ENABLED) != 0;

// 修复后：检测maxSeedRecords变化并分配/resize buffer
oldMaxRecords = g_hookConfig.maxSeedRecords;
newMaxRecords = pConfig->maxSeedRecords;

if (newMaxRecords != oldMaxRecords)
{
    if (newMaxRecords > 0)
    {
        // 分配新buffer
        pNewBuffer = portMemAllocNonPaged(newMaxRecords * sizeof(GSP_FUZZ_SEED_RECORD));
        // ... 错误处理 ...
    }
    
    // 保存旧buffer以便释放
    pOldBuffer = g_pSeedRecordBuffer;
    
    // 更新配置和buffer
    g_pSeedRecordBuffer = pNewBuffer;
    g_seedRecordIndex = 0;  // 重置索引
    
    // 在锁外释放旧buffer
    if (pOldBuffer != NULL)
    {
        portMemFree(pOldBuffer);
    }
}
```

---

## 问题4：IOCTL控制面不闭环（关键）✅ 已修复

### 问题4.1：命令号体系不一致

#### 问题描述
- `nv-ioctl-numbers.h`: `NV_ESC_GSP_FUZZ_HOOK = NV_IOCTL_BASE + 19`（也就是219）
- `nv-ioctl.h`: `NV_IOCTL_GSP_FUZZ_HOOK_GET_CONFIG __NV_IOWR(19, ...)` 等
- `nv.c` 中的 `case NV_ESC_GSP_FUZZ_HOOK (219)` 和 `gsp_fuzz_ioctl.c` 中的 `case 19/20/21/22/23` 不匹配

#### 修复方案
采用**方案A（推荐）**：只保留一个 `NV_ESC_GSP_FUZZ_HOOK`，payload里带subcmd（GET_CONFIG/SET_CONFIG/…），不再用19/20/21当ioctl nr做二级分发。

**修改内容**：
1. 在 `nv-ioctl.h` 中定义子命令常量：
   ```c
   #define GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG    1
   #define GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG    2
   #define GSP_FUZZ_HOOK_SUBCMD_GET_STATS     3
   #define GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS     4
   #define GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS   5
   ```

2. 定义统一的请求结构：
   ```c
   typedef struct nv_ioctl_gsp_fuzz_hook_request
   {
       NvU32 subcmd;                   // 子命令
       union {
           nv_ioctl_gsp_fuzz_hook_config_t config;
           nv_ioctl_gsp_fuzz_hook_stats_t stats;
           nv_ioctl_gsp_fuzz_hook_get_seeds_t get_seeds;
       } u;
   } nv_ioctl_gsp_fuzz_hook_request_t;
   ```

3. 修改 `gsp_fuzz_ioctl.c` 中的处理函数：
   - 函数签名改为：`int nvidia_ioctl_gsp_fuzz_hook(void *arg_copy, size_t arg_size)`
   - 根据 `req->subcmd` 分发，而不是 `_IOC_NR(cmd)`

4. 修改 `nv.c` 中的调用：
   - 调用 `nvidia_ioctl_gsp_fuzz_hook(arg_copy, arg_size)`（不再传递cmd）

### 问题4.2：头文件问题

#### 问题描述
- `nv-ioctl.h` 中使用了 `__NV_IOWR/__NV_IOR/__NV_IO`，但这些宏在用户态头文件中可能未定义
- `nv-ioctl.h` include了 `"gpu/gsp/gsp_fuzz_hook.h"`，这对用户态头文件不合适（会引入内核类型/路径依赖）

#### 修复方案
1. **移除内核头文件include**：从 `nv-ioctl.h` 中移除 `#include "gpu/gsp/gsp_fuzz_hook.h"`
2. **只定义纯数据结构**：在 `nv-ioctl.h` 中只定义用户态可见的数据结构（固定宽度类型）
3. **定义用户态常量**：在 `nv-ioctl.h` 中定义Hook配置标志和子命令常量（与内核保持一致）
4. **定义用户态种子记录结构**：`nv_gsp_fuzz_seed_record_t`（与内核 `GSP_FUZZ_SEED_RECORD` 布局一致）

### 修改文件
- `kernel-open/nvidia/gsp_fuzz_ioctl.c`（IOCTL处理，使用Linux原生函数）
- `kernel-open/nvidia/nv.c`
- `tools/gsp_fuzz_hook_user.c`

---

## 问题5：其他中等问题 ✅ 已修复

### 5.1 inlineFuzzProbability未clamp到[0,100]

**修复**：在 `gspFuzzHookInlineFuzz` 中添加clamp：
```c
NvU32 clampedProbability = NV_MIN(g_hookConfig.inlineFuzzProbability, 100);
```

### 5.2 pParams可能为NULL时的健壮性

**修复**：在 `gspFuzzHookInlineFuzz` 开头添加检查：
```c
if (pOriginalParams == NULL || paramsSize == 0)
{
    if (pMutatedSize != NULL)
        *pMutatedSize = paramsSize;
    return NULL;
}
```

### 5.3 统计计数并发

**修复**：所有统计计数操作都使用锁保护：
```c
portSpinLockAcquire(g_pSeedRecordLock);
g_hookStats.totalHooks++;
portSpinLockRelease(g_pSeedRecordLock);
```

### 5.4 cleanup没接入模块退出

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

### 修改文件
- `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c`
- `kernel-open/nvidia/nv.c`

---

## 问题6：resource.c RPC路径下 `pOriginalParamsCopy` 内存泄漏（关键）✅ 已修复

### 问题描述
在RPC路径中，代码会释放 `pMutatedParams` 和 `pResponseParamsCopy`，但**没有释放 `pOriginalParamsCopy`**，而且函数在RPC路径会直接 `return ...`。这意味着：只要走到RPC分支（也就是最关心的主链路），每次控制调用都会泄漏一块快照内存。长时间跑fuzz/业务回放会把nonpaged内存吃爆。

### 修复方案
在RPC分支return前释放 `pOriginalParamsCopy`：
- 位置：放在 `gspFuzzHook_RmresControlPrologueResponse(...)` 调用之后、return之前
- 与 `pResponseParamsCopy` 的释放对齐

### 修改文件
- `src/nvidia/src/kernel/rmapi/resource.c`

### 关键代码变化
```c
// 修复后：在RPC路径return前释放原始参数快照
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

---

## 问题7：快照分配失败时size未清零 ✅ 已修复

### 问题描述
快照分配失败时：`pOriginalParamsCopy == NULL`，但 `originalParamsSize` 仍然保留原始size，可能造成"记录有size但params全0/垃圾"的假象。虽然 `gspFuzzHookRecordSeed` 里copy时会检查 `pParams != NULL`，不会crash，但记录结果会变得**难以判读**：`paramsSize>0` 但params内容没意义。

### 修复方案
- 如果 `pOriginalParamsCopy == NULL`：直接 `originalParamsSize = 0;`
- response copy同理：分配失败就保持 `responseParamsSize = 0`

### 修改文件
- `src/nvidia/src/kernel/rmapi/resource.c`

### 关键代码变化
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

---

## 问题8：GET_CONFIG返回内核指针的安全问题 ✅ 已修复

### 问题描述
在 `gsp_fuzz_ioctl.c` 的 GET_CONFIG 中返回了：`seedRecordBufferAddr = (NvU64)(uintptr_t)kernelConfig.pSeedRecordBuffer`。这有两个问题：
- **信息泄露**：把内核指针暴露给用户态（即使只允许root，也不建议）
- **语义误导**：用户态拿到这个地址也不能直接读（不是映射地址）

### 修复方案
GET_CONFIG里把这两个字段（addr/size）返回0，明确表示"不暴露内核地址"。真正的数据读取走已有的GET_SEEDS（由内核copy_to_user到用户提供的buffer）。

### 修改文件
- `kernel-open/nvidia/gsp_fuzz_ioctl.c`（IOCTL处理，使用Linux原生函数）

### 关键代码变化
```c
// 修复前：返回内核指针（不安全）
userConfig->seedRecordBufferAddr = (NvU64)(uintptr_t)kernelConfig.pSeedRecordBuffer;
userConfig->seedRecordBufferSize = kernelConfig.seedRecordBufferSize;

// 修复后：不暴露内核地址
userConfig->seedRecordBufferAddr = 0;  // 不暴露内核地址
userConfig->seedRecordBufferSize = 0;   // 不暴露内核buffer大小
```

---

## 问题9：SetConfig/GetConfig/GetStats缺少锁的空指针检查 ✅ 已修复

### 问题描述
在 `SetConfig` / `GetStats` / `GetConfig` 中直接 `portSpinLockAcquire(g_pSeedRecordLock)`，这要求SetConfig调用前必须确保 `gspFuzzHookInit()` 已经跑过并初始化锁。如果未来有人把IOCTL路径放到init之前，或者init失败后仍可触发ioctl，会直接NULL deref。

### 修复方案
SetConfig / GetStats / GetConfig / ClearStats 都加一层空指针检查：
- `if (g_pSeedRecordLock == NULL) return NV_ERR_INVALID_STATE;` 或者返回默认值

### 修改文件
- `src/nvidia/src/kernel/gpu/gsp/gsp_fuzz_hook.c`

### 关键代码变化
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
    
    portSpinLockAcquire(g_pSeedRecordLock);
    // ...
}

void gspFuzzHookGetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig)
{
    if (pConfig != NULL)
    {
        // ⭐ 修复：检查锁是否已初始化
        if (g_pSeedRecordLock != NULL)
        {
            portSpinLockAcquire(g_pSeedRecordLock);
            // ...
            portSpinLockRelease(g_pSeedRecordLock);
        }
        else
        {
            // Hook未初始化，返回默认值
            portMemSet(pConfig, 0, sizeof(GSP_FUZZ_HOOK_CONFIG));
        }
    }
}
```

---

## 修复后的实现状态

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

### ✅ 已修复的关键问题
1. ✅ 原始请求参数快照（问题1）
2. ✅ 响应记录逻辑（问题2）
3. ✅ SetConfig中buffer分配/resize（问题3）
4. ✅ IOCTL命令号体系统一（问题4.1）
5. ✅ 头文件问题（问题4.2）
6. ✅ 其他中等问题（问题5）

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

## 测试建议

1. **编译测试**：确保所有文件可以编译通过
2. **基本功能测试**：
   - 测试Hook启用/禁用
   - 测试种子记录功能（验证记录的是原始请求，不是响应）
   - 测试IOCTL接口（使用新的统一接口）
   - 测试SetConfig后buffer是否正确分配
3. **集成测试**：使用真实RPC调用测试完整流程
4. **并发测试**：多线程环境下测试统计计数和种子记录的线程安全性

---

## 注意事项

1. **用户态和内核态结构体一致性**：
   - `nv_gsp_fuzz_seed_record_t` 必须与 `GSP_FUZZ_SEED_RECORD` 布局完全一致
   - 如果内核结构体发生变化，必须同步更新用户态结构体

2. **内存管理**：
   - 所有临时缓冲区（原始参数快照、响应快照）必须在使用后立即释放
   - ⭐ **关键**：RPC路径return前必须释放所有分配的临时缓冲区（包括 `pOriginalParamsCopy`）
   - SetConfig中的buffer resize需要在锁外释放旧buffer
   - 快照分配失败时，必须将对应的size清零，避免记录"伪seed"

3. **线程安全**：
   - 所有统计计数操作都必须使用锁保护
   - 种子记录的ring buffer访问必须使用锁保护

4. **错误处理**：
   - 如果原始参数快照分配失败，降级为只记录元数据（size必须清零）
   - 如果buffer分配失败，返回适当的错误码
   - ⭐ **关键**：所有IOCTL函数必须检查Hook是否已初始化（锁是否已分配）

5. **安全性**：
   - ⭐ **关键**：GET_CONFIG等接口不得返回内核指针给用户态
   - 所有用户态可见的数据必须通过copy_to_user传递

