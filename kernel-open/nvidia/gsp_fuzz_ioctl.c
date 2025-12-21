/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * GSP Fuzz Hook IOCTL处理函数
 * 
 * ⭐ 修复：此文件已移动到kernel-open层，可以访问Linux内核定义
 * ⭐ 修复：使用Linux原生内存函数替代nvport函数
 */

#include "nv.h"
#include "nv-linux.h"  // For NV_IS_SUSER, NV_COPY_TO_USER, Linux error codes (EPERM, EINVAL, etc.)
#include "nvmisc.h"  // For NV_MIN macro
#include "nv-ioctl.h"  // ⭐ 修复：包含kernel-open层的nv-ioctl.h（已包含GSP Fuzz Hook定义）
// ⭐ 修复：包含OS-agnostic层的Hook模块头文件（用于访问内核Hook函数）
// 通过Kbuild设置的include路径访问（-I$(src)/../src/nvidia/inc/kernel）
#include "kernel/gpu/gsp/gsp_fuzz_hook.h"

// ⭐ 修复：不再包含nvport头文件，使用Linux原生函数
// #include "nvport/nvport.h"  // 已移除

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

