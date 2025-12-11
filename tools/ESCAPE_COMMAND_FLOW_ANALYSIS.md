# ESCAPE 命令执行流程分析

## 目录
- [概述](#概述)
- [ESCAPE-RM 其他对象类的命令流程](#escape-rm-其他对象类的命令流程)
- [ESCAPE 非 RM 类型的 ioctl 执行流程](#escape-非-rm-类型的-ioctl-执行流程)
- [GSP 交互条件总结](#gsp-交互条件总结)

---

## 概述

根据代码分析，ESCAPE 命令可以分为两大类：

1. **ESCAPE-RM 类型**：所有 `NV_ESC_RM_*` 命令（23 个）
   - 统一走 RM 栈：`escape.c` → `rmapi` → `resserv` → `resControl` → 多态分发
   - **可能**与 GSP 交互（取决于对象类型和命令标志）

2. **ESCAPE 非 RM 类型**：其他 Escape 命令（约 15 个）
   - 在 `nvidia_ioctl` 或 `osapi.c` 中直接处理
   - **不会**与 GSP 交互

---

## ESCAPE-RM 其他对象类的命令流程

### 关键发现：不同 RM 命令有不同的执行路径

**重要结论**：`NV_ESC_RM_*` 类型的命令根据命令类型有不同的执行路径：

1. **NV_ESC_RM_CONTROL**：走 `resControl` 多态分发路径
2. **NV_ESC_RM_ALLOC**：走 `rmapiAlloc` 路径，通过资源描述符标志判断是否 RPC
3. **NV_ESC_RM_FREE**：走 `rmapiFree` 路径，通过资源的 `bRpcFree` 标志判断是否 RPC
4. **NV_ESC_RM_DUP_OBJECT**：走 `rmapiDupObject` 路径，在 GSP 客户端模式下直接 RPC
5. **其他 RM 命令**：各自特定的处理路径

### NV_ESC_RM_CONTROL 的完整执行路径

```
1. 用户态: ioctl(NV_ESC_RM_CONTROL, ...)
   └─ 准备参数结构体（包含 hObject）

2. 内核态: nvidia_ioctl (nv.c:2377)
   └─ 复制参数到内核空间

3. Escape 层: Nv04ControlWithSecInfo (escape.c:759)
   └─ 转换 file_private 为 hClient
   └─ 调用 Nv04ControlWithSecInfo

4. RMAPI 入口: _nv04ControlWithSecInfo (entry_points.c:493)
   └─ 调用 rmapiControlWithSecInfo

5. RMAPI 控制: rmapiControlWithSecInfo → _rmapiRmControl (control.c:1034, 350)
   ├─ 获取命令标志: rmapiutilGetControlInfo
   └─ 初始化 RmCtrlParams 和 Cookie

6. ResServ: serverControl (rs_server.c:1453)
   ├─ 获取 Top Lock (API Lock)
   ├─ 获取 Client 锁并查找客户端
   ├─ 查找资源对象 (hObject) ← 关键：根据 hObject 找到对应的资源对象
   └─ 设置 TLS 上下文

7. 多态分发: resControl (虚函数调用)
   └─ 根据对象类型跳转：
      ├─ Subdevice → subdeviceControl_IMPL → gpuresControl_IMPL → resControl_IMPL
      ├─ Device → deviceControl_IMPL → resControl_IMPL
      ├─ GpuResource → gpuresControl_IMPL → resControl_IMPL
      └─ 其他资源类 → 各自的 *Control_IMPL → resControl_IMPL

8. 命令查找: resControl_IMPL (rs_resource.c:152)
   └─ 在对应类的导出表中查找命令处理函数
      ├─ Subdevice: 在 g_subdevice_nvoc.c 的导出表中查找
      ├─ Device: 在 g_device_nvoc.c 的导出表中查找
      └─ 其他类: 在各自的 g_<ClassName>_nvoc.c 中查找

9. RPC 路由拦截: rmresControl_Prologue_IMPL (resource.c:254)
   ├─ 检查: IS_FW_CLIENT(pGpu) && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL
   ├─ ✅ 条件满足 → 执行 RPC (rpcRmApiControl_GSP)
   └─ ❌ 条件不满足 → 继续本地执行
```

### NV_ESC_RM_ALLOC 的执行路径

```
1. 用户态: ioctl(NV_ESC_RM_ALLOC, ...)
   └─ 准备 NVOS21_PARAMETERS 或 NVOS64_PARAMETERS 结构体

2. 内核态: nvidia_ioctl (nv.c:2377)
   └─ 复制参数到内核空间

3. Escape 层: Nv04AllocWithSecInfo (escape.c:376)
   └─ 验证参数大小
   └─ 调用 Nv04AllocWithSecInfo

4. RMAPI 入口: _nv04AllocWithSecInfo (entry_points.c:235)
   └─ 调用 rmapiAllocWithSecInfo

5. RMAPI Alloc: rmapiAllocWithSecInfo (alloc_free.c:1195)
   └─ 初始化锁信息
   └─ 调用 serverAllocResource

6. ResServ Alloc: serverAllocResource (alloc_free.c:750)
   ├─ 查找资源描述符
   ├─ 检查 RS_FLAGS_ALLOC_RPC_TO_PHYS_RM 标志
   └─ 如果标志存在且 IS_FW_CLIENT(pGpu) → 调用 NV_RM_RPC_ALLOC_OBJECT

7. RPC 调用: rpcRmApiAlloc_GSP (rpc.c:11262)
   ├─ 准备 RPC 消息
   ├─ 序列化分配参数
   └─ 发送到 GSP 并等待响应
```

**关键点**：
- 在 GSP 客户端模式下，`pRmApi->AllocWithHandle` 被替换为 `rpcRmApiAlloc_GSP`
- 但实际是否发送 RPC 取决于资源描述符的 `RS_FLAGS_ALLOC_RPC_TO_PHYS_RM` 标志
- 不是所有资源分配都会走 RPC，只有特定资源类（如某些 GPU 资源）才会

### NV_ESC_RM_FREE 的执行路径

```
1. 用户态: ioctl(NV_ESC_RM_FREE, ...)
   └─ 准备 NVOS00_PARAMETERS 结构体

2. 内核态: nvidia_ioctl (nv.c:2377)
   └─ 复制参数到内核空间

3. Escape 层: Nv01FreeWithSecInfo (escape.c:427)
   └─ 验证参数大小
   └─ 调用 Nv01FreeWithSecInfo

4. RMAPI 入口: _nv01FreeWithSecInfo (entry_points.c:320)
   └─ 调用 rmapiFreeWithSecInfo

5. RMAPI Free: rmapiFreeWithSecInfo (alloc_free.c:1487)
   └─ 初始化锁信息
   └─ 调用 serverFreeResourceTree

6. ResServ Free: serverFreeResourceTree → serverFreeResourceRpcUnderLock (alloc_free.c:957)
   ├─ 检查: IS_FW_CLIENT(pGpu) || IS_VIRTUAL(pGpu)
   ├─ 检查: 资源的 bRpcFree == NV_TRUE
   └─ 如果条件满足 → 调用 NV_RM_RPC_FREE

7. RPC 调用: rpcRmApiFree_GSP (rpc.c:11435)
   ├─ 准备 RPC 消息
   └─ 发送到 GSP 并等待响应
```

**关键点**：
- 在 GSP 客户端模式下，`pRmApi->Free` 被替换为 `rpcRmApiFree_GSP`
- 但实际是否发送 RPC 取决于资源的 `bRpcFree` 标志
- 只有特定资源（在分配时设置了 `bRpcFree`）的释放才会走 RPC

### NV_ESC_RM_DUP_OBJECT 的执行路径

```
1. 用户态: ioctl(NV_ESC_RM_DUP_OBJECT, ...)
   └─ 准备 NVOS55_PARAMETERS 结构体

2. Escape 层: Nv04DupObjectWithSecInfo (escape.c:651)
   └─ 调用 rmapiDupObjectWithSecInfo

3. RMAPI DupObject: rmapiDupObjectWithSecInfo
   └─ 在 GSP 客户端模式下，直接调用 rpcRmApiDupObject_GSP

4. RPC 调用: rpcRmApiDupObject_GSP (rpc.c:11383)
   ├─ 准备 RPC 消息
   └─ 发送到 GSP 并等待响应
```

**关键点**：
- 在 GSP 客户端模式下，`pRmApi->DupObject` 被替换为 `rpcRmApiDupObject_GSP`
- 没有额外的条件检查，直接发送 RPC

### 不同对象类的差异（仅适用于 CONTROL 命令）

#### 1. Subdevice 类（已分析）

**特点**：
- 继承自 `GpuResource`
- 有 606 个控制命令
- 大部分命令带有 `ROUTE_TO_PHYSICAL` 标志（约 69.5%）

**执行路径**：
```
resControl → subdeviceControl_IMPL → gpuresControl_IMPL → resControl_IMPL
```

#### 2. Device 类

**特点**：
- 直接继承自 `RsResource`（不经过 `GpuResource`）
- 命令数量较少（主要用于设备级操作）

**执行路径**：
```
resControl → deviceControl_IMPL → resControl_IMPL
```

**关键代码位置**：
- 导出表：`src/nvidia/generated/g_device_nvoc.c`
- 实现：`src/nvidia/src/kernel/gpu/device/`（如果存在）

#### 3. GpuResource 类（基类）

**特点**：
- `Subdevice` 的基类
- 提供 GPU 相关的通用处理

**执行路径**：
```
resControl → gpuresControl_IMPL → resControl_IMPL
```

**关键代码位置**：
- 实现：`src/nvidia/src/kernel/gpu/gpu_resource.c:393`

#### 4. 其他资源类

**示例**：
- `Memory`、`Channel`、`Context` 等

**执行路径**：
```
resControl → <ClassName>Control_IMPL → resControl_IMPL
```

### 关键点：RPC 路由对所有对象类都适用

**重要发现**：`rmresControl_Prologue_IMPL` 是**所有资源类共享的 Prologue 钩子**，无论对象类型是什么，只要满足以下条件，都会触发 GSP RPC：

```c
// resource.c:264-266
if (pGpu != NULL &&
    ((IS_VIRTUAL(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST))
     || (IS_FW_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
{
    // 执行 RPC 调用
    NV_RM_RPC_CONTROL(...);
    return NV_WARN_NOTHING_TO_DO;
}
```

**条件说明**：
1. **`pGpu != NULL`**：资源必须关联到一个 GPU 对象
   - `Subdevice`、`Device`、`GpuResource` 都有 `pGpu` 指针
   - 某些非 GPU 资源（如纯内存对象）可能 `pGpu == NULL`，不会触发 RPC

2. **`IS_FW_CLIENT(pGpu)`**：GPU 必须运行在固件客户端模式
   - 这是环境条件，与对象类型无关

3. **`RMCTRL_FLAGS_ROUTE_TO_PHYSICAL`**：命令必须带有该标志
   - 这是命令定义时的属性，与对象类型无关
   - 不同对象类的命令可能有不同的标志设置

### 示例：Device 类命令的 GSP 交互

假设有一个 Device 类的命令 `NVXXXX_CTRL_CMD_XXX`：

1. **如果命令带有 `ROUTE_TO_PHYSICAL` 标志**：
   - 执行路径：`deviceControl_IMPL` → `resControl_IMPL` → `rmresControl_Prologue_IMPL`
   - 条件检查：`IS_FW_CLIENT(pGpu) && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL`
   - **结果**：✅ 触发 GSP RPC

2. **如果命令没有 `ROUTE_TO_PHYSICAL` 标志**：
   - 执行路径：同上
   - 条件检查：不满足
   - **结果**：❌ 本地执行 `deviceCtrlCmdXXX_IMPL`

---

## ESCAPE 非 RM 类型的 ioctl 执行流程

### 分类

根据代码分析，非 RM 类型的 Escape 命令可以分为以下几类：

#### A. 系统级命令（在 `nvidia_ioctl` 中直接处理）

**位置**：`kernel-open/nvidia/nv.c:2502`

**命令列表**：
1. `NV_ESC_QUERY_DEVICE_INTR` (0xCD) - 查询设备中断
2. `NV_ESC_CARD_INFO` (0xC8) - 显卡信息
3. `NV_ESC_ATTACH_GPUS_TO_FD` (0xCC) - 附加 GPU 到文件描述符
4. `NV_ESC_CHECK_VERSION_STR` (0xCA) - 检查版本字符串
5. `NV_ESC_SYS_PARAMS` (0xCE) - 系统参数
6. `NV_ESC_EXPORT_TO_DMABUF_FD` (0xD1) - 导出到 DMA-BUF
7. `NV_ESC_WAIT_OPEN_COMPLETE` (0xD2) - 等待打开完成
8. `NV_ESC_NUMA_INFO` (0xD3) - NUMA 信息
9. `NV_ESC_SET_NUMA_STATUS` (0xD4) - 设置 NUMA 状态
10. `NV_ESC_REGISTER_FD` (0xC9) - 注册文件描述符
11. `NV_ESC_IOCTL_XFER_CMD` (0xCB) - 传输命令（用于大参数）

**执行流程**：
```
用户态: ioctl(fd, NV_ESC_XXX, &params)
   ↓
内核态: nvidia_ioctl (nv.c:2377)
   ↓
switch (arg_cmd) {
    case NV_ESC_XXX:
        // 直接处理，不进入 RM 栈
        // 不调用 escape.c 或 rmapi
        break;
}
   ↓
返回结果到用户态
```

**特点**：
- ✅ 直接在 `nvidia_ioctl` 中处理
- ❌ **不会**进入 RM 栈（`escape.c`、`rmapi`、`resserv`）
- ❌ **不会**与 GSP 交互
- ✅ 主要用于系统级操作（设备信息、NUMA、文件描述符管理等）

**示例代码**：
```c
// nv.c:2504-2520
case NV_ESC_QUERY_DEVICE_INTR:
{
    nv_ioctl_query_device_intr_t *query_intr = arg_copy;
    
    NV_ACTUAL_DEVICE_ONLY(nv);
    
    if ((arg_size < sizeof(*query_intr)) || (!nv->regs->map))
    {
        status = -EINVAL;
        goto done;
    }
    
    query_intr->intrStatus = *(nv->regs->map + (NV_RM_DEVICE_INTR_ADDRESS >> 2));
    query_intr->status = NV_OK;
    break;
}
```

#### B. OS 事件类（在 `osapi.c` 中处理）

**位置**：`src/nvidia/arch/nvalloc/unix/src/osapi.c:2693`

**命令列表**：
1. `NV_ESC_ALLOC_OS_EVENT` (0xC6) - 分配 OS 事件
2. `NV_ESC_FREE_OS_EVENT` (0xC7) - 释放 OS 事件
3. `NV_ESC_RM_GET_EVENT_DATA` (0x52) - 获取事件数据（虽然是 RM 前缀，但在这里处理）

**执行流程**：
```
用户态: ioctl(fd, NV_ESC_ALLOC_OS_EVENT, &params)
   ↓
内核态: nvidia_ioctl (nv.c:2377)
   ↓
调用: rm_ioctl (osapi.c:2672)
   ↓
switch (Command) {
    case NV_ESC_ALLOC_OS_EVENT:
        // 在 OS 层直接处理
        allocate_os_event(...);
        break;
}
   ↓
返回结果
```

**特点**：
- ✅ 在 `osapi.c` 的 `rm_ioctl` 函数中处理
- ❌ **不会**进入 RM 栈的核心部分
- ❌ **不会**与 GSP 交互
- ✅ 主要用于操作系统事件管理

**示例代码**：
```c
// osapi.c:2693-2704
case NV_ESC_ALLOC_OS_EVENT:
{
    nv_ioctl_alloc_os_event_t *pApi = pData;
    
    if (dataSize != sizeof(nv_ioctl_alloc_os_event_t))
    {
        rmStatus = NV_ERR_INVALID_ARGUMENT;
        break;
    }
    
    pApi->Status = allocate_os_event(pApi->hClient, nvfp, pApi->fd);
    break;
}
```

#### C. 其他 RM 前缀但不在 RM 栈处理的命令

**注意**：`NV_ESC_RM_GET_EVENT_DATA` 虽然以 `RM_` 开头，但实际在 `osapi.c` 中处理，不在标准的 RM 控制流程中。

---

## GSP 交互条件总结

### 会与 GSP 交互的 RM 命令类型

根据代码分析，**多个 `NV_ESC_RM_*` 命令都可能与 GSP 交互**，但触发机制不同：

#### 1. NV_ESC_RM_CONTROL（控制命令）

**执行路径**：
```
escape.c:711 → Nv04ControlWithSecInfo → rmapiControlWithSecInfo
  → _rmapiRmControl → serverControl → resControl
  → rmresControl_Prologue_IMPL (检查 ROUTE_TO_PHYSICAL)
  → rpcRmApiControl_GSP (如果条件满足)
```

**触发条件**：
1. `IS_FW_CLIENT(pGpu) == TRUE`
2. 命令带有 `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` 标志
3. 资源对象关联到 GPU (`pGpu != NULL`)

**关键代码**：
- 路由判断：`src/nvidia/src/kernel/rmapi/resource.c:254` (`rmresControl_Prologue_IMPL`)
- RPC 实现：`src/nvidia/src/kernel/vgpu/rpc.c:10977` (`rpcRmApiControl_GSP`)

#### 2. NV_ESC_RM_ALLOC（资源分配）

**执行路径**：
```
escape.c:376 → Nv04AllocWithSecInfo → rmapiAllocWithSecInfo
  → serverAllocResource
  → 检查资源描述符的 RS_FLAGS_ALLOC_RPC_TO_PHYS_RM 标志
  → rpcRmApiAlloc_GSP (如果条件满足)
```

**触发条件**：
1. `IS_FW_CLIENT(pGpu) == TRUE`
2. 资源描述符带有 `RS_FLAGS_ALLOC_RPC_TO_PHYS_RM` 标志
3. 资源对象关联到 GPU

**关键代码**：
- 路由判断：`src/nvidia/src/kernel/rmapi/alloc_free.c:868-896`
- RPC 实现：`src/nvidia/src/kernel/vgpu/rpc.c:11262` (`rpcRmApiAlloc_GSP`)

**注意**：在 GSP 客户端模式下，`pRmApi->AllocWithHandle` 函数指针被替换为 `rpcRmApiAlloc_GSP`（`rpc_common.c:77`），但实际是否调用 RPC 还取决于资源描述符的标志。

#### 3. NV_ESC_RM_FREE（资源释放）

**执行路径**：
```
escape.c:427 → Nv01FreeWithSecInfo → rmapiFreeWithSecInfo
  → serverFreeResourceTree → serverFreeResourceRpcUnderLock
  → 检查资源的 bRpcFree 标志
  → rpcRmApiFree_GSP (如果条件满足)
```

**触发条件**：
1. `IS_FW_CLIENT(pGpu) == TRUE` 或 `IS_VIRTUAL(pGpu) == TRUE`
2. 资源的 `bRpcFree == NV_TRUE`
3. 资源对象关联到 GPU

**关键代码**：
- 路由判断：`src/nvidia/src/kernel/rmapi/alloc_free.c:957-986` (`serverFreeResourceRpcUnderLock`)
- RPC 实现：`src/nvidia/src/kernel/vgpu/rpc.c:11435` (`rpcRmApiFree_GSP`)

**注意**：在 GSP 客户端模式下，`pRmApi->Free` 函数指针被替换为 `rpcRmApiFree_GSP`（`rpc_common.c:78`），但实际是否调用 RPC 还取决于资源的 `bRpcFree` 标志。

#### 4. NV_ESC_RM_DUP_OBJECT（对象复制）

**执行路径**：
```
escape.c → Nv04DupObjectWithSecInfo → rmapiDupObject
  → rpcRmApiDupObject_GSP (在 GSP 客户端模式下)
```

**触发条件**：
1. `IS_FW_CLIENT(pGpu) == TRUE`

**关键代码**：
- RPC 实现：`src/nvidia/src/kernel/vgpu/rpc.c:11383` (`rpcRmApiDupObject_GSP`)

**注意**：在 GSP 客户端模式下，`pRmApi->DupObject` 函数指针被替换为 `rpcRmApiDupObject_GSP`（`rpc_common.c:79`）。

### RMAPI 函数指针替换机制

**关键发现**：在 GSP 客户端模式下，RMAPI 的函数指针会在初始化时被替换为 RPC 版本：

```c
// rpc_common.c:74-80
if (IS_GSP_CLIENT(pGpu)) {
    pRmApi->Control         = rpcRmApiControl_GSP;
    pRmApi->AllocWithHandle = rpcRmApiAlloc_GSP;
    pRmApi->Free            = rpcRmApiFree_GSP;
    pRmApi->DupObject       = rpcRmApiDupObject_GSP;
}
```

这意味着：
- 所有通过 RMAPI 接口调用的操作（Control、Alloc、Free、DupObject）都会经过这些 RPC 函数
- 但 RPC 函数内部会检查具体条件，决定是否真正发送 RPC 到 GSP

### 不会与 GSP 交互的情况

#### 情况 1：非 GSP 客户端模式

**示例**：
- 传统模式（非 GSP 客户端）
- 非固件客户端模式

**原因**：
- `IS_FW_CLIENT(pGpu) == FALSE`
- RMAPI 函数指针不会被替换为 RPC 版本

#### 情况 2：命令不满足特定条件

**示例**：
- `NV_ESC_RM_CONTROL` 但命令没有 `ROUTE_TO_PHYSICAL` 标志
- `NV_ESC_RM_ALLOC` 但资源描述符没有 `RS_FLAGS_ALLOC_RPC_TO_PHYS_RM` 标志
- `NV_ESC_RM_FREE` 但资源的 `bRpcFree == NV_FALSE`

**原因**：
- 虽然进入了 RPC 函数，但内部条件检查不通过，会回退到本地处理或直接返回

#### 情况 3：其他 RM 命令

**示例**：
- `NV_ESC_RM_CONFIG_GET/SET` - 配置管理（未找到对应的 RPC 函数）
- `NV_ESC_RM_MAP_MEMORY` - 内存映射（本地处理，不经过 RPC）
- `NV_ESC_RM_I2C_ACCESS` - I2C 访问（本地处理）

**原因**：
- 这些命令在 `escape.c` 中直接处理，调用特定的处理函数
- 没有对应的 RPC 函数实现

#### 情况 2：非 RM 类型的 Escape 命令

**示例**：
- `NV_ESC_CARD_INFO` - 显卡信息
- `NV_ESC_QUERY_DEVICE_INTR` - 查询中断
- `NV_ESC_ALLOC_OS_EVENT` - OS 事件

**原因**：
- 在 `nvidia_ioctl` 或 `osapi.c` 中直接处理
- **不会**进入 RM 栈
- **不会**调用 `escape.c` 的 `Nv04ControlWithSecInfo`

#### 情况 3：RM_CONTROL 但不满足 RPC 条件

**示例**：
- `NV_ESC_RM_CONTROL` 但 `IS_FW_CLIENT(pGpu) == FALSE`
- `NV_ESC_RM_CONTROL` 但命令没有 `ROUTE_TO_PHYSICAL` 标志

**原因**：
- 虽然进入了 `rmresControl_Prologue_IMPL`，但条件不满足
- 返回 `NV_OK`，继续本地执行

---

## 总结表

| 命令类型 | 执行路径 | 是否进入 RM 栈 | 是否可能 GSP 交互 | 触发条件 | 说明 |
|---------|---------|--------------|-----------------|---------|------|
| **NV_ESC_RM_CONTROL** | escape.c → rmapi → resserv → resControl | ✅ | ⚠️ **条件满足时** | `IS_FW_CLIENT` + `ROUTE_TO_PHYSICAL` | 通过 `rmresControl_Prologue_IMPL` 检查 |
| **NV_ESC_RM_ALLOC** | escape.c → rmapiAlloc → serverAllocResource | ✅ | ⚠️ **条件满足时** | `IS_FW_CLIENT` + `RS_FLAGS_ALLOC_RPC_TO_PHYS_RM` | 通过资源描述符标志检查 |
| **NV_ESC_RM_FREE** | escape.c → rmapiFree → serverFreeResourceTree | ✅ | ⚠️ **条件满足时** | `IS_FW_CLIENT` + `bRpcFree == NV_TRUE` | 通过资源的 `bRpcFree` 标志检查 |
| **NV_ESC_RM_DUP_OBJECT** | escape.c → rmapiDupObject | ✅ | ⚠️ **条件满足时** | `IS_FW_CLIENT` | 在 GSP 客户端模式下直接调用 RPC |
| **NV_ESC_RM_CONFIG_GET/SET** | escape.c → 各自处理函数 | ✅ | ❌ | - | 未找到对应的 RPC 函数 |
| **NV_ESC_RM_MAP_MEMORY** | escape.c → rmapiMapMemory | ✅ | ❌ | - | 本地处理，不经过 RPC |
| **其他 NV_ESC_RM_*** | escape.c → 各自处理函数 | ✅ | ❌ | - | 大多数不走 RPC 路径 |
| **NV_ESC_CARD_INFO** | nvidia_ioctl 直接处理 | ❌ | ❌ | - | 系统级命令 |
| **NV_ESC_QUERY_DEVICE_INTR** | nvidia_ioctl 直接处理 | ❌ | ❌ | - | 系统级命令 |
| **NV_ESC_ALLOC_OS_EVENT** | osapi.c 直接处理 | ❌ | ❌ | - | OS 事件管理 |

### GSP 交互的完整条件总结

**会与 GSP 交互的命令及其条件**：

1. **NV_ESC_RM_CONTROL**：
   ```
   IS_FW_CLIENT(pGpu) == TRUE
   && RMCTRL_FLAGS_ROUTE_TO_PHYSICAL == TRUE
   && pGpu != NULL
   ```

2. **NV_ESC_RM_ALLOC**：
   ```
   IS_FW_CLIENT(pGpu) == TRUE
   && 资源描述符带有 RS_FLAGS_ALLOC_RPC_TO_PHYS_RM 标志
   && pGpu != NULL
   ```

3. **NV_ESC_RM_FREE**：
   ```
   (IS_FW_CLIENT(pGpu) == TRUE || IS_VIRTUAL(pGpu) == TRUE)
   && 资源的 bRpcFree == NV_TRUE
   && pGpu != NULL
   ```

4. **NV_ESC_RM_DUP_OBJECT**：
   ```
   IS_FW_CLIENT(pGpu) == TRUE
   ```

**关键代码位置**：
- RMAPI 函数指针替换：`src/nvidia/src/kernel/rmapi/rpc_common.c:74-80` (`rpcRmApiSetup`)
- CONTROL RPC：`src/nvidia/src/kernel/vgpu/rpc.c:10977` (`rpcRmApiControl_GSP`)
- ALLOC RPC：`src/nvidia/src/kernel/vgpu/rpc.c:11262` (`rpcRmApiAlloc_GSP`)
- FREE RPC：`src/nvidia/src/kernel/vgpu/rpc.c:11435` (`rpcRmApiFree_GSP`)
- DUP_OBJECT RPC：`src/nvidia/src/kernel/vgpu/rpc.c:11383` (`rpcRmApiDupObject_GSP`)

---

*文档生成时间: 2025-01-XX*  
*基于代码库: open-gpu-kernel-modules*  
*分析范围: 所有 Escape 命令类型的执行路径和 GSP 交互条件*

