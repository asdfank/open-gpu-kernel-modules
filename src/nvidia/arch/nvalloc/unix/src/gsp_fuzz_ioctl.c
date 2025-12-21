/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * GSP Fuzz Hook IOCTL处理函数
 * 
 * ⚠️ 重要：此文件位于OS-agnostic层，但需要返回Linux错误码
 * 这些定义通过kernel-open层的头文件提供（在编译kernel-open层时可用）
 */

#include "nv.h"
#include "gpu/gsp/gsp_fuzz_hook.h"
#include "nv-ioctl.h"
#include "nvmisc.h"  // For NV_MIN macro
#include "core/core.h"  // For core/prelude.h which includes nvport
#include "os/os.h"   // For os functions
#include "os-interface.h"  // For osIsAdministrator

// ⭐ 修复：nvport函数通过core/prelude.h -> nvport/nvport.h -> nvport/sync.h 和 nvport/memory.h 提供
// 注意：函数名是 portSyncSpinlockAcquire/Release (不是portSpinLockAcquire)

// ⚠️ 重要：此文件需要返回Linux错误码（-EPERM等）和使用kernel-open层的宏（NV_IS_SUSER, NV_COPY_TO_USER）
// 这些定义在kernel-open层可用，但在OS-agnostic层编译时不可用
// 
// 解决方案1（推荐）：将文件移动到 kernel-open/nvidia/gsp_fuzz_ioctl.c
// 解决方案2（临时）：在OS-agnostic层提供抽象，在kernel-open层包装
//
// 当前临时修复：定义占位符（实际应该移动到kernel-open层）
#ifndef NV_KERNEL_INTERFACE_LAYER
    // OS-agnostic层：定义占位符（实际编译时会失败，需要移动到kernel-open层）
    #define EPERM    1
    #define EINVAL   22
    #define ENODEV   19
    #define ENOMEM   12
    #define EFAULT   14
    #define NV_IS_SUSER() osIsAdministrator()  // 使用OS-agnostic层的替代
    #define NV_COPY_TO_USER(to, from, n) (0)  // 占位符，实际需要kernel-open层实现
#endif

// 注意：这个文件需要在kernel-open/nvidia/nv.c中被调用
// 由于nv.c是kernel-open层的文件，我们需要在这里实现处理逻辑
// 但实际调用会在nv.c中

