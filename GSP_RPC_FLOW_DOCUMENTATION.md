# NVIDIA 驱动与 GSP 交互全链路流程文档

## 目录

- [场景示例](#场景示例)
- [第一阶段：用户态发起 & 内核适配层 (User Space → Kernel Boundary)](#第一阶段用户态发起--内核适配层-user-space--kernel-boundary)
  - [1.1 用户态准备参数](#11-用户态准备参数)
  - [1.2 系统调用入口](#12-系统调用入口)
  - [1.3 Escape 层转换](#13-escape-层转换)
  - [1.4 RMAPI 入口点](#14-rmapi-入口点)
- [第二阶段：RMAPI 路由与资源服务层 (RMAPI → ResServ)](#第二阶段rmapi-路由与资源服务层-rmapi--resserv)
  - [2.1 RMAPI 控制入口](#21-rmapi-控制入口)
  - [2.2 RMAPI 内部控制处理](#22-rmapi-内部控制处理)
  - [2.3 资源服务器分发](#23-资源服务器分发)
- [第三阶段：对象分发与多态跳转 (NVOC Object Dispatch)](#第三阶段对象分发与多态跳转-nvoc-object-dispatch)
  - [3.1 虚函数入口](#31-虚函数入口)
  - [3.2 GPU 资源层处理](#32-gpu-资源层处理)
  - [3.3 查表分发](#33-查表分发)
  - [3.4 RPC 路由拦截（关键步骤）](#34-rpc-路由拦截关键步骤)
  - [3.4.1 标志获取机制](#341-标志获取机制)
  - [3.5 本地执行路径 vs GSP 卸载路径（重要对比）](#35-本地执行路径-vs-gsp-卸载路径重要对比)
- [第四阶段：业务实现与 RPC 传输 (Business Logic & RPC)](#第四阶段业务实现与-rpc-传输-business-logic--rpc)
  - [4.1 RPC 控制宏](#41-rpc-控制宏)
  - [4.2 GSP RPC 控制实现](#42-gsp-rpc-控制实现)
  - [4.3 RPC 发送消息](#43-rpc-发送消息)
  - [4.4 消息队列发送命令](#44-消息队列发送命令)
  - [4.5 触发硬件中断](#45-触发硬件中断)
- [第五阶段：固件执行与响应 (Firmware Execution & Return)](#第五阶段固件执行与响应-firmware-execution--return)
  - [5.1 GSP 固件执行（固件端）](#51-gsp-固件执行固件端)
  - [5.2 RPC 接收轮询](#52-rpc-接收轮询)
  - [5.3 消息队列接收状态](#53-消息队列接收状态)
  - [5.4 RPC 响应处理](#54-rpc-响应处理)
  - [5.5 层层返回](#55-层层返回)
- [关键数据结构](#关键数据结构)
- [完整执行时序总结](#完整执行时序总结)
  - [满足 RPC 条件的完整执行流程](#满足-rpc-条件的完整执行流程)
  - [不满足 RPC 条件的执行流程](#不满足-rpc-条件的执行流程)
- [关键要点总结](#关键要点总结)
  - [设计说明](#设计说明)
- [文件路径索引](#文件路径索引)

## 场景示例
驱动程序查询 GSP 功能特性：`NV2080_CTRL_CMD_GSP_GET_FEATURES` (0x20803601)

---

## 第一阶段：用户态发起 & 内核适配层 (User Space → Kernel Boundary)

### 1.1 用户态准备参数
**位置**: 用户空间应用程序  
**作用**: 分配并填充 `NV2080_CTRL_GSP_GET_FEATURES_PARAMS` 结构体，包含：
- `gspFeatures`: GSP 功能位掩码（输出）
- `bValid`: 有效性标志（输出）
- `bDefaultGspRmGpu`: 默认 GSP-RM 标志（输出）
- `firmwareVersion`: 固件版本字符串（输出）

### 1.2 系统调用入口
**文件**: `kernel-open/nvidia/nv.c`  
**函数**: `nvidia_ioctl` (行 2419)  
**作用**: 
- 接收用户态 `ioctl(fd, NV_ESC_RM_CONTROL, &params)` 调用
- 进行基本的权限检查
- 从用户空间复制参数到内核空间
- 将控制权转发到 escape 层处理

**说明**: `NV_ESC_RM_CONTROL` 的实际处理在 escape 层（见 1.3 节），`nvidia_ioctl` 主要负责参数复制和转发。

### 1.3 Escape 层转换
**文件**: `src/nvidia/arch/nvalloc/unix/src/escape.c`  
**函数**: `Nv04ControlWithSecInfo` (行 759)  
**作用**: 
- 将 Linux 特定的 `file_private` 数据转换为 RM 内部的 `hClient` 句柄
- 构建 `API_SECURITY_INFO` 安全信息结构
- 准备调用 Core RM 的统一接口

**关键代码**:
```c
// 行 711-772: 处理 NV_ESC_RM_CONTROL
case NV_ESC_RM_CONTROL:
{
    NVOS54_PARAMETERS *pApi = data;
    // ... 获取 file_private 并转换为 hClient ...
    secInfo.gpuOsInfo = priv;
    Nv04ControlWithSecInfo(pApi, secInfo);  // 行 759
}
```

### 1.4 RMAPI 入口点
**文件**: `src/nvidia/src/kernel/rmapi/entry_points.c`  
**函数**: `_nv04ControlWithSecInfo` (行 493)  
**作用**: 
- 将 `NVOS54_PARAMETERS` 转换为 RMAPI 调用
- 获取 RMAPI 接口实例
- 调用 `rmapiControlWithSecInfo`

**关键代码**:
```c
// 行 509-512: 调用 RMAPI
RM_API *pRmApi = rmapiGetInterface(bInternalCall ? RMAPI_MODS_LOCK_BYPASS : RMAPI_EXTERNAL);
pArgs->status = pRmApi->ControlWithSecInfo(pRmApi, pArgs->hClient, pArgs->hObject, 
                                           pArgs->cmd, pArgs->params, pArgs->paramsSize, 
                                           pArgs->flags, &secInfo);
```

---

## 第二阶段：RMAPI 路由与资源服务层 (RMAPI → ResServ)

### 2.1 RMAPI 控制入口
**文件**: `src/nvidia/src/kernel/rmapi/control.c`  
**函数**: `rmapiControlWithSecInfo` (行 1034)  
**作用**: 
- Core RM 的对外统一接口
- 记录日志（hClient, hObject, cmd）
- 调用内部实现 `_rmapiRmControl`

**关键代码**:
```c
// 行 1048-1052: 记录日志并转发
NV_PRINTF(LEVEL_INFO, "Nv04Control: hClient:0x%x hObject:0x%x cmd:0x%x ...\n",
          hClient, hObject, cmd, ...);
status = _rmapiRmControl(hClient, hObject, cmd, pParams, paramsSize, flags, pRmApi, pSecInfo);
```

### 2.2 RMAPI 内部控制处理
**文件**: `src/nvidia/src/kernel/rmapi/control.c`  
**函数**: `_rmapiRmControl` (行 350)  
**作用**: 
- 验证命令参数（NULL 命令检查、参数大小验证）
- 确定命令模式（lock bypass / raised IRQL / normal）
- 初始化 `RmCtrlParams` 结构
- 获取控制命令信息（flags, access rights）
- 调用 `serverControl` 进入资源服务器

**关键代码**:
```c
// 行 464-478: 初始化控制参数
portMemSet(&rmCtrlParams, 0, sizeof(rmCtrlParams));
rmCtrlParams.hClient = hClient;
rmCtrlParams.hObject = hObject;
rmCtrlParams.cmd = cmd;
rmCtrlParams.pParams = NvP64_VALUE(pUserParams);
rmCtrlParams.paramsSize = paramsSize;
rmCtrlParams.secInfo = *pSecInfo;
rmCtrlParams.pLockInfo = &lockInfo;
rmCtrlParams.pCookie = &rmCtrlExecuteCookie;

// 行 516: 调用资源服务器
rmStatus = serverControl(&g_resServ, &rmCtrlParams);
```

### 2.3 资源服务器分发
**文件**: `src/nvidia/src/libraries/resserv/src/rs_server.c`  
**函数**: `serverControl` (行 1453)  
**作用**: 
- **锁定管理**: 调用 `serverTopLock_Prologue` 获取 Top Lock (API Lock)
- **客户端查找**: 调用 `_serverLockClientWithLockInfo` 获取 Client 锁并查找客户端
- **资源查找**: 调用 `clientGetResourceRef` 根据 `hSubDevice` 在哈希表中查找资源对象引用
- **上下文设置**: 调用 `resservSwapTlsCallContext` 设置线程局部存储 (TLS)，确保后续代码能通过全局变量访问当前上下文
- **虚函数调用**: 调用 `resControl` 进入对象的多态分发

**关键代码**:
```c
// 行 1486-1488: 获取 Top Lock
status = serverTopLock_Prologue(pServer, access, pLockInfo, &releaseFlags);

// 行 1494-1550: 获取 Client 锁并查找客户端
status = _serverLockClientWithLockInfo(...);

// 行 1551-1554: 查找资源对象
status = clientGetResourceRef(pClient, pParams->hObject, &pResourceRef);

// 行 1578-1600: 设置调用上下文
portMemSet(&callContext, 0, sizeof(callContext));
callContext.pResourceRef = pResourceRef;
callContext.pClient = pClient;
callContext.secInfo = pParams->secInfo;
callContext.pServer = pServer;
NV_ASSERT_OK_OR_GOTO(status, resservSwapTlsCallContext(&pOldContext, &callContext), done);

// 行 1602: 调用资源对象的 control 方法（多态入口）
status = resControl(pResourceRef->pResource, &callContext, pParams);
```

---

## 第三阶段：对象分发与多态跳转 (NVOC Object Dispatch)

### 3.1 虚函数入口
**文件**: `src/nvidia/inc/libraries/resserv/rs_resource.h`  
**宏定义**: `resControl` (行 292)  
**作用**: 
- 通过 NVOC (NVIDIA Object Model) 的虚表 (vtable) 机制
- 调用对象基类 `RsResource` 的 `resControl` 虚函数
- 由于 `pResource` 实际上是 `Subdevice` 对象，触发多态跳转

**实现机制**: NVOC 生成的 Thunk 函数会将 `RsResource` 指针向下转型为具体类型（如 `GpuResource`、`Subdevice`）

### 3.2 GPU 资源层处理
**文件**: `src/nvidia/src/kernel/gpu/gpu_resource.c`  
**函数**: `gpuresControl_IMPL` (行 393)  
**作用**: 
- 执行 GPU 相关的通用检查（如电源状态检查、GPU 是否丢失）
- 设置 `pParams->pGpu` 指针
- 调用通用的 `resControl_IMPL` 进行查表分发

**关键代码**:
```c
// 行 400-404: GPU 检查和转发
NV_ASSERT_OR_RETURN(pGpuResource->pGpu != NULL, NV_ERR_INVALID_STATE);
gpuresControlSetup(pParams, pGpuResource);
return resControl_IMPL(staticCast(pGpuResource, RsResource), pCallContext, pParams);
```

### 3.3 查表分发
**文件**: `src/nvidia/src/libraries/resserv/src/rs_resource.c`  
**函数**: `resControl_IMPL` (行 152)  
**作用**: 
- 使用命令 ID (`NV2080_CTRL_CMD_GSP_GET_FEATURES`) 在 Subdevice 类的 NVOC 导出方法表中查找
- 找到对应的处理函数指针：`subdeviceCtrlCmdGspGetFeatures_IMPL`
- 执行 Prologue 钩子（**关键：RPC 路由在这里发生**）
- 调用具体的处理函数

**关键代码**:
```c
// 行 166: 查找命令处理函数
status = resControlLookup(pResource, pRsParams, &pEntry);

// 行 187: 序列化 Prologue
status = resControlSerialization_Prologue(pResource, pCallContext, pRsParams);

// 行 191: 资源 Prologue（RPC 路由发生在这里）
status = resControl_Prologue(pResource, pCallContext, pRsParams);

// 行 209-216: 调用具体的处理函数
if (pEntry->paramSize == 0) {
    CONTROL_EXPORT_FNPTR_NO_PARAMS pFunc = ((CONTROL_EXPORT_FNPTR_NO_PARAMS) pEntry->pFunc);
    status = pFunc(pDynamicObj);
} else {
    CONTROL_EXPORT_FNPTR pFunc = ((CONTROL_EXPORT_FNPTR) pEntry->pFunc);
    status = pFunc(pDynamicObj, pRsParams->pParams);
}
```

### 3.4 RPC 路由拦截（关键步骤）
**文件**: `src/nvidia/src/kernel/rmapi/resource.c`  
**函数**: `rmresControl_Prologue_IMPL` (行 254)  
**作用**: 
- **检查是否需要 RPC 路由**: 判断 `IS_GSP_CLIENT(pGpu)` 且命令带有 `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` 标志
- **如果匹配**: 直接调用 `NV_RM_RPC_CONTROL` 宏，**跳过**后续的具体处理函数调用
- **返回**: `NV_WARN_NOTHING_TO_DO` 表示已在 Prologue 阶段处理完成

**关键代码**:
```c
// 行 264-266: 检查是否需要 RPC 路由
if (pGpu != NULL &&
    ((IS_VIRTUAL(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST))
     || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
{
    // 行 289-290: 执行 RPC 调用
    NV_RM_RPC_CONTROL(pGpu, pParams->hClient, pParams->hObject, pParams->cmd,
                      pParams->pParams, pParams->paramsSize, status);
    
    // 行 297: 返回，跳过后续处理
    return (status == NV_OK) ? NV_WARN_NOTHING_TO_DO : status;
}

// 行 300: 如果不满足 RPC 条件，返回 NV_OK，继续本地执行
return NV_OK;
```

**重要说明**: 代码中直接使用 `IS_GSP_CLIENT(pGpu)` 而不是 `IS_FW_CLIENT(pGpu)`。虽然 `IS_FW_CLIENT` 可能定义为 `IS_GSP_CLIENT || IS_DCE_CLIENT`，但当前实现只检查 GSP 客户端的情况。

**关键逻辑说明**:
1. **满足 RPC 条件** (`IS_GSP_CLIENT` + `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL`):
   - 执行 RPC 调用
   - 返回 `NV_WARN_NOTHING_TO_DO`
   - **结果**: `resControl_IMPL` 检测到 `NV_WARN_NOTHING_TO_DO`，直接返回，**不会调用本地函数**（如 `subdeviceCtrlCmdGspGetFeatures_KERNEL`）

2. **不满足 RPC 条件**:
   - 返回 `NV_OK`
   - **结果**: `resControl_IMPL` 继续执行，**会调用本地处理函数**（如 `subdeviceCtrlCmdGspGetFeatures_KERNEL`）

**在 `resControl_IMPL` 中的处理逻辑** (`rs_resource.c:191-217`):
```c
status = resControl_Prologue(pResource, pCallContext, pRsParams);
if ((status != NV_OK) && (status != NV_WARN_NOTHING_TO_DO))
    goto done;

if (status == NV_WARN_NOTHING_TO_DO)
{
    // RPC 已在 Prologue 中处理完成，跳过本地函数调用
    status = NV_OK;
}
else
{
    // 不满足 RPC 条件，执行本地函数
    status = pFunc(pDynamicObj, pRsParams->pParams);  // 例如：subdeviceCtrlCmdGspGetFeatures
}
```

**注意**: 对于 `NV2080_CTRL_CMD_GSP_GET_FEATURES`，如果 GPU 是 GSP 客户端模式且命令带有 `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` 标志，命令会在这一步被路由到 GSP，`subdeviceCtrlCmdGspGetFeatures_KERNEL` **不会被调用**。否则，会在本地执行该函数。

### 3.4.1 标志获取机制

**文件**: `src/nvidia/src/kernel/rmapi/control.c`  
**函数**: `_rmapiRmControl` (行 450, 497)  
**作用**: 
- 在命令处理早期阶段获取命令标志
- 通过 `rmapiutilGetControlInfo` 从命令信息表中查找该命令的标志位
- 将标志存储在 `RS_CONTROL_COOKIE` 结构中，供后续阶段使用

**关键代码**:
```c
// 行 450: 获取命令信息（包括标志）
getCtrlInfoStatus = rmapiutilGetControlInfo(cmd, &ctrlFlags, &ctrlAccessRight, &ctrlParamsSize);

// 行 497: 存储标志到 Cookie 中
rmCtrlParams.pCookie->ctrlFlags = ctrlFlags;
```

**标志传递流程**:
1. `_rmapiRmControl` 调用 `rmapiutilGetControlInfo` 获取 `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` 等标志
2. 标志存储在 `rmCtrlExecuteCookie->ctrlFlags` 中
3. Cookie 通过 `rmCtrlParams.pCookie` 传递给 `serverControl`
4. 最终在 `rmresControl_Prologue_IMPL` 中通过 `pParams->pCookie->ctrlFlags` 访问

**标志定义**:
- **文件**: `src/nvidia/inc/kernel/rmapi/control.h` (行 233)
- **定义**: `#define RMCTRL_FLAGS_ROUTE_TO_PHYSICAL 0x000000040`
- **语义**: 该控制命令应直接转发到物理对象（GSP 固件），如果在 CPU-RM 内核中调用

---

### 3.5 本地执行路径 vs GSP 卸载路径（重要对比）

#### 条件判断逻辑

**RPC 路由条件**（在 `rmresControl_Prologue_IMPL` 中）:
```c
// 行 264-266: 两个条件必须同时满足
if (pGpu != NULL &&
    ((IS_VIRTUAL(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST))
     || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
```

**关键条件说明**:
1. **`IS_GSP_CLIENT(pGpu)`**: 
   - 表示当前 GPU 运行在 GSP 客户端模式（GSP 作为资源管理前端）
   - 在 GSP 客户端模式下，CPU-RM 是"瘦客户端"，大部分 GPU 操作由 GSP 固件执行
   - **注意**: 代码中直接使用 `IS_GSP_CLIENT` 而不是 `IS_FW_CLIENT`，只检查 GSP 客户端情况

2. **`RMCTRL_FLAGS_ROUTE_TO_PHYSICAL`**:
   - 这个标志在命令定义时设置（通常在 `.def` 文件中）
   - 表示该命令语义上针对"物理 GPU/物理资源"
   - 在 GSP 架构中，这类操作应该优先 offload 到 GSP 执行

#### 路径分支详解

**路径 A: 满足 RPC 条件 → GSP 卸载路径**

**执行流程**:
1. `rmresControl_Prologue_IMPL` 检测到条件满足
2. 调用 `NV_RM_RPC_CONTROL` → `rpcRmApiControl_GSP`
3. 准备 RPC 消息，发送到 GSP 固件
4. GSP 固件执行实际逻辑（例如：查询 GSP 功能特性）
5. GSP 将结果写回共享内存
6. CPU-RM 接收响应，复制回用户参数
7. 返回 `NV_WARN_NOTHING_TO_DO`
8. `resControl_IMPL` 检测到 `NV_WARN_NOTHING_TO_DO`，**跳过本地函数调用**

**特点**:
- ✅ 执行在 GSP 固件中（更接近硬件）
- ✅ 可以访问 GSP 内部状态和配置
- ✅ 返回真实的 GSP 功能信息（`bValid = NV_TRUE`）
- ⚠️ 需要 RPC 通信开销（共享内存 + 中断）

**路径 B: 不满足 RPC 条件 → 本地执行路径**

**执行流程**:
1. `rmresControl_Prologue_IMPL` 检测到条件不满足
2. 返回 `NV_OK`（不是 `NV_WARN_NOTHING_TO_DO`）
3. `resControl_IMPL` 继续执行
4. 调用本地处理函数：`subdeviceCtrlCmdGspGetFeatures_KERNEL`

**本地函数实现** (`subdevice_ctrl_gpu_kernel.c:3542-3551`):
```c
NV_STATUS
subdeviceCtrlCmdGspGetFeatures_KERNEL
(
    Subdevice *pSubdevice,
    NV2080_CTRL_GSP_GET_FEATURES_PARAMS *pGspFeaturesParams
)
{
    pGspFeaturesParams->bValid = NV_FALSE;  // ⚠️ 关键：设置为无效
    return NV_OK;
}
```

**特点**:
- ✅ 执行在 CPU-RM 中（无 RPC 开销）
- ✅ 响应速度快（无跨进程通信）
- ❌ **无法访问 GSP 内部状态**
- ❌ **返回无效结果**（`bValid = NV_FALSE`）
- ❌ 功能位掩码、固件版本等信息都是未初始化的

#### 两种路径的根本区别

| 特性 | GSP 卸载路径 | 本地执行路径 |
|------|-------------|-------------|
| **执行位置** | GSP 固件（GPU 上） | CPU-RM（主机 CPU） |
| **数据来源** | GSP 内部状态/配置 | CPU-RM 本地数据 |
| **结果有效性** | `bValid = NV_TRUE` | `bValid = NV_FALSE` |
| **功能信息** | 真实的 GSP 功能位掩码 | 未初始化/无效 |
| **固件版本** | 真实的固件版本字符串 | 未初始化 |
| **通信方式** | 共享内存 + 硬件中断 | 直接函数调用 |
| **性能开销** | RPC 通信延迟 | 几乎无开销 |
| **适用场景** | GSP 客户端模式 | 传统模式/非 GSP 模式 |

#### 为什么本地路径返回无效结果？

**设计原因**:
1. **架构分离**: 在 GSP 客户端模式下，CPU-RM 是"瘦客户端"，不维护完整的 GPU 状态
2. **数据所有权**: GSP 功能信息由 GSP 固件维护，CPU-RM 无法直接访问
3. **一致性保证**: 通过返回 `bValid = NV_FALSE`，明确告知调用者数据无效，必须通过 GSP 路径获取

**实际使用场景**:
- **GSP 客户端模式**: 必须通过 RPC 路径，否则无法获取有效信息
- **传统模式（非 GSP）**: 可能没有 GSP 固件，本地路径返回 `bValid = NV_FALSE` 是合理的
- **混合模式**: 某些命令可能在本地执行，某些必须 offload 到 GSP

#### 场景分析

**场景 1: GSP 客户端模式 + ROUTE_TO_PHYSICAL 标志**
- **条件**: `IS_GSP_CLIENT(pGpu) == TRUE` && `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL == TRUE`
- **路径**: GSP 卸载路径
- **结果**: `bValid = NV_TRUE`，数据有效
- **本地函数**: **不会调用**

**场景 2: 非 GSP 模式（传统模式）**
- **条件**: `IS_GSP_CLIENT(pGpu) == FALSE`
- **路径**: 本地执行路径
- **结果**: `bValid = NV_FALSE`，数据无效
- **本地函数**: **会调用**，但返回无效结果

**场景 3: GSP 客户端模式但无 ROUTE_TO_PHYSICAL 标志**
- **条件**: `IS_GSP_CLIENT(pGpu) == TRUE` && `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL == FALSE`
- **路径**: 本地执行路径
- **结果**: `bValid = NV_FALSE`，数据无效
- **本地函数**: **会调用**，但返回无效结果

**注意**: 对于 `NV2080_CTRL_CMD_GSP_GET_FEATURES`，这个命令**应该总是**带有 `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` 标志，因为它的语义就是查询 GSP 功能。

#### 代码验证

**验证点 1**: `resControl_IMPL` 如何处理返回值
```c
// rs_resource.c:191-217
status = resControl_Prologue(pResource, pCallContext, pRsParams);
if ((status != NV_OK) && (status != NV_WARN_NOTHING_TO_DO))
    goto done;

if (status == NV_WARN_NOTHING_TO_DO)
{
    // ✅ RPC 路径：跳过本地函数调用
    status = NV_OK;
}
else
{
    // ✅ 本地路径：执行本地函数
    status = pFunc(pDynamicObj, pRsParams->pParams);
}
```

**验证点 2**: 本地函数的实现
```c
// subdevice_ctrl_gpu_kernel.c:3549
pGspFeaturesParams->bValid = NV_FALSE;  // 明确标记为无效
return NV_OK;  // 返回成功，但数据无效
```

**验证点 3**: RPC 路径的完整处理
```c
// resource.c:289-297
NV_RM_RPC_CONTROL(pGpu, ...);  // 发送 RPC 并等待响应
// 响应数据已填充到 pParams->pParams
return (status == NV_OK) ? NV_WARN_NOTHING_TO_DO : status;
```

---

## 第四阶段：业务实现与 RPC 传输 (Business Logic & RPC)

### 3.6 RMAPI 函数指针替换机制（重要补充）

**关键发现**：在 GSP 客户端模式下，RMAPI 的函数指针会在初始化时被替换为 RPC 版本。

**文件**: `src/nvidia/src/kernel/rmapi/rpc_common.c`  
**函数**: `rpcRmApiSetup` (行 54)  
**作用**: 
- 在 RPC 对象初始化时，根据 GPU 模式替换 RMAPI 函数指针
- 对于 GSP 客户端模式：将 `pRmApi->Control` 替换为 `rpcRmApiControl_GSP`

**关键代码**:
```c
// 行 74-80: GSP 客户端模式下的函数指针替换
else if (IS_GSP_CLIENT(pGpu))
{
    pRmApi->Control         = rpcRmApiControl_GSP;
    pRmApi->AllocWithHandle = rpcRmApiAlloc_GSP;
    pRmApi->Free            = rpcRmApiFree_GSP;
    pRmApi->DupObject       = rpcRmApiDupObject_GSP;
}
```

**重要说明**：
1. **只替换 `Control`，不替换 `ControlWithSecInfo`**：
   - `pRmApi->Control` → `rpcRmApiControl_GSP`（被替换）
   - `pRmApi->ControlWithSecInfo` → `rmapiControlWithSecInfo`（**不被替换**）

2. **对文档流程的影响**：
   - 文档中描述的标准路径使用的是 `pRmApi->ControlWithSecInfo()`（行 106）
   - 因此，**从用户态到 `rmresControl_Prologue_IMPL` 的标准流程不受影响**
   - 函数指针替换主要影响的是：
     - **直接调用 `pRmApi->Control()` 的代码路径**
     - **`NV_RM_RPC_CONTROL` 宏内部的调用**（见下文）

3. **`NV_RM_RPC_CONTROL` 宏的影响**：
   - 当 `rmresControl_Prologue_IMPL` 检测到 RPC 条件满足时，会调用 `NV_RM_RPC_CONTROL` 宏
   - 该宏内部调用 `pRmApi->Control()`（行 229），此时使用的是**已被替换**的 `rpcRmApiControl_GSP`
   - `rpcRmApiControl_GSP` **直接**准备并发送 RPC 消息，**不经过** `_rmapiRmControl` → `serverControl` 等步骤
   - 这是**预期的行为**，因为此时已经确定了需要 RPC 路由，无需再经过完整的本地处理流程

4. **两种路径的对比**：

   **路径 A（文档标准路径，满足 RPC 条件）**：
   ```
   用户态 → nvidia_ioctl → Nv04ControlWithSecInfo → 
   pRmApi->ControlWithSecInfo (rmapiControlWithSecInfo) → 
   _rmapiRmControl → serverControl → resControl_IMPL → 
   rmresControl_Prologue_IMPL (检测到 RPC 条件) → 
   NV_RM_RPC_CONTROL → pRmApi->Control (rpcRmApiControl_GSP) → 
   直接发送 RPC（跳过后续本地处理）
   ```

   **路径 B（如果直接调用 `pRmApi->Control()`）**：
   ```
   直接调用 pRmApi->Control() → rpcRmApiControl_GSP → 
   直接发送 RPC（完全绕过标准 RMAPI 流程）
   ```

**结论**：文档中描述的流程在 GSP 客户端模式下**仍然有效**，因为标准路径使用的是 `ControlWithSecInfo`，而函数指针替换只影响 `Control`。但在 `NV_RM_RPC_CONTROL` 宏调用时，会使用被替换的 `rpcRmApiControl_GSP`，这是设计的预期行为，用于在确定需要 RPC 路由时直接发送 RPC，避免不必要的本地处理开销。

#### 内部转发路径（间接通过 ioctl）

**重要发现**：虽然用户态 ioctl 不直接调用 `pRmApi->Control()`，但存在一个**内部转发路径**，可能会间接调用被替换的函数指针：

**文件**: `src/nvidia/src/kernel/gpu/gpu_resource.c`  
**函数**: `gpuresInternalControlForward_IMPL` (行 364-380)  
**作用**: 
- 某些内部控制命令的处理函数可能会调用此函数来转发到物理 RM
- 此函数**直接调用** `pRmApi->Control()`，在 GSP 客户端模式下会使用被替换的 `rpcRmApiControl_GSP`

**关键代码**:
```c
// 行 373-379: 直接调用 pRmApi->Control()
RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(GPU_RES_GET_GPU(pGpuResource));
return pRmApi->Control(pRmApi,
                       RES_GET_CLIENT_HANDLE(pGpuResource),
                       gpuresGetInternalObjectHandle(pGpuResource),
                       command,
                       pParams,
                       size);
```

**调用场景**：
- 某些资源对象（如 `Subdevice`、`Device`、`KernelGraphicsContext` 等）的内部控制命令处理函数
- 这些处理函数可能通过 `gpuresInternalControlForward` 转发到物理 RM
- 在 GSP 客户端模式下，这会**直接调用** `rpcRmApiControl_GSP`，**绕过**标准流程中的 `rmresControl_Prologue_IMPL` 检查

**示例**：
```c
// src/nvidia/src/kernel/gpu/subdevice/subdevice.c:245-254
subdeviceInternalControlForward_IMPL(...)
{
    // 调用 gpuresInternalControlForward_IMPL
    // → 直接调用 pRmApi->Control() (在 GSP 模式下是 rpcRmApiControl_GSP)
}
```

**与标准路径的区别**：

| 特性 | 标准 ioctl 路径 | 内部转发路径 |
|------|---------------|-------------|
| **入口** | `Nv04ControlWithSecInfo` | `gpuresInternalControlForward_IMPL` |
| **RMAPI 调用** | `pRmApi->ControlWithSecInfo()` | `pRmApi->Control()` |
| **函数指针** | 不被替换（使用标准实现） | **被替换**（GSP 模式下使用 `rpcRmApiControl_GSP`） |
| **是否经过 `rmresControl_Prologue_IMPL`** | ✅ 是 | ❌ **否** |
| **RPC 路由检查** | 在 `rmresControl_Prologue_IMPL` 中检查 | **直接发送 RPC**（在 GSP 模式下） |

**重要说明**：
- 内部转发路径不是从用户态 ioctl **直接**调用的，而是从内核内部的资源对象方法中调用的
- 这意味着某些内部控制命令在 GSP 客户端模式下会**自动**通过 RPC 发送，无需经过标准的路由检查
- 这是**设计的预期行为**，因为这些是内部控制命令，通常都需要在物理 RM（GSP）端执行

---

### 4.1 RPC 控制宏
**文件**: `src/nvidia/inc/kernel/vgpu/rpc.h`  
**宏定义**: `NV_RM_RPC_CONTROL` (行 223)  
**作用**: 
- 根据 GPU 类型选择 RPC 实现
- 对于 `IS_FW_CLIENT(pGpu)` (GSP 客户端): 调用 `pRmApi->Control` (物理 RMAPI)
- 对于 vGPU: 调用 `rpcDmaControl_wrapper`

**关键代码**:
```c
// 行 226-230: GSP 客户端路径
if (IS_FW_CLIENT(pGpu)) {
    RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);
    // 注意：这里的 pRmApi->Control 在 GSP 客户端模式下已被替换为 rpcRmApiControl_GSP
    status = pRmApi->Control(pRmApi, hClient, hObject, cmd, pParams, paramSize);
}
```

**关键说明**：
- 在 GSP 客户端模式下，`pRmApi->Control` 已在初始化时被替换为 `rpcRmApiControl_GSP`
- 因此，`NV_RM_RPC_CONTROL` 宏实际上直接调用 `rpcRmApiControl_GSP`，**不会**再经过 `_rmapiRmControl` 等标准流程
- 这是设计的预期行为：在确定需要 RPC 路由后，直接发送 RPC，避免不必要的本地处理

### 4.2 GSP RPC 控制实现
**文件**: `src/nvidia/src/kernel/vgpu/rpc.c`  
**函数**: `rpcRmApiControl_GSP` (行 10361)  
**作用**: 
- 准备 RPC 消息头，确定 RPC 功能号为 `NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL`
- 填充 RPC 参数结构 `rpc_gsp_rm_control_v03_00`:
  - `hClient`, `hObject`, `cmd`, `paramsSize`, `rmapiRpcFlags`
  - `rmctrlFlags`, `rmctrlAccessRight` (额外字段)
  - 复制命令参数到 RPC 消息缓冲区
- 处理大消息（超过缓冲区大小的情况）
- 调用 `_issueRpcAndWait` 或 `_issueRpcAndWaitLarge` 发送并等待响应

**关键代码**:
```c
// 行 10491-10493: 写入 RPC 消息头
NV_ASSERT_OK_OR_GOTO(status,
    rpcWriteCommonHeader(pGpu, pRpc, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, rpc_params_size),
    done);

// 行 10495-10501: 填充 RPC 参数
rpc_params->hClient        = hClient;
rpc_params->hObject        = hObject;
rpc_params->cmd            = cmd;
rpc_params->paramsSize     = paramsSize;
rpc_params->rmapiRpcFlags  = RMAPI_RPC_FLAGS_NONE;
rpc_params->rmctrlFlags    = 0;
rpc_params->rmctrlAccessRight = 0;

// 行 10528: 复制参数数据
portMemCopy(rpc_params->params, message_buffer_remaining, pParamStructPtr, paramsSize);

// 行 10554-10562: 发送并等待响应（根据消息大小选择函数）
if (large_message_copy) {
    status = _issueRpcAndWaitLarge(pGpu, pRpc, total_size, large_message_copy, NV_TRUE);
} else {
    status = _issueRpcAndWait(pGpu, pRpc);
}
```

### 4.3 RPC 发送消息
**文件**: `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c`  
**函数**: `_kgspRpcSendMessage` (约行 386)  
**作用**: 
- 调用 `GspMsgQueueSendCommand` 将命令放入共享内存队列（序列号在 `GspMsgQueueSendCommand` 内部设置）
- 调用 `kgspSetCmdQueueHead_HAL` 触发硬件中断

**关键代码**:
```c
// 行 386: 发送命令到消息队列（序列号在内部设置）
nvStatus = GspMsgQueueSendCommand(pRpc->pMessageQueueInfo, pGpu);

// 行 400: 触发硬件中断
kgspSetCmdQueueHead_HAL(pGpu, pKernelGsp, pRpc->pMessageQueueInfo->queueIdx, 0);
```

**说明**: 序列号的设置实际在 `GspMsgQueueSendCommand` 函数内部完成（见 `message_queue_cpu.c:471`）。

### 4.4 消息队列发送命令
**文件**: `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c`  
**函数**: `GspMsgQueueSendCommand` (行 446)  
**作用**: 
- 等待共享内存队列有空闲空间
- 使用 `portMemCopy` 将命令参数复制到共享内存队列中
- 设置校验和、序列号、元素计数
- 使用 `portAtomicMemoryFenceStore` 内存屏障，确保数据可见性
- 调用 `msgqTxSubmitBuffers` 更新软件写指针 (Write Pointer)

**关键代码**:
```c
// 行 455-456: 计算消息长度
NvU32 msgLen = GSP_MSG_QUEUE_ELEMENT_HDR_SIZE + pMQI->pCmdQueueElement->rpc.length;

// 行 471-473: 设置队列元素元数据
pCQE->seqNum = pMQI->txSeqNum;
pCQE->elemCount = GSP_MSG_QUEUE_BYTES_TO_ELEMENTS(msgLen);
pCQE->checkSum = 0;

// 行 566: 内存屏障确保数据可见性
portAtomicMemoryFenceStore();

// 行 568: 提交缓冲区，更新写指针
nRet = msgqTxSubmitBuffers(pMQI->hQueue, pCQE->elemCount);
```

### 4.5 触发硬件中断
**文件**: `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c`  
**函数**: `kgspSetCmdQueueHead_TU102` (行 341)  
**作用**: 
- 写硬件寄存器 `NV_PGSP_QUEUE_HEAD(queueIdx)` 更新命令队列头指针
- 向 GSP 处理器发送物理中断信号（Doorbell/Kick）
- GSP 固件检测到中断后开始处理命令

**关键代码**:
```c
// 行 352: 写入硬件寄存器触发中断
GPU_REG_WR32(pGpu, NV_PGSP_QUEUE_HEAD(queueIdx), value);
```

---

## 第五阶段：固件执行与响应 (Firmware Execution & Return)

### 5.1 GSP 固件执行（固件端）
**位置**: GSP 固件（不在驱动代码中）  
**作用**: 
- GSP 收到中断后读取队列消息
- 解析 Control 命令 (`NV2080_CTRL_CMD_GSP_GET_FEATURES`)
- 执行 `GET_FEATURES` 逻辑，填充功能位掩码
- 将结果写回共享内存状态队列
- 触发 CPU 中断或更新完成标志

### 5.2 RPC 接收轮询
**文件**: `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c`  
**函数**: `_kgspRpcRecvPoll` (行 2176)  
**作用**: 
- 轮询模式等待 GSP 响应
- 调用 `GspMsgQueueReceiveStatus` 读取状态队列
- 处理超时和错误情况
- 处理异步事件（在等待目标响应时）

**关键代码**:
```c
// 行 2202-2207: 设置轮询上下文
KernelGspRpcEventHandlerContext rpcHandlerContext = KGSP_RPC_EVENT_HANDLER_CONTEXT_POLL;
if (expectedFunc == NV_VGPU_MSG_EVENT_GSP_INIT_DONE) {
    rpcHandlerContext = KGSP_RPC_EVENT_HANDLER_CONTEXT_POLL_BOOTUP;
}

// 轮询循环中调用 GspMsgQueueReceiveStatus
```

### 5.3 消息队列接收状态
**文件**: `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c`  
**函数**: `GspMsgQueueReceiveStatus` (行 598)  
**作用**: 
- 从共享内存状态队列读取响应消息
- 验证校验和、序列号
- 将响应数据复制到本地缓冲区
- 更新读指针

**关键代码**:
```c
// 行 612-616: 重试循环读取消息
for (nRetries = 0; nRetries < nMaxRetries; nRetries++) {
    // 从状态队列读取元素
    nRet = msgqRxReceiveBuffers(pMQI->hQueue, 1, &pNextElement);
    // 验证并复制数据
}
```

### 5.4 RPC 响应处理
**文件**: `src/nvidia/src/kernel/vgpu/rpc.c`  
**函数**: `rpcRmApiControl_GSP` (行 10569-10596)  
**作用**: 
- 检查 RPC 传输状态
- 从 RPC 响应中提取状态码 (`rpc_params->status`)
- 将响应参数复制回用户传入的内存区
- 处理序列化/反序列化（如果使用）

**关键代码**:
```c
// 行 10569-10576: 检查响应状态
if (status == NV_OK) {
    if (rpc_params->status != NV_OK && !(rpc_params->rmapiRpcFlags & RMAPI_RPC_FLAGS_COPYOUT_ON_ERROR)) {
        status = rpc_params->status;
        goto done;
    }
}

// 行 10590-10595: 复制响应参数（处理序列化和非序列化两种情况）
if (resCtrlFlags & NVOS54_FLAGS_FINN_SERIALIZED) {
    // 序列化路径
    portMemCopy(pCallContext->pSerializedParams, pCallContext->serializedSize, rpc_params->params, rpc_params->paramsSize);
    // ... 反序列化 ...
} else {
    // 非序列化路径
    if (paramsSize != 0) {
        portMemCopy(pParamStructPtr, paramsSize, rpc_params->params, paramsSize);
    }
}
```

### 5.5 层层返回
**调用栈返回路径**:
1. `rpcRmApiControl_GSP` → 返回状态和填充的参数
2. `rmresControl_Prologue_IMPL` → 返回 `NV_WARN_NOTHING_TO_DO` 或错误状态
3. `resControl_IMPL` → 返回状态
4. `gpuresControl_IMPL` → 返回状态
5. `serverControl` → 返回状态，释放锁
6. `_rmapiRmControl` → 返回状态
7. `rmapiControlWithSecInfo` → 返回状态
8. `Nv04ControlWithSecInfo` → 设置 `pArgs->status`
9. `escape.c` → 返回内核状态
10. `nvidia_ioctl` → 将结果复制回用户空间，返回系统调用结果

---

## 关键数据结构

### RPC 消息头
```c
typedef struct rpc_message_header_v {
    NvU32 function;      // NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL
    NvU32 sequence;      // 序列号
    NvU32 rpc_result;    // 状态码
    // ...
} rpc_message_header_v;
```

### GSP RPC 控制参数
```c
typedef struct rpc_gsp_rm_control_v03_00 {
    NvHandle hClient;
    NvHandle hObject;
    NvU32 cmd;
    NvU32 paramsSize;
    NvU8 params[];  // 实际参数数据
} rpc_gsp_rm_control_v03_00;
```

### 消息队列元素
```c
typedef struct GSP_MSG_QUEUE_ELEMENT {
    NvU8  authTagBuffer[16];
    NvU8  aadBuffer[16];
    NvU32 checkSum;
    NvU32 seqNum;
    NvU32 elemCount;
    rpc_message_header_v rpc;
} GSP_MSG_QUEUE_ELEMENT;
```

---

## 完整执行时序总结

### 满足 RPC 条件的完整执行流程

**完整执行时序**（满足 RPC 条件的情况）:

```
1. 用户态: ioctl(NV_ESC_RM_CONTROL, ...)
   └─ 准备参数结构体

2. 内核态: nvidia_ioctl (nv.c:2419)
   └─ 复制参数到内核空间

3. Escape 层: Nv04ControlWithSecInfo (escape.c:759)
   └─ 转换 file_private 为 hClient

4. RMAPI 入口: _nv04ControlWithSecInfo (entry_points.c:493)
   └─ 调用 rmapiControlWithSecInfo

5. RMAPI 控制: rmapiControlWithSecInfo → _rmapiRmControl (control.c:1034, 350)
   ├─ 获取命令标志: rmapiutilGetControlInfo → 获取 RMCTRL_FLAGS_ROUTE_TO_PHYSICAL
   └─ 初始化 RmCtrlParams 和 Cookie

6. ResServ: serverControl (rs_server.c:1453)
   ├─ 获取 Top Lock (API Lock)
   ├─ 获取 Client 锁并查找客户端
   ├─ 查找资源对象 (hSubDevice)
   └─ 设置 TLS 上下文

7. 多态分发: resControl → gpuresControl_IMPL → resControl_IMPL
   ├─ 查找命令处理函数: subdeviceCtrlCmdGspGetFeatures_IMPL
   └─ 执行 Prologue 钩子

8. RPC 路由拦截: rmresControl_Prologue_IMPL (resource.c:254)
   ├─ 检查: IS_GSP_CLIENT(pGpu) && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL
   ├─ ✅ 条件满足
   └─ 执行 RPC: NV_RM_RPC_CONTROL → pRmApi->Control()
       └─ 注意：pRmApi->Control 在 GSP 客户端模式下已被替换为 rpcRmApiControl_GSP

9. RPC 直接发送: rpcRmApiControl_GSP (rpc.c:10361)
   ├─ 准备 RPC 消息头（跳过标准 RMAPI 流程）
   ├─ 填充 RPC 参数结构
   ├─ 复制命令参数到 RPC 消息缓冲区
   └─ 调用 _issueRpcAndWait 发送并等待响应

10. RPC 传输: GspMsgQueueSendCommand → kgspSetCmdQueueHead_HAL
    ├─ 复制命令到共享内存队列
    ├─ 更新写指针
    └─ 触发硬件中断

11. GSP 执行: 固件处理命令 → 写回结果
    └─ 填充功能位掩码、固件版本等信息

12. RPC 接收: GspMsgQueueReceiveStatus → 复制结果
    └─ 从共享内存读取响应

13. RPC 响应处理: rpcRmApiControl_GSP 复制响应数据
    └─ 将响应参数复制回原始参数缓冲区

14. 返回处理: rmresControl_Prologue_IMPL 返回 NV_WARN_NOTHING_TO_DO
    └─ resControl_IMPL 检测到 NV_WARN_NOTHING_TO_DO，跳过本地函数调用

15. 层层返回: 结果复制回用户空间
    └─ 用户态获得有效数据 (bValid = NV_TRUE)
```

**关键时间点**: 
- **步骤 8** 是决定性的：如果条件满足，RPC 在这里完成
- **步骤 9** 是关键：`rpcRmApiControl_GSP` 直接发送 RPC，**不经过** `_rmapiRmControl` → `serverControl` 等步骤
- **步骤 14** 是关键分支：`NV_WARN_NOTHING_TO_DO` 导致本地函数不被调用

**重要说明**：
- 在步骤 9 中，由于 `pRmApi->Control` 已被替换为 `rpcRmApiControl_GSP`，所以 `NV_RM_RPC_CONTROL` 宏直接调用 `rpcRmApiControl_GSP`
- `rpcRmApiControl_GSP` 会执行完整的 RPC 发送和接收流程（步骤 9-13），但**跳过**了标准的本地 RMAPI 处理流程（`_rmapiRmControl`、`serverControl` 等）
- 这是设计的预期行为：在确定需要 RPC 路由后，直接发送 RPC，避免不必要的本地处理开销

### 不满足 RPC 条件的执行流程

**执行时序**（不满足 RPC 条件的情况）:

```
1-7. 与上述相同（用户态到多态分发）

8. RPC 路由检查: rmresControl_Prologue_IMPL
   ├─ 检查: IS_GSP_CLIENT(pGpu) && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL
   ├─ ❌ 条件不满足
   └─ 返回: NV_OK

9. 本地函数调用: resControl_IMPL
   └─ 调用: subdeviceCtrlCmdGspGetFeatures_KERNEL
       └─ 设置 bValid = NV_FALSE

10. 层层返回: 结果复制回用户空间
    └─ 用户态获得无效数据 (bValid = NV_FALSE)
```

---

## 关键要点总结

1. **RPC 路由时机**: 在 `rmresControl_Prologue_IMPL` 中检查并路由，而不是在具体的处理函数中
2. **多态分发**: 通过 NVOC 虚表机制实现对象的多态调用
3. **共享内存通信**: 使用共享内存队列进行 CPU 和 GSP 之间的通信
4. **硬件中断**: 通过写寄存器触发 GSP 中断
5. **同步等待**: 使用轮询或中断方式等待 GSP 响应
6. **参数序列化**: 支持大消息的分片传输和序列化/反序列化
7. **标志获取**: 命令标志在 `_rmapiRmControl` 阶段获取，存储在 Cookie 中传递
8. **路径选择**: 基于 `IS_GSP_CLIENT` 和 `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` 两个条件决定执行路径
9. **函数指针替换**: 在 GSP 客户端模式下，`pRmApi->Control` 在初始化时被替换为 `rpcRmApiControl_GSP`，但标准路径使用 `ControlWithSecInfo`，因此不受影响；只有在 `NV_RM_RPC_CONTROL` 宏中调用 `pRmApi->Control()` 时才会使用替换后的函数

### 设计说明

**为什么在 Prologue 阶段拦截？**
- Prologue 在所有资源特定的处理之前执行
- 可以在不查找具体处理函数的情况下就决定路由
- 减少了不必要的对象查找和函数指针解析开销

**`NV_WARN_NOTHING_TO_DO` 的设计意图**
- 这是一个"警告"级别的状态码，表示"没有需要本地处理的事情"
- 与错误状态码不同，它表示"处理已完成，但不在本地"
- 上层代码可以安全地忽略这个警告，继续执行

**本地函数的"占位符"作用**
- 在非 GSP 模式下，接口仍然存在，保持 API 兼容性
- 通过 `bValid = NV_FALSE` 明确告知调用者需要 GSP 支持
- 避免了在非 GSP 模式下返回错误，而是返回"无效但可接受"的结果

---

## 文件路径索引

| 阶段 | 文件路径 | 函数/宏 | 行号 |
|------|---------|---------|------|
| 1.2 | `kernel-open/nvidia/nv.c` | `nvidia_ioctl` | 2419 |
| 1.3 | `src/nvidia/arch/nvalloc/unix/src/escape.c` | `Nv04ControlWithSecInfo` | 759 |
| 1.4 | `src/nvidia/src/kernel/rmapi/entry_points.c` | `_nv04ControlWithSecInfo` | 493 |
| 2.1 | `src/nvidia/src/kernel/rmapi/control.c` | `rmapiControlWithSecInfo` | 1034 |
| 2.2 | `src/nvidia/src/kernel/rmapi/control.c` | `_rmapiRmControl` | 350 |
| 2.2.1 | `src/nvidia/src/kernel/rmapi/control.c` | `rmapiutilGetControlInfo` (标志获取) | 450, 497 |
| 2.3 | `src/nvidia/src/libraries/resserv/src/rs_server.c` | `serverControl` | 1453 |
| 3.1 | `src/nvidia/inc/libraries/resserv/rs_resource.h` | `resControl` (宏) | 292 |
| 3.2 | `src/nvidia/src/kernel/gpu/gpu_resource.c` | `gpuresControl_IMPL` | 393 |
| 3.3 | `src/nvidia/src/libraries/resserv/src/rs_resource.c` | `resControl_IMPL` | 152 |
| 3.4 | `src/nvidia/src/kernel/rmapi/resource.c` | `rmresControl_Prologue_IMPL` | 254 |
| 3.4.1 | `src/nvidia/inc/kernel/rmapi/control.h` | `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` (标志定义) | 233 |
| 4.1 | `src/nvidia/inc/kernel/vgpu/rpc.h` | `NV_RM_RPC_CONTROL` (宏) | 223 |
| 4.2 | `src/nvidia/src/kernel/vgpu/rpc.c` | `rpcRmApiControl_GSP` | 10361 |
| 4.3 | `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` | `_kgspRpcSendMessage` | ~386 |
| 4.4 | `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c` | `GspMsgQueueSendCommand` | 446 |
| 4.5 | `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c` | `kgspSetCmdQueueHead_TU102` | 341 |
| 5.2 | `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` | `_kgspRpcRecvPoll` | 2176 |
| 5.3 | `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c` | `GspMsgQueueReceiveStatus` | 598 |