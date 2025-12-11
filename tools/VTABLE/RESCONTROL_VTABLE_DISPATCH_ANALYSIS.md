# resControl 虚表跳转机制详解

## 目录
- [跳转机制概述](#跳转机制概述)
- [跳转依据：命令 ID (methodId)](#跳转依据命令-id-methodid)
- [虚表跳转流程](#虚表跳转流程)
- [如何获取所有可用的功能](#如何获取所有可用的功能)
- [导出表结构详解](#导出表结构详解)
- [查找机制实现](#查找机制实现)
- [实际示例](#实际示例)

---

## 跳转机制概述

`resControl` 的虚表跳转基于 **NVOC (NVIDIA Object Model)** 框架，通过以下机制实现：

1. **虚函数调用**：通过对象的虚表 (vtable) 调用 `resControl` 虚函数
2. **命令查找**：使用命令 ID (`methodId`) 在导出方法表中进行二分查找
3. **多态分发**：根据对象类型（如 `Subdevice`、`Device` 等）查找对应的处理函数

---

## 跳转依据：命令 ID (methodId)

### 关键数据结构

**命令 ID 就是控制命令的标识符**，例如：
- `NV2080_CTRL_CMD_GSP_GET_FEATURES` = `0x20803601`
- `NV2080_CTRL_CMD_GPU_GET_INFO_V2` = `0x20800102`

### 跳转流程

```
用户调用 ioctl(cmd=0x20803601)
    ↓
resControl (虚函数调用)
    ↓
resControl_IMPL
    ↓
resControlLookup (使用 cmd 作为 methodId 查找)
    ↓
objGetExportedMethodDef (在导出表中二分查找)
    ↓
找到 NVOC_EXPORTED_METHOD_DEF 结构
    ↓
返回函数指针 pFunc
    ↓
调用具体处理函数 (如 subdeviceCtrlCmdGspGetFeatures_IMPL)
```

---

## 虚表跳转流程

### 1. 虚函数入口

**文件**: `src/nvidia/inc/libraries/resserv/rs_resource.h` (行 292)

```c
virtual NV_STATUS resControl(RsResource *pResource, CALL_CONTEXT *pCallContext,
                             RS_RES_CONTROL_PARAMS_INTERNAL *pParams);
```

**实现**: NVOC 生成的宏展开为虚表调用

```c
// 生成的代码 (g_rs_resource_nvoc.h:397)
#define resControl(pResource, pCallContext, pParams) \
    resControl_DISPATCH(pResource, pCallContext, pParams)

// 虚表调用
static inline NV_STATUS resControl_DISPATCH(...) {
    return pResource->__nvoc_metadata_ptr->vtable.__resControl__(...);
}
```

### 2. 对象类型分发

根据对象类型，虚表指向不同的实现：

- **Subdevice** → `subdeviceControl_IMPL` → `gpuresControl_IMPL` → `resControl_IMPL`
- **Device** → `deviceControl_IMPL` → `resControl_IMPL`
- **其他资源类型** → 各自的 `*Control_IMPL` → `resControl_IMPL`

### 3. 命令查找

**文件**: `src/nvidia/src/libraries/resserv/src/rs_resource.c` (行 117-146)

```c
NV_STATUS resControlLookup_IMPL(
    RsResource *pResource,
    RS_RES_CONTROL_PARAMS_INTERNAL *pRsParams,
    const struct NVOC_EXPORTED_METHOD_DEF **ppEntry
)
{
    const struct NVOC_EXPORTED_METHOD_DEF *pEntry;
    NvU32 cmd = pRsParams->cmd;  // 命令 ID，例如 0x20803601

    *ppEntry = NULL;
    // 关键：使用 cmd 作为 methodId 查找
    pEntry = objGetExportedMethodDef(staticCast(objFullyDerive(pResource), Dynamic), cmd);

    if (pEntry == NULL)
        return NV_ERR_NOT_SUPPORTED;

    // 验证参数大小
    if ((pEntry->paramSize != 0) && (pRsParams->paramsSize != pEntry->paramSize))
        return NV_ERR_INVALID_PARAM_STRUCT;

    *ppEntry = pEntry;
    return NV_OK;
}
```

---

## 如何获取所有可用的功能

### 方法 1: 查看生成的导出表文件

**位置**: `src/nvidia/generated/g_<ClassName>_nvoc.c`

**示例**: Subdevice 类的导出表

**文件**: `src/nvidia/generated/g_subdevice_nvoc.c`

```c
// 导出表定义 (行 143)
static const struct NVOC_EXPORTED_METHOD_DEF __nvoc_exported_method_def_Subdevice[] = 
{
    {               /*  [0] */
        /*pFunc=*/      (void (*)(void)) &subdeviceCtrlCmdGpuGetInfoV2_IMPL,
        /*flags=*/      0x30118u,
        /*accessRight=*/0x0u,
        /*methodId=*/   0x20800102u,  // ← 命令 ID
        /*paramSize=*/  sizeof(NV2080_CTRL_GPU_GET_INFO_V2_PARAMS),
        /*pClassInfo=*/ &(__nvoc_class_def_Subdevice.classInfo),
#if NV_PRINTF_STRINGS_ALLOWED
        /*func=*/       "subdeviceCtrlCmdGpuGetInfoV2"  // ← 函数名
#endif
    },
    // ... 共 606 个条目
};

// 导出信息结构 (行 9543)
const struct NVOC_EXPORT_INFO __nvoc_export_info__Subdevice = 
{
    /*numEntries=*/     606,  // ← Subdevice 类支持 606 个控制命令
    /*pExportEntries=*/ __nvoc_exported_method_def_Subdevice
};
```

### 方法 2: 使用 grep 搜索命令 ID

```bash
# 查找所有 Subdevice 的命令 ID
grep -E "methodId.*0x2080" src/nvidia/generated/g_subdevice_nvoc.c

# 查找特定命令
grep "0x20803601" src/nvidia/generated/g_subdevice_nvoc.c
```

### 方法 3: 查看头文件中的命令定义

**位置**: `src/common/sdk/nvidia/inc/ctrl/ctrl2080/`

**示例**: `ctrl2080gsp.h` 定义了 GSP 相关的命令

```c
#define NV2080_CTRL_CMD_GSP_GET_FEATURES    (0x20803601)
```

### 方法 4: 运行时查询（需要调试支持）

如果启用了 `NV_PRINTF_STRINGS_ALLOWED`，导出表中的 `func` 字段包含函数名，可以通过调试工具查看。

---

## 导出表结构详解

### NVOC_EXPORTED_METHOD_DEF 结构

**文件**: `src/nvidia/inc/libraries/nvoc/runtime.h` (行 71-83)

```c
struct NVOC_EXPORTED_METHOD_DEF
{
    void (*pFunc) (void);              // 函数指针：指向实际处理函数
    NvU32 flags;                       // 标志位：权限、属性等（如 NO_LOCK, PRIVILEGED）
    NvU32 accessRight;                 // 访问权限：调用此方法所需的权限
    NvU32 methodId;                    // 方法 ID：命令 ID，用于查找匹配
    NvU32 paramSize;                   // 参数大小：参数结构体的字节数（0 表示无参数）
    const NVOC_CLASS_INFO* pClassInfo; // 类信息：定义此方法的类

#if NV_PRINTF_STRINGS_ALLOWED
    const char  *func;                 // 调试信息：函数名（字符串）
#endif
};
```

### 关键字段说明

1. **methodId**: 
   - 这就是**命令 ID**，例如 `0x20803601`
   - 用于在导出表中进行二分查找
   - 对应 `NV2080_CTRL_CMD_*` 宏定义的值

2. **pFunc**: 
   - 指向实际的处理函数
   - 例如：`&subdeviceCtrlCmdGspGetFeatures_IMPL`
   - 通过类型转换后调用：`((CONTROL_EXPORT_FNPTR) pEntry->pFunc)(pDynamicObj, pParams)`

3. **flags**: 
   - 包含命令的标志位
   - 例如：`RMCTRL_FLAGS_ROUTE_TO_PHYSICAL` (0x40)
   - 用于 RPC 路由判断

4. **paramSize**: 
   - 参数结构体的大小
   - 用于验证用户传入的参数大小是否正确

---

## 查找机制实现

### 二分查找算法

**文件**: `src/nvidia/src/libraries/nvoc/src/runtime.c` (行 299-333)

```c
const struct NVOC_EXPORTED_METHOD_DEF* 
nvocGetExportedMethodDefFromMethodInfo_IMPL(
    const struct NVOC_EXPORT_INFO *pExportInfo, 
    NvU32 methodId
)
{
    NvU32 exportLength;
    const struct NVOC_EXPORTED_METHOD_DEF *exportArray;

    if (pExportInfo == NULL)
        return NULL;

    exportLength = pExportInfo->numEntries;
    exportArray = pExportInfo->pExportEntries;

    if (exportArray != NULL && exportLength > 0)
    {
        // 关键：导出表按 methodId 排序，使用二分查找
        NvU32 low = 0;
        NvU32 high = exportLength;
        while (1)
        {
            NvU32 mid = (low + high) / 2;

            if (exportArray[mid].methodId == methodId)
                return &exportArray[mid];  // 找到匹配的命令

            if (high == mid || low == mid)
                break;  // 未找到

            if (exportArray[mid].methodId > methodId)
                high = mid;
            else
                low = mid;
        }
    }

    return NULL;  // 未找到
}
```

### 继承链查找

**文件**: `src/nvidia/src/libraries/nvoc/src/runtime.c` (行 335-350)

```c
const struct NVOC_EXPORTED_METHOD_DEF *objGetExportedMethodDef_IMPL(
    Dynamic *pObj, 
    NvU32 methodId
)
{
    const struct NVOC_CASTINFO *const pCastInfo = pObj->__nvoc_rtti->pClassDef->pCastInfo;
    const NvU32 numRelatives = pCastInfo->numRelatives;
    const struct NVOC_RTTI *const *relatives = pCastInfo->relatives;
    NvU32 i;

    // 遍历继承链：从派生类到基类
    for (i = 0; i < numRelatives; i++)
    {
        const void *pDef = nvocGetExportedMethodDefFromMethodInfo_IMPL(
            relatives[i]->pClassDef->pExportInfo, 
            methodId
        );
        if (pDef != NULL)
            return pDef;  // 在某个祖先类中找到
    }

    return NULL;  // 整个继承链都未找到
}
```

**说明**: 
- 如果当前类没有该命令，会向上查找基类（如 `GpuResource`、`RsResource`）
- 这支持命令的继承和覆盖

---

## 实际示例

### 示例 1: GSP_GET_FEATURES 命令的查找过程

**命令 ID**: `0x20803601` (`NV2080_CTRL_CMD_GSP_GET_FEATURES`)

**查找流程**:

1. **用户调用**: `ioctl(fd, NV_ESC_RM_CONTROL, &params)`，其中 `cmd = 0x20803601`

2. **到达 resControlLookup**:
   ```c
   pEntry = objGetExportedMethodDef(pResource, 0x20803601);
   ```

3. **在 Subdevice 导出表中查找**:
   ```c
   // g_subdevice_nvoc.c:8819-8829
   {
       /*methodId=*/   0x20803601u,  // ← 匹配！
       /*pFunc=*/      &subdeviceCtrlCmdGspGetFeatures_DISPATCH,
       /*func=*/       "subdeviceCtrlCmdGspGetFeatures"
   }
   ```

4. **返回函数指针**: `pEntry->pFunc = &subdeviceCtrlCmdGspGetFeatures_DISPATCH`

5. **调用处理函数**:
   ```c
   // resControl_IMPL 中
   CONTROL_EXPORT_FNPTR pFunc = ((CONTROL_EXPORT_FNPTR) pEntry->pFunc);
   status = pFunc(pDynamicObj, pRsParams->pParams);
   ```

### 示例 2: 查看 Subdevice 的所有命令

**方法**: 查看导出表文件

```bash
# 统计 Subdevice 的命令数量
grep -c "methodId" src/nvidia/generated/g_subdevice_nvoc.c
# 结果: 606 个命令

# 列出所有命令 ID 和函数名
grep -E "(methodId|func=)" src/nvidia/generated/g_subdevice_nvoc.c | \
    grep -A1 "methodId" | head -40
```

**输出示例**:
```
methodId=*/   0x20800102u,  // GPU_GET_INFO_V2
func=*/       "subdeviceCtrlCmdGpuGetInfoV2"

methodId=*/   0x20800110u,  // GPU_GET_NAME_STRING
func=*/       "subdeviceCtrlCmdGpuGetNameString"

methodId=*/   0x20803601u,  // GSP_GET_FEATURES
func=*/       "subdeviceCtrlCmdGspGetFeatures"
```

### 示例 3: 查找特定功能的命令

**场景**: 查找所有与 GSP 相关的命令

```bash
grep -i "gsp" src/nvidia/generated/g_subdevice_nvoc.c | grep "methodId"
```

**结果**:
- `0x20803601` - `subdeviceCtrlCmdGspGetFeatures`
- `0x20803602` - `subdeviceCtrlCmdGspGetRmHeapStats`
- 等等...

---

## 总结

### 跳转依据

**`resControl` 的虚表跳转根据以下内容进行**:

1. **对象类型** (通过虚表): 决定调用哪个类的 `*Control_IMPL`
2. **命令 ID** (`methodId`): 在导出表中查找对应的处理函数
3. **继承链**: 如果当前类没有，向上查找基类

### 获取功能列表的方法

1. **查看生成的导出表文件**: `src/nvidia/generated/g_<ClassName>_nvoc.c`
2. **使用 grep 搜索**: 查找 `methodId` 字段
3. **查看头文件**: `src/common/sdk/nvidia/inc/ctrl/ctrl2080/`
4. **运行时调试**: 如果启用了调试字符串，可以查看 `func` 字段

### 关键文件位置

| 组件 | 文件路径 |
|------|---------|
| 导出表结构定义 | `src/nvidia/inc/libraries/nvoc/runtime.h` |
| 查找实现 | `src/nvidia/src/libraries/nvoc/src/runtime.c` |
| 命令查找入口 | `src/nvidia/src/libraries/resserv/src/rs_resource.c` |
| Subdevice 导出表 | `src/nvidia/generated/g_subdevice_nvoc.c` |
| 命令定义头文件 | `src/common/sdk/nvidia/inc/ctrl/ctrl2080/` |

### 数据流

```
命令 ID (0x20803601)
    ↓
objGetExportedMethodDef (继承链查找)
    ↓
nvocGetExportedMethodDefFromMethodInfo (二分查找)
    ↓
NVOC_EXPORTED_METHOD_DEF (找到匹配项)
    ↓
pFunc (函数指针)
    ↓
调用处理函数
```

---

*文档生成时间: 2025-01-XX*  
*基于代码库: open-gpu-kernel-modules*


1. 第一级：对象类型决定进哪一个 resControl_IMPL

你有这几步（P2 / P3 那块）：

通过句柄 hObject 找到 RsResource *pResource（这个对象可能实际是 Subdevice/Device/Channel 等）

调用宏：resControl(pResource, pCallContext, pParams)

宏展开成：

return pResource->__nvoc_metadata_ptr->vtable.__resControl__(pResource, ...);


这就是标准 C++ 风格的虚函数调用：

同一个接口 resControl(...)，实际调到哪个实现，由“对象真实类型”决定：

如果是 Subdevice 对象 → 调 Subdevice::resControl_IMPL（或者 subdeviceControl_IMPL）；

如果是 Device 对象 → 调 Device::resControl_IMPL；

如果是别的类 → 调它自己类里的 *_Control_IMPL。

因此，第一跳只跟“对象的类别”有关，不看 cmd。
这一步通常会做一些“跟类有关的通用工作”，比如：

Subdevice 那边：找对应的 GPU、做电源/时钟检查，设置 pGpu 等（你文档里的“GPU 状态门卫” A7）

GpuResource 那边：绑定到具体 GPU instance

最后，再把事交给通用的 resControl_IMPL 去做“根据 cmd 找 handler”。

现实里的 call stack 大概是：

subdeviceControl_IMPL         (Subdevice 自己的控制入口)
  └─ gpuresControl_IMPL       (继承层里的中间类)
      └─ resControl_IMPL      (RsResource 通用实现)
          └─ resControlLookup_IMPL
              └─ objGetExportedMethodDef(...)
                  └─ nvocGetExportedMethodDefFromMethodInfo
                      └─ 找到 pFunc = subdeviceCtrlCmdGspGetFeatures_IMPL

2. 第二级：在 resControl_IMPL 里用 “cmd” 找具体函数

到了 resControl_IMPL 这一层，才开始用你传进来的 cmd（= methodId）去查表：

NV_STATUS resControl_IMPL(RsResource *pResource, CALL_CONTEXT *ctx,
                          RS_RES_CONTROL_PARAMS_INTERNAL *pRsParams)
{
    const NVOC_EXPORTED_METHOD_DEF *pEntry;

    status = resControlLookup_IMPL(pResource, pRsParams, &pEntry);
    if (status != NV_OK)
        return status;

    // 拿到函数指针
    CONTROL_EXPORT_FNPTR pFunc =
        (CONTROL_EXPORT_FNPTR)pEntry->pFunc;

    // 调用具体处理函数，比如 subdeviceCtrlCmdGspGetFeatures_IMPL
    return pFunc((Dynamic *)objFullyDerive(pResource),
                 pRsParams->pParams);
}


而 resControlLookup_IMPL 做的就是：

取出 cmd = pRsParams->cmd；

调 objGetExportedMethodDef((Dynamic*)pResource, cmd) 去当前对象对应的导出表找条目；

验证参数大小是否匹配 pEntry->paramSize；

找到后把 pEntry 传回。

这个查表跟“对象类别 + cmd”的关系

objGetExportedMethodDef(pObj, cmd) 的核心逻辑是：

先看对象的动态类型，比如 Subdevice；

取这个类的导出表 __nvoc_export_info__Subdevice（里面有 606 条 NVOC_EXPORTED_METHOD_DEF）；

在这张表里用 cmd（methodId）做二分查找；

如果没找到，再沿继承链往上（GpuResource, RsResource 等）找上一层类的导出表。

所以：

第二级跳转真正依赖的是“对象的动态类 + cmd”这一对。

对于 Subdevice + 0x20803601（GSP_GET_FEATURES），就能在 g_subdevice_nvoc.c 里找到那条：

{
    .pFunc      = &subdeviceCtrlCmdGspGetFeatures_IMPL,
    .methodId   = 0x20803601u,
    .paramSize  = sizeof(NV2080_CTRL_GSP_GET_FEATURES_PARAMS),
    .func       = "subdeviceCtrlCmdGspGetFeatures"
}


然后调用 subdeviceCtrlCmdGspGetFeatures_IMPL 这一个真正干活的函数。