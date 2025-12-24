# Flat API vs FINN 序列化 API 及绕过 Prologue 的 RPC 场景分析

## 目录
- [一、Flat API 与 FINN 序列化 API](#一flat-api-与-finn-序列化-api)
  - [1.1 基本概念](#11-基本概念)
  - [1.2 区别对比](#12-区别对比)
  - [1.3 序列化流程](#13-序列化流程)
  - [1.4 判断走哪条路径](#14-判断走哪条路径)
- [二、绕过 Prologue 直接 RPC 的情况](#二绕过-prologue-直接-rpc-的情况)
  - [2.1 正常 RPC 路径（通过 Prologue）](#21-正常-rpc-路径通过-prologue)
  - [2.2 绕过 Prologue 的五种情况](#22-绕过-prologue-的五种情况)
  - [2.3 总结表格](#23-总结表格)
  - [2.4 对 Fuzz 种子收集的影响](#24-对-fuzz-种子收集的影响)

---

## 一、Flat API 与 FINN 序列化 API

### 1.1 基本概念

#### Flat API（扁平 API）

**定义**：参数结构体是"扁平"的，即结构体内部**不包含嵌入式指针**，所有数据都在结构体本身内部。

**特点**：
- 参数可以直接通过 `memcpy` 复制到 RPC 消息缓冲区
- 不需要特殊的序列化/反序列化处理
- GSP 端收到的就是原始的 C 结构体
- **大多数 RM Control 命令都是 Flat API**

**示例**：
```c
// Flat API 示例：NV2080_CTRL_GPU_GET_INFO_V2_PARAMS
typedef struct NV2080_CTRL_GPU_GET_INFO_V2_PARAMS {
    NvU32  gpuInfoListSize;           // 简单类型
    struct {
        NvU32 index;
        NvU32 data;
    } gpuInfoList[32];                // 内联数组，非指针
} NV2080_CTRL_GPU_GET_INFO_V2_PARAMS;
```

#### FINN 序列化 API（FINN Serialized API）

**定义**：**FINN (Full Interface NN)** 是 NVIDIA 的一种序列化框架，用于处理包含**嵌入式指针**的复杂参数结构。

**特点**：
- 参数结构体包含**指向其他数据的指针**
- 需要将指针指向的数据"展开"并序列化为字节流
- 序列化后的数据才能通过 RPC 传输
- **少数命令需要 FINN 序列化**

**示例**：
```c
// FINN 序列化 API 示例：包含嵌入指针的结构
typedef struct NV0080_CTRL_FIFO_GET_CHANNELLIST_PARAMS {
    NvU32   numChannels;
    NvU32  *pChannelHandleList;   // ⭐ 嵌入指针！
    NvU32  *pChannelClassList;    // ⭐ 嵌入指针！
} NV0080_CTRL_FIFO_GET_CHANNELLIST_PARAMS;
```

### 1.2 区别对比

| 特性 | Flat API | FINN 序列化 API |
|-----|----------|-----------------|
| **参数结构** | 扁平结构，无嵌入指针 | 包含嵌入指针 |
| **数据传输方式** | 直接 `memcpy` | 需要序列化/反序列化 |
| **序列化标志** | `NVOS54_FLAGS_FINN_SERIALIZED` 未设置 | `NVOS54_FLAGS_FINN_SERIALIZED` 已设置 |
| **处理函数** | 直接复制到 RPC buffer | 调用 `FinnRmApiSerializeDown/Up` |
| **命令占比** | **大多数**（约 90%+） | **少数** |
| **复杂度** | 简单 | 复杂（需要 FINN 库支持） |
| **用于 Fuzz** | ✅ 容易，结构与头文件定义一致 | ⚠️ 需要理解序列化格式 |

### 1.3 序列化流程

#### 序列化发生位置

**文件**：`src/nvidia/src/kernel/vgpu/rpc.c` 第 10437-10454 行

```c
// rpcRmApiControl_GSP 函数中的序列化逻辑

// 检查是否已经预序列化
if (resCtrlFlags & NVOS54_FLAGS_FINN_SERIALIZED)
{
    bPreSerialized = NV_TRUE;  // 调用者已经序列化
}
else
{
    // 尝试序列化（如果支持）
    status = serverSerializeCtrlDown(pCallContext, cmd, 
                                     &pParamStructPtr, &paramsSize, 
                                     &resCtrlFlags);
    if (status != NV_OK)
        goto done;
}

// 检查序列化结果
if (!(resCtrlFlags & NVOS54_FLAGS_FINN_SERIALIZED))
{
    // ⭐ 这是 Flat API - 直接使用原始结构体
    // 可以尝试使用缓存
    ...
}
else
{
    // ⭐ 这是 FINN 序列化 API - 使用序列化后的字节流
    ...
}
```

#### serverSerializeCtrlDown 函数逻辑

**文件**：`src/nvidia/src/kernel/rmapi/rmapi_finn.c` 第 40-108 行

```c
NV_STATUS serverSerializeCtrlDown(
    CALL_CONTEXT *pCallContext,
    NvU32 cmd,
    void **ppParams,
    NvU32 *pParamsSize,
    NvU32 *flags
)
{
    if (!(*flags & NVOS54_FLAGS_FINN_SERIALIZED))
    {
        // 从命令 ID 提取接口 ID 和消息 ID
        const NvU32 interface_id = (DRF_VAL(XXXX, _CTRL_CMD, _CLASS, cmd) << 8) |
                                    DRF_VAL(XXXX, _CTRL_CMD, _CATEGORY, cmd);
        const NvU32 message_id = DRF_VAL(XXXX, _CTRL_CMD, _INDEX, cmd);
        
        // 获取序列化后的大小
        NvU32 serializedSize = FinnRmApiGetSerializedSize(interface_id, message_id, *ppParams);

        // ⭐ 关键判断：如果 FINN 不支持序列化此命令，直接返回
        if (serializedSize == 0)
            return NV_OK;  // Flat API - 无需序列化

        // 分配序列化缓冲区
        pSerBuffer = portMemAllocNonPaged(serializedSize);
        
        // 执行序列化
        status = FinnRmApiSerializeDown(interface_id, message_id, 
                                        *ppParams, pSerBuffer, serializedSize);
        
        // 设置序列化标志
        *flags |= NVOS54_FLAGS_FINN_SERIALIZED;
        
        // 用序列化后的数据替换原始参数
        *ppParams = pCallContext->pSerializedParams;
        *pParamsSize = pCallContext->serializedSize;
    }
    
    return NV_OK;
}
```

### 1.4 判断走哪条路径

**判断流程**：

```
命令进入 rpcRmApiControl_GSP
    ↓
调用 serverSerializeCtrlDown()
    ↓
FinnRmApiGetSerializedSize() 返回值是否 > 0？
    │
    ├─ 返回 0 → ⭐ Flat API
    │   │   - 不设置 NVOS54_FLAGS_FINN_SERIALIZED
    │   │   - 直接 memcpy 原始结构体到 RPC buffer
    │   └─→ rpc_params->params = memcpy(pParams, paramsSize)
    │
    └─ 返回 > 0 → ⭐ FINN 序列化 API
        │   - 调用 FinnRmApiSerializeDown() 进行序列化
        │   - 设置 NVOS54_FLAGS_FINN_SERIALIZED 标志
        └─→ rpc_params->params = 序列化后的字节流
```

**RPC 传输时的区别**：

```c
// 在 rpcRmApiControl_GSP 中复制参数到 RPC 消息

if (paramsSize != 0)
{
    if (portMemCopy(rpc_params->params, message_buffer_remaining, 
                    pParamStructPtr, paramsSize) == NULL)
    {
        status = NV_ERR_BUFFER_TOO_SMALL;
        goto done;
    }
}

// ⭐ 无论是 Flat API 还是 FINN API，最终都是 memcpy
// 区别在于 pParamStructPtr 指向什么：
//   - Flat API: 指向原始 C 结构体
//   - FINN API: 指向序列化后的字节流
```

---

## 二、绕过 Prologue 直接 RPC 的情况

### 2.1 正常 RPC 路径（通过 Prologue）

**正常路径**：用户态 IOCTL 调用会经过完整的 RM 栈：

```
用户态: ioctl(NV_ESC_RM_CONTROL)
    ↓
P1: nvidia_ioctl → Nv04ControlWithSecInfo
    ↓
P2: rmapiControlWithSecInfo → _rmapiRmControl → serverControl
    ↓
P3: resControl → gpuresControl_IMPL → resControl_IMPL
    ↓
P3: rmresControl_Prologue_IMPL  ← ⭐ Hook 点 1（Prologue）
    ├─ 条件检查: IS_GSP_CLIENT(pGpu) && ROUTE_TO_PHYSICAL
    ├─ ✅ 条件满足 → NV_RM_RPC_CONTROL()
    └─ 返回 NV_WARN_NOTHING_TO_DO（跳过本地函数）
    ↓
P4: rpcRmApiControl_GSP → GspMsgQueueSendCommand
    ↓
P5: GSP 固件执行 → 返回响应
```

**Prologue 的路由条件**：

```c
// resource.c:221-247
if (pGpu != NULL &&
    ((IS_VIRTUAL(pGpu)    && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST)
    ) || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
{
    // 执行 RPC
    NV_RM_RPC_CONTROL(pGpu, pParams->hClient, pParams->hObject, pParams->cmd,
                      pParams->pParams, pParams->paramsSize, status);
    
    return (status == NV_OK) ? NV_WARN_NOTHING_TO_DO : status;
}
```

### 2.2 绕过 Prologue 的五种情况

#### 情况 1：_VF 后缀函数（vGPU 专用路径）

**描述**：带有 `_VF` 后缀的函数是 **vGPU (Virtual GPU)** 专用的实现，在 `IS_VIRTUAL(pGpu)` 环境下直接调用 RPC，不经过标准 Prologue。

**触发条件**：`IS_VIRTUAL(pGpu) == TRUE`（在 vGPU Guest 环境中）

**代码示例**：
```c
// g_device_nvoc.h 中的 _VF 函数声明
NV_STATUS deviceCtrlCmdGpuGetBrandCaps_VF(struct Device *pDevice, 
                                          NV0080_CTRL_GPU_GET_BRAND_CAPS_PARAMS *pParams);
NV_STATUS deviceCtrlCmdDmaFlush_VF(struct Device *pDevice, 
                                   NV0080_CTRL_DMA_FLUSH_PARAMS *flushParams);
NV_STATUS deviceCtrlCmdFifoGetEngineContextProperties_VF(struct Device *pDevice, 
                                                          NV0080_CTRL_FIFO_GET_ENGINE_CONTEXT_PROPERTIES_PARAMS *pParams);
```

**执行流程**：
```
用户态 IOCTL
    ↓
RM 栈 → resControl_IMPL → 命令查找
    ↓
⭐ 直接调用 xxxCtrlCmdYyy_VF() 函数
    ↓
函数内部: NV_RM_RPC_CONTROL() → vGPU Host
    ↓
⭐ 完全绕过 rmresControl_Prologue_IMPL
```

**典型 _VF 函数列表**：
- `deviceCtrlCmdGpuGetBrandCaps_VF`
- `deviceCtrlCmdDmaFlush_VF`
- `deviceCtrlCmdFifoGetEngineContextProperties_VF`
- `deviceCtrlCmdFifoGetLatencyBufferSize_VF`
- `deviceCtrlCmdMsencGetCapsV2_VF`
- `deviceCtrlCmdBspGetCapsV2_VF`
- `deviceCtrlCmdNvjpgGetCapsV2_VF`

---

#### 情况 2：条件性 RPC（函数内部判断后调用）

**描述**：某些命令**没有设置 `ROUTE_TO_PHYSICAL` 标志**，因此不会在 Prologue 阶段自动路由到 GSP。但函数内部会**根据具体条件**判断是否需要调用 RPC。

**典型示例**：`NV2080_CTRL_CMD_GPU_GET_INFO_V2` (0x20800102)

**代码分析**：
```c
// subdevice_ctrl_gpu_kernel.c → getGpuInfos()
static NV_STATUS getGpuInfos(Subdevice *pSubdevice, 
                             NV2080_CTRL_GPU_GET_INFO_V2_PARAMS *pParams)
{
    NvBool bPhysicalForward = NV_FALSE;
    
    for (i = 0; i < pParams->gpuInfoListSize; i++)
    {
        switch (pParams->gpuInfoList[i].index)
        {
            case NV2080_CTRL_GPU_INFO_INDEX_GPU_ID:
                // ⭐ 本地处理：GPU ID 可以在 CPU-RM 获取
                data = gpuGetId(pGpu);
                break;
                
            case NV2080_CTRL_GPU_INFO_INDEX_SOME_HW_INFO:
                // ⭐ 需要 RPC：某些硬件信息需要从 GSP 获取
                if (IS_GSP_CLIENT(pGpu))
                {
                    pParams->gpuInfoList[i].index |= INDEX_FORWARD_TO_PHYSICAL;
                    bPhysicalForward = NV_TRUE;
                }
                break;
        }
    }
    
    // ⭐ 条件性 RPC：只有当有需要转发的信息时才调用 RPC
    if ((IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu)) && bPhysicalForward && (status == NV_OK))
    {
        NV_RM_RPC_CONTROL(pGpu,
                          RES_GET_CLIENT_HANDLE(pSubdevice),
                          RES_GET_HANDLE(pSubdevice),
                          NV2080_CTRL_CMD_GPU_GET_INFO_V2,
                          pParams, sizeof(*pParams), status);
    }
    
    return status;
}
```

**执行流程**：
```
用户态 IOCTL
    ↓
RM 栈 → rmresControl_Prologue_IMPL
    ↓
⭐ ROUTE_TO_PHYSICAL 未设置 → 返回 NV_OK，继续执行本地函数
    ↓
subdeviceCtrlCmdGpuGetInfoV2_IMPL → getGpuInfos()
    ↓
⭐ 函数内部判断：部分信息需要 RPC
    ↓
函数内部: NV_RM_RPC_CONTROL() → GSP
```

---

#### 情况 3：内核内部逻辑触发的 RPC

**描述**：驱动内核的其他子系统（如 Channel、Memory 管理）可能直接调用 RPC，**不是由用户态 IOCTL 触发**。

**触发场景**：
- 内核事件处理
- 资源清理
- 内部状态同步
- 硬件中断响应

**代码示例**：
```c
// kernel_channel.c 中的内部 RPC 调用
static NV_STATUS kchannelSomeFunctionInternal(KernelChannel *pKernelChannel)
{
    OBJGPU *pGpu = GPU_RES_GET_GPU(pKernelChannel);
    NV_STATUS status = NV_OK;
    
    // ⭐ 直接调用 RPC，不经过 IOCTL 路径
    NV_RM_RPC_CONTROL(pGpu,
                      RES_GET_CLIENT_HANDLE(pKernelChannel),
                      hSubDevice,  // ⚠️ 可能是内部构造的句柄
                      NV2080_CTRL_CMD_FIFO_GET_CHANNEL_MEM_INFO,
                      &memInfoParams,
                      sizeof(memInfoParams),
                      status);
    
    return status;
}
```

**关键特点**：
- **句柄可能不是用户传入的**：`RES_GET_CLIENT_HANDLE()` 获取的是驱动内部资源的句柄
- **无法追溯到用户 IOCTL**：这些 RPC 不对应任何用户态请求
- **对 Fuzz 意义有限**：因为这些不是用户态攻击面

---

#### 情况 4：函数内部判断（智能路由）

**描述**：某些命令虽然**没有设置 `ROUTE_TO_PHYSICAL`**，但函数内部会检查 `IS_GSP_CLIENT(pGpu)`，根据运行环境决定是否调用 RPC。

**典型示例**：`NV2080_CTRL_CMD_GPU_EXEC_REG_OPS` (0x20800122)

**代码分析**：
```c
// subdeviceCtrlCmdGpuExecRegOps_cmn()
static NV_STATUS subdeviceCtrlCmdGpuExecRegOps_cmn(
    Subdevice *pSubdevice,
    NV2080_CTRL_GPU_EXEC_REG_OPS_PARAMS *pParams,
    NvBool bUseMigratableOps
)
{
    OBJGPU *pGpu = GPU_RES_GET_GPU(pSubdevice);
    NV_STATUS status = NV_OK;
    
    // 参数验证
    if (pParams->regOpCount == 0)
        return NV_ERR_INVALID_PARAM_STRUCT;
    
    // ⭐ 情况 A：vGPU 环境，直接 RPC
    if (IS_VIRTUAL(pGpu))
    {
        NV_RM_RPC_GPU_EXEC_REG_OPS(pGpu, ...);
        return status;
    }
    
    // ⭐ 情况 B：GSP 客户端模式
    if (IS_GSP_CLIENT(pGpu))
    {
        // 根据参数类型选择不同的 RPC
        if (bUseMigratableOps)
        {
            // 使用可迁移的寄存器操作 RPC
            NV_RM_RPC_REG_MIGRATABLE_OPS(pGpu, ...);
        }
        else
        {
            // 使用普通寄存器操作 RPC
            NV_RM_RPC_GPU_EXEC_REG_OPS(pGpu, ...);
        }
        return status;
    }
    
    // ⭐ 情况 C：传统模式，本地执行
    // 直接访问 GPU 寄存器
    for (i = 0; i < pParams->regOpCount; i++)
    {
        // 执行寄存器读写...
    }
    
    return status;
}
```

**为什么不设置 `ROUTE_TO_PHYSICAL`**：
- 需要根据**参数类型**（如 `bUseMigratableOps`）选择不同的 RPC
- 需要在函数内部进行**参数验证**后再决定路径
- 提供更**细粒度的控制**

---



### 2.3 总结表格

| 情况 | 触发条件 | 入口路径 | 是否经过 Prologue | 使用的 RPC 函数 | 示例 |
|-----|---------|---------|-----------------|----------------|------|
| **正常路径** | `ROUTE_TO_PHYSICAL` 已设置 | 用户 IOCTL | ✅ 经过 | `NV_RM_RPC_CONTROL` | 大多数 Subdevice 命令 |
| **情况 1：_VF 函数** | `IS_VIRTUAL(pGpu)` | 用户 IOCTL | ❌ 绕过 | 函数内部直接调用 | `deviceCtrlCmdGpuGetBrandCaps_VF` |
| **情况 2：条件性 RPC** | 函数内部逻辑判断 | 用户 IOCTL | ❌ 绕过 | `NV_RM_RPC_CONTROL` | `subdeviceCtrlCmdGpuGetInfoV2` |
| **情况 3：内部逻辑 RPC** | 内核子系统触发 | 内核内部 | ❌ 无关 | `NV_RM_RPC_CONTROL` | Channel 管理等 |
| **情况 4：智能路由** | `IS_GSP_CLIENT()` 检查 | 用户 IOCTL | ❌ 绕过 | 多种 RPC 宏 | `subdeviceCtrlCmdGpuExecRegOps` |

### 2.4 对 Fuzz 种子收集的影响

#### Hook 点 1（rmresControl_Prologue_IMPL）能捕获的

✅ **可捕获**：
- 设置了 `ROUTE_TO_PHYSICAL` 的所有命令（约 69.5% 的 Subdevice 命令）
- 正常用户态 IOCTL 触发的 RPC 调用

❌ **无法捕获**：
- 情况 1：_VF 函数调用
- 情况 2：条件性 RPC（函数内部调用）
- 情况 3：内核内部逻辑触发的 RPC
- 情况 4：智能路由（函数内部判断后调用）
- 情况 5：通过函数指针替换的直接调用

#### Hook 点 2（rpcRmApiControl_GSP）能捕获的

✅ **可捕获**：
- **所有**经过 `NV_RM_RPC_CONTROL` 宏的 RPC 调用
- 包括上述所有情况

❌ **缺失信息**：
- 缺少 `ctrlFlags`、`ctrlAccessRight` 等上下文信息
- 句柄可能不是用户原始传入的

#### 建议

**对于 Fuzz 种子收集**：

1. **优先使用 Hook 点 1**：
   - 信息完整（包含所有上下文）
   - 句柄是用户原始值
   - 便于分析和重放

2. **Hook 点 2 作为补充**：
   - 用于监控是否有遗漏的 RPC
   - 用于统计所有 RPC 流量

3. **关于绕过场景**：
   - **情况 1（_VF 函数）**：主要用于 vGPU，非主要攻击面
   - **情况 2、4（条件性/智能路由）**：可能需要特定输入才能触发 RPC 路径
   - **情况 3、5（内部逻辑）**：不是用户态攻击面，可忽略

---

*文档生成时间: 2025-12-22*  
*基于代码库: open-gpu-kernel-modules*
