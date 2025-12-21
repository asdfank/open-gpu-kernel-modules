/*
 * SPDX-FileCopyrightText: Copyright (c) 2018-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "resserv/rs_client.h"
#include "resserv/rs_server.h"
#include "rmapi/client.h"
#include "rmapi/resource.h"
#include "rmapi/rmapi.h"
#include "rmapi/rmapi_utils.h"
#include "rmapi/control.h"
#include "ctrl/ctrlxxxx.h"
#include "gpu/gpu_resource.h"
#include "gpu/gpu.h"
#include "gpu_mgr/gpu_mgr.h"
#include "vgpu/rpc.h"
#include "core/locks.h"

#include "gpu/gsp/gsp_fuzz_hook.h"

NV_STATUS
rmrescmnConstruct_IMPL
(
    RmResourceCommon *pResourceCommmon
)
{
    return NV_OK;
}

NV_STATUS
rmresConstruct_IMPL
(
    RmResource *pResource,
    CALL_CONTEXT *pCallContext,
    RS_RES_ALLOC_PARAMS_INTERNAL *pParams
)
{
    if (RS_IS_COPY_CTOR(pParams))
    {
        RmResource *pSrcResource = dynamicCast(pParams->pSrcRef->pResource, RmResource);

        pResource->rpcGpuInstance = pSrcResource->rpcGpuInstance;
        pResource->bRpcFree = pSrcResource->bRpcFree;
    }
    else
    {
        pResource->rpcGpuInstance = ~0;
        pResource->bRpcFree = NV_FALSE;
    }

    return NV_OK;
}

NvBool
rmresAccessCallback_IMPL
(
    RmResource *pResource,
    RsClient *pInvokingClient,
    void *pAllocParams,
    RsAccessRight accessRight
)
{
    NV_STATUS status;
    RsResourceRef *pCliResRef;

    status = clientGetResourceRef(RES_GET_CLIENT(pResource),
                                  RES_GET_CLIENT_HANDLE(pResource),
                                  &pCliResRef);

    if (status == NV_OK)
    {
        // Allow access if the resource's owner would get the access right
        if(resAccessCallback(pCliResRef->pResource, pInvokingClient, pAllocParams, accessRight))
            return NV_TRUE;
    }

    // Delegate to superclass
    return resAccessCallback_IMPL(staticCast(pResource, RsResource), pInvokingClient, pAllocParams, accessRight);
}

NvBool
rmresShareCallback_IMPL
(
    RmResource *pResource,
    RsClient *pInvokingClient,
    RsResourceRef *pParentRef,
    RS_SHARE_POLICY *pSharePolicy
)
{
    NV_STATUS status;
    RsResourceRef *pCliResRef;

    //
    // cliresShareCallback contains some require exceptions for non-GpuResource,
    // which we don't want to hit. ClientResource doesn't normally implement these
    // share types anyway, so we're fine with skipping them.
    //
    switch (pSharePolicy->type)
    {
        case RS_SHARE_TYPE_SMC_PARTITION:
        case RS_SHARE_TYPE_GPU:
        {
            //
            // We do not want to lock down these GpuResource-specific require policies
            // when the check cannot be applied for other resources, so add these checks
            // as an alternative bypass for those policies
            //
            if ((pSharePolicy->action & RS_SHARE_ACTION_FLAG_REQUIRE) &&
                (NULL == dynamicCast(pResource, GpuResource)))
            {
                return NV_TRUE;
            }
            break;
        }
        case RS_SHARE_TYPE_FM_CLIENT:
        {
            RmClient *pSrcClient = dynamicCast(RES_GET_CLIENT(pResource), RmClient);
            NvBool bSrcIsKernel = (pSrcClient != NULL) && (rmclientGetCachedPrivilege(pSrcClient) >= RS_PRIV_LEVEL_KERNEL);

            if (rmclientIsCapable(dynamicCast(pInvokingClient, RmClient),
                                  NV_RM_CAP_EXT_FABRIC_MGMT) && !bSrcIsKernel)
            {
                return NV_TRUE;
            }
            break;
        }
        default:
        {
            status = clientGetResourceRef(RES_GET_CLIENT(pResource),
                                          RES_GET_CLIENT_HANDLE(pResource),
                                          &pCliResRef);
            if (status == NV_OK)
            {
                // Allow sharing if the resource's owner would be shared with
                if (resShareCallback(pCliResRef->pResource, pInvokingClient,
                                     pParentRef, pSharePolicy))
                    return NV_TRUE;
            }
            break;
        }
    }

    // Delegate to superclass
    return resShareCallback_IMPL(staticCast(pResource, RsResource),
                                 pInvokingClient, pParentRef, pSharePolicy);
}

void serverControl_InitCookie
(
    const struct NVOC_EXPORTED_METHOD_DEF   *exportedEntry,
    RmCtrlExecuteCookie                     *pRmCtrlExecuteCookie
)
{
    // Copy from NVOC exportedEntry
    pRmCtrlExecuteCookie->cmd       = exportedEntry->methodId;
    pRmCtrlExecuteCookie->ctrlFlags = exportedEntry->flags;
    // One time initialization of a const variable
    *(NvU32 *)&pRmCtrlExecuteCookie->rightsRequired.limbs[0]
                                    = exportedEntry->accessRight;
}

NV_STATUS
rmresGetMemInterMapParams_IMPL
(
    RmResource                 *pRmResource,
    RMRES_MEM_INTER_MAP_PARAMS *pParams
)
{
    return NV_ERR_INVALID_OBJECT_HANDLE;
}

NV_STATUS
rmresCheckMemInterUnmap_IMPL
(
    RmResource *pRmResource,
    NvBool      bSubdeviceHandleProvided
)
{
    return NV_ERR_INVALID_OBJECT_HANDLE;
}

NV_STATUS
rmresGetMemoryMappingDescriptor_IMPL
(
    RmResource *pRmResource,
    struct MEMORY_DESCRIPTOR **ppMemDesc
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS
rmresControlSerialization_Prologue_IMPL
(
    RmResource                     *pResource,
    CALL_CONTEXT                   *pCallContext,
    RS_RES_CONTROL_PARAMS_INTERNAL *pParams
)
{
    OBJGPU *pGpu = gpumgrGetGpu(pResource->rpcGpuInstance);

    if (pGpu != NULL &&
        ((IS_VIRTUAL(pGpu)    && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST)
        ) || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
    {
        return serverSerializeCtrlDown(pCallContext, pParams->cmd, &pParams->pParams, &pParams->paramsSize, &pParams->flags);
    }
    else
    {
        NV_CHECK_OK_OR_RETURN(LEVEL_ERROR, serverDeserializeCtrlDown(pCallContext, pParams->cmd, &pParams->pParams, &pParams->paramsSize, &pParams->flags));
    }

    return NV_OK;
}

void
rmresControlSerialization_Epilogue_IMPL
(
    RmResource                     *pResource,
    CALL_CONTEXT                   *pCallContext,
    RS_RES_CONTROL_PARAMS_INTERNAL *pParams
)
{
    OBJGPU *pGpu = gpumgrGetGpu(pResource->rpcGpuInstance);

    if (pGpu != NULL &&
        ((IS_VIRTUAL(pGpu)    && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_VGPU_HOST)
        ) || (IS_GSP_CLIENT(pGpu) && (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))))
    {
        NV_ASSERT_OK(serverDeserializeCtrlUp(pCallContext, pParams->cmd, &pParams->pParams, &pParams->paramsSize, &pParams->flags));
    }

    NV_ASSERT_OK(serverSerializeCtrlUp(pCallContext, pParams->cmd, &pParams->pParams, &pParams->paramsSize, &pParams->flags));
    serverFreeSerializeStructures(pCallContext, pParams->pParams);
}

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
    void *pOriginalParamsCopy = NULL;  // 原始参数的快照（用于记录合法种子）
    NvU32 originalParamsSize = 0;
    NvU32 mutatedParamsSize = 0;
    void *pResponseParams = NULL;
    NvU32 responseParamsSize = 0;
    NvU32 ctrlAccessRight = 0;
    
    // ⭐ Hook点1：在RPC路由检查之前调用Hook（准备变异参数，但不修改原始参数）
    if (pGpu != NULL && IS_GSP_CLIENT(pGpu) && 
        (pParams->pCookie->ctrlFlags & RMCTRL_FLAGS_ROUTE_TO_PHYSICAL))
    {
        // ⭐ 修复问题1：在RPC前复制原始请求参数（避免记录到响应内容）
        // 很多RM control的params buffer是in/out复用的，RPC返回后pParams->pParams可能被写成响应内容
        originalParamsSize = pParams->paramsSize;
        if (pParams->pParams != NULL && originalParamsSize > 0)
        {
            NvU32 copySize = NV_MIN(originalParamsSize, GSP_FUZZ_MAX_PARAMS_SIZE);
            pOriginalParamsCopy = portMemAllocNonPaged(copySize);
            if (pOriginalParamsCopy != NULL)
            {
                portMemCopy(pOriginalParamsCopy, copySize, pParams->pParams, copySize);
                // 如果原始参数大于MAX，只记录能记录的部分
                if (originalParamsSize > GSP_FUZZ_MAX_PARAMS_SIZE)
                {
                    originalParamsSize = GSP_FUZZ_MAX_PARAMS_SIZE;
                }
            }
            else
            {
                // ⭐ 修复问题2：分配失败时清零size，避免"伪seed"（有size但params全0/垃圾）
                originalParamsSize = 0;
            }
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

        // ⭐ 修复问题2：响应记录逻辑
        // 如果启用了inline fuzz，响应可能写在pMutatedParams上，但我们在记录前就free了
        // 方案：如果启用了inline fuzz，禁用响应记录（seed是合法的原始请求，响应是fuzz后的，不匹配）
        void *pResponseParamsCopy = NULL;
        
        if (gspFuzzHookIsResponseRecordingEnabled() && pMutatedParams == NULL)
        {
            // 只有在未启用inline fuzz时才记录响应
            // 响应数据在pParams->pParams中（因为使用的是原始参数）
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
                    // ⭐ 修复问题2：分配失败时清零size，避免"伪seed"
                    responseParamsSize = 0;
                }
            }
        }

        // ⭐ 释放变异参数缓冲区（如果分配了）
        if (pMutatedParams != NULL)
        {
            portMemFree(pMutatedParams);
            pMutatedParams = NULL;
        }

        // ⭐ Hook点1：记录响应（在RPC返回后）
        // 注意：使用原始未变异的参数快照记录种子，保证种子的合法性
        if (pGpu != NULL && IS_GSP_CLIENT(pGpu))
        {
            gspFuzzHook_RmresControlPrologueResponse(
                pGpu,
                pParams->hClient,
                pParams->hObject,
                pParams->cmd,
                pOriginalParamsCopy,  // ⭐ 使用原始参数的快照记录种子
                originalParamsSize,
                pParams->pCookie->ctrlFlags,
                ctrlAccessRight,
                status,
                pResponseParams,  // 响应参数（如果启用且未fuzz）
                responseParamsSize,
                latencyUs  // 真实的RPC往返延迟
            );
        }
        
        // 释放响应参数快照（如果分配了）
        if (pResponseParamsCopy != NULL)
        {
            portMemFree(pResponseParamsCopy);
        }
        
        // ⭐ 修复问题1：RPC路径下释放原始参数快照（避免内存泄漏）
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
    
    // 如果未进入RPC路径，也需要释放分配的缓冲区
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

void
rmresControl_Epilogue_IMPL
(
    RmResource                     *pResource,
    CALL_CONTEXT                   *pCallContext,
    RS_RES_CONTROL_PARAMS_INTERNAL *pParams
)
{
}