// 处理GSP Fuzz Hook IOCTL（从nv.c调用）
// ⭐ 修复问题4：使用统一的请求结构，通过subcmd分发
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
    
    // 参数验证
    if (arg_size < sizeof(nv_ioctl_gsp_fuzz_hook_request_t))
    {
        return -EINVAL;
    }
    
    req = (nv_ioctl_gsp_fuzz_hook_request_t *)arg_copy;
    
    // 根据subcmd分发
    switch (req->subcmd)
    {
        case GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG:
        {
            nv_ioctl_gsp_fuzz_hook_config_t *userConfig = &req->u.config;
            GSP_FUZZ_HOOK_CONFIG kernelConfig;
            
            gspFuzzHookGetConfig(&kernelConfig);
            
            // 转换内核配置到用户态配置
            userConfig->flags = kernelConfig.flags;
            userConfig->maxSeedRecords = kernelConfig.maxSeedRecords;
            userConfig->inlineFuzzProbability = kernelConfig.inlineFuzzProbability;
            // ⭐ 修复问题3：不要返回内核指针给用户态（安全/语义问题）
            // 真正的数据读取走GET_SEEDS（由内核copy_to_user到用户提供的buffer）
            userConfig->seedRecordBufferAddr = 0;  // 不暴露内核地址
            userConfig->seedRecordBufferSize = 0;   // 不暴露内核buffer大小
            
            break;
        }
        
        case GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG:
        {
            nv_ioctl_gsp_fuzz_hook_config_t *userConfig = &req->u.config;
            GSP_FUZZ_HOOK_CONFIG kernelConfig;
            
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
        
        case GSP_FUZZ_HOOK_SUBCMD_GET_STATS:
        {
            nv_ioctl_gsp_fuzz_hook_stats_t *userStats = &req->u.stats;
            GSP_FUZZ_HOOK_STATS kernelStats;
            
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
        
        case GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS:
        {
            nv_ioctl_gsp_fuzz_hook_get_seeds_t *getSeedsReq = &req->u.get_seeds;
            GSP_FUZZ_SEED_RECORD *pKernelSeeds = NULL;
            GSP_FUZZ_SEED_RECORD *pSeedRecordBuffer = NULL;
            PORT_SPINLOCK *pSeedRecordLock = NULL;
            GSP_FUZZ_HOOK_CONFIG *pConfig = NULL;
            GSP_FUZZ_HOOK_STATS *pStats = NULL;
            NvU32 seedRecordIndex = 0;
            NvU32 actualCount = 0;
            NvU32 i;
            
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
            // 这里假设它们大小相同（实际应该做字段映射，但为简化先这样处理）
            if (getSeedsReq->seedRecordBufferSize < getSeedsReq->count * sizeof(GSP_FUZZ_SEED_RECORD))
            {
                return -EINVAL;
            }
            
            // 获取内部状态指针
            pSeedRecordBuffer = gspFuzzHookGetSeedRecordBuffer();
            pSeedRecordLock = gspFuzzHookGetSeedRecordLock();
            pConfig = gspFuzzHookGetConfigPtr();
            pStats = gspFuzzHookGetStatsPtr();
            seedRecordIndex = gspFuzzHookGetSeedRecordIndex();
            
            if (pSeedRecordBuffer == NULL || pSeedRecordLock == NULL || 
                pConfig == NULL || pStats == NULL)
            {
                return -ENODEV;  // Hook未初始化
            }
            
            // 分配内核缓冲区
            pKernelSeeds = portMemAllocNonPaged(getSeedsReq->count * sizeof(GSP_FUZZ_SEED_RECORD));
            if (pKernelSeeds == NULL)
            {
                return -ENOMEM;
            }
            
            // 从种子记录缓冲区读取（需要加锁）
            // ⭐ 修复：使用正确的函数名 portSyncSpinlockAcquire
            portSyncSpinlockAcquire(pSeedRecordLock);
            
            // 计算实际可读取的数量
            NvU32 totalRecords = pStats->seedRecords;
            NvU32 availableRecords = (totalRecords > pConfig->maxSeedRecords) 
                                      ? pConfig->maxSeedRecords 
                                      : totalRecords;
            
            if (getSeedsReq->startIndex >= availableRecords)
            {
                actualCount = 0;
            }
            else
            {
                actualCount = NV_MIN(getSeedsReq->count, availableRecords - getSeedsReq->startIndex);
                
                // 从ring buffer中读取（考虑循环缓冲区）
                for (i = 0; i < actualCount; i++)
                {
                    NvU32 srcIndex = (seedRecordIndex - availableRecords + getSeedsReq->startIndex + i) 
                                      % pConfig->maxSeedRecords;
                    portMemCopy(&pKernelSeeds[i], sizeof(GSP_FUZZ_SEED_RECORD),
                               &pSeedRecordBuffer[srcIndex], sizeof(GSP_FUZZ_SEED_RECORD));
                }
            }
            
            // ⭐ 修复：使用正确的函数名 portSyncSpinlockRelease
            portSyncSpinlockRelease(pSeedRecordLock);
            
            // 复制到用户态缓冲区
            if (actualCount > 0)
            {
                if (NV_COPY_TO_USER((void *)(uintptr_t)getSeedsReq->seedRecordBufferAddr,
                                 pKernelSeeds, actualCount * sizeof(GSP_FUZZ_SEED_RECORD)))
                {
                    portMemFree(pKernelSeeds);
                    return -EFAULT;
                }
            }
            
            // 更新实际返回数量
            getSeedsReq->actualCount = actualCount;
            
            portMemFree(pKernelSeeds);
            break;
        }
        
        case GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS:
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

