#include "core/core.h"  // 已经包含了core/prelude.h，而prelude.h包含了nvport/nvport.h
#include "gpu/gpu.h"
#include "rmapi/resource.h"
#include "gpu/gsp/gsp_fuzz_hook.h"
#include "os/os.h"
#include "nv.h"
#include "nvmisc.h"  // For NV_MIN macro
// ⭐ 修复：显式包含nvport sync头文件以使用portSyncSpinlock函数
// 注意：虽然core/prelude.h包含了nvport/nvport.h，但显式包含更清晰
#include "nvport/sync.h"
// ⭐ 修复：使用osGetCurrentTime()替代osGetCurrentTimeNs()
// osGetCurrentTime()已确认存在且可见，返回秒和微秒，可以转换为纳秒

// 全局Hook状态
static NvBool g_bHookEnabled = NV_FALSE;
static NvBool g_bHookInitialized = NV_FALSE;  // ⭐ 新增：全局初始化标志
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

// 导出访问函数（供IOCTL处理使用）
NvU32 gspFuzzHookGetSeedRecordIndex(void)
{
    return g_seedRecordIndex;
}

GSP_FUZZ_SEED_RECORD *gspFuzzHookGetSeedRecordBuffer(void)
{
    return g_pSeedRecordBuffer;
}

PORT_SPINLOCK *gspFuzzHookGetSeedRecordLock(void)
{
    return g_pSeedRecordLock;
}

GSP_FUZZ_HOOK_CONFIG *gspFuzzHookGetConfigPtr(void)
{
    return &g_hookConfig;
}

GSP_FUZZ_HOOK_STATS *gspFuzzHookGetStatsPtr(void)
{
    return &g_hookStats;
}

// 初始化Hook模块
NV_STATUS gspFuzzHookInit(void)
{
    // ⭐ 新增：如果已经初始化，直接返回
    if (g_bHookInitialized)
    {
        return NV_OK;
    }
    
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
            portSyncSpinlockDestroy(g_pSeedRecordLock);
            portMemFree(g_pSeedRecordLock);
            g_pSeedRecordLock = NULL;
            return NV_ERR_INSUFFICIENT_RESOURCES;
        }
        portMemSet(g_pSeedRecordBuffer, 0, bufferSize);
    }
    
    // ⭐ 新增：标记初始化完成
    g_bHookInitialized = NV_TRUE;
    
    return NV_OK;
}

// 清理Hook模块
void gspFuzzHookCleanup(void)
{
    // ⭐ 关键修复：如果未初始化，直接返回（幂等/安全）
    if (!g_bHookInitialized)
    {
        return;
    }
    
    // ⭐ 先标记为未初始化，防止重复调用
    g_bHookInitialized = NV_FALSE;
    g_bHookEnabled = NV_FALSE;
    
    // ⭐ 先释放 buffer，再释放 lock
    if (g_pSeedRecordBuffer != NULL)
    {
        portMemFree(g_pSeedRecordBuffer);
        g_pSeedRecordBuffer = NULL;
    }
    
    if (g_pSeedRecordLock != NULL)
    {
        // ⭐ 关键：先 destroy 再 free
        portSyncSpinlockDestroy(g_pSeedRecordLock);
        portMemFree(g_pSeedRecordLock);
        g_pSeedRecordLock = NULL;
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
    
    // ⭐ 修复问题5：统计计数并发（使用锁保护）
    // 获取锁并分配记录槽
    // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
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
    
    // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
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
    
    // ⭐ 修复问题5：pParams可能为NULL时的健壮性检查
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
        // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
        g_hookStats.errors++;
        // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
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
        NvU32 sec = 0, usec = 0;
        NvU64 timeSeed = 0;
        if (osGetCurrentTime(&sec, &usec) == NV_OK)
        {
            timeSeed = (NvU64)sec * 1000000ULL + (NvU64)usec;
        }
        i = (timeSeed % paramsSize);
        pByteParams[i] ^= (1 << (timeSeed % 8));
    }
    
    // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    g_hookStats.inlineFuzzCount++;
    // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
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
    // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    g_hookStats.totalHooks++;
    // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
    portSyncSpinlockRelease(g_pSeedRecordLock);
    
    // 检查是否为RPC路径
    if (pGpu != NULL && IS_GSP_CLIENT(pGpu) && 
        (ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))
    {
        // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
        g_hookStats.rpcHooks++;
        // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
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
        // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
        g_hookStats.localHooks++;
        // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
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
        // ⭐ 修复问题4：检查锁是否已初始化
        if (g_pSeedRecordLock != NULL)
        {
            // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
            portMemCopy(pStats, sizeof(GSP_FUZZ_HOOK_STATS), &g_hookStats, sizeof(GSP_FUZZ_HOOK_STATS));
            // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
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
    
    // ⭐ 修复问题4：检查锁是否已初始化（避免NULL deref）
    if (g_pSeedRecordLock == NULL)
    {
        return NV_ERR_INVALID_STATE;  // Hook未初始化
    }
    
    // ⭐ 修复问题3：检测maxSeedRecords变化并分配/resize ring buffer
    // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
    
    oldMaxRecords = g_hookConfig.maxSeedRecords;
    newMaxRecords = pConfig->maxSeedRecords;
    
    // 如果maxSeedRecords发生变化，需要重新分配buffer
    if (newMaxRecords != oldMaxRecords)
    {
        if (newMaxRecords > 0)
        {
            // 分配新buffer
            NvU32 newBufferSize = newMaxRecords * sizeof(GSP_FUZZ_SEED_RECORD);
            pNewBuffer = portMemAllocNonPaged(newBufferSize);
            if (pNewBuffer == NULL)
            {
                // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
    portSyncSpinlockRelease(g_pSeedRecordLock);
                return NV_ERR_INSUFFICIENT_RESOURCES;
            }
            portMemSet(pNewBuffer, 0, newBufferSize);
        }
        
        // 保存旧buffer以便释放
        pOldBuffer = g_pSeedRecordBuffer;
        
        // 更新配置和buffer
        portMemCopy(&g_hookConfig, sizeof(GSP_FUZZ_HOOK_CONFIG), pConfig, sizeof(GSP_FUZZ_HOOK_CONFIG));
        g_hookConfig.pSeedRecordBuffer = NULL;  // 内核中不使用这个字段
        g_pSeedRecordBuffer = pNewBuffer;
        g_seedRecordIndex = 0;  // 重置索引
        
        g_bHookEnabled = (g_hookConfig.flags & GSP_FUZZ_HOOK_ENABLED) != 0;
        
        // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
    portSyncSpinlockRelease(g_pSeedRecordLock);
        
        // 在锁外释放旧buffer（避免在锁内进行可能阻塞的操作）
        if (pOldBuffer != NULL)
        {
            portMemFree(pOldBuffer);
        }
    }
    else
    {
        // maxSeedRecords未变化，只更新其他配置
        portMemCopy(&g_hookConfig, sizeof(GSP_FUZZ_HOOK_CONFIG), pConfig, sizeof(GSP_FUZZ_HOOK_CONFIG));
        g_hookConfig.pSeedRecordBuffer = NULL;  // 内核中不使用这个字段
        g_bHookEnabled = (g_hookConfig.flags & GSP_FUZZ_HOOK_ENABLED) != 0;
        // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
    portSyncSpinlockRelease(g_pSeedRecordLock);
    }
    
    return NV_OK;
}

// 获取配置
void gspFuzzHookGetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig)
{
    if (pConfig != NULL)
    {
        // ⭐ 修复问题4：检查锁是否已初始化
        if (g_pSeedRecordLock != NULL)
        {
            // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
            portMemCopy(pConfig, sizeof(GSP_FUZZ_HOOK_CONFIG), &g_hookConfig, sizeof(GSP_FUZZ_HOOK_CONFIG));
            // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
    portSyncSpinlockRelease(g_pSeedRecordLock);
        }
        else
        {
            // Hook未初始化，返回默认值
            portMemSet(pConfig, 0, sizeof(GSP_FUZZ_HOOK_CONFIG));
        }
    }
}

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

// 清除统计信息（供IOCTL使用）
void gspFuzzHookClearStats(void)
{
    if (g_pSeedRecordLock != NULL)
    {
        // ⭐ 修复：使用正确的函数名portSyncSpinlockAcquire
    portSyncSpinlockAcquire(g_pSeedRecordLock);
        portMemSet(&g_hookStats, 0, sizeof(g_hookStats));
        g_seedRecordIndex = 0;
        // ⭐ 修复：使用正确的函数名portSyncSpinlockRelease
    portSyncSpinlockRelease(g_pSeedRecordLock);
    }
}

// ⭐ 新增：在锁保护下复制seed记录到用户提供的缓冲区
// 这个函数封装了所有nvport操作，供kernel-open层的gsp_fuzz_ioctl.c使用
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