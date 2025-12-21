/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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


#ifndef NV_IOCTL_H
#define NV_IOCTL_H

#include <nv-ioctl-numbers.h>
#include <nvtypes.h>

typedef struct {
    NvU32    domain;        /* PCI domain number   */
    NvU8     bus;           /* PCI bus number      */
    NvU8     slot;          /* PCI slot number     */
    NvU8     function;      /* PCI function number */
    NvU16    vendor_id;     /* PCI vendor ID       */
    NvU16    device_id;     /* PCI device ID       */
} nv_pci_info_t;

/*
 * ioctl()'s with parameter structures too large for the
 * _IOC cmd layout use the nv_ioctl_xfer_t structure
 * and the NV_ESC_IOCTL_XFER_CMD ioctl() to pass the actual
 * size and user argument pointer into the RM, which
 * will then copy it to/from kernel space in separate steps.
 */
typedef struct nv_ioctl_xfer
{
    NvU32   cmd;
    NvU32   size;
    NvP64   ptr  NV_ALIGN_BYTES(8);
} nv_ioctl_xfer_t;

typedef struct nv_ioctl_card_info
{
    NvBool        valid;
    nv_pci_info_t pci_info;            /* PCI config information      */
    NvU32         gpu_id;
    NvU16         interrupt_line;
    NvU64         reg_address    NV_ALIGN_BYTES(8);
    NvU64         reg_size       NV_ALIGN_BYTES(8);
    NvU64         fb_address     NV_ALIGN_BYTES(8);
    NvU64         fb_size        NV_ALIGN_BYTES(8);
    NvU32         minor_number;
    NvU8          dev_name[10];  /* device names such as vmgfx[0-32] for vmkernel */
} nv_ioctl_card_info_t;

/* alloc event */
typedef struct nv_ioctl_alloc_os_event
{
    NvHandle hClient;
    NvHandle hDevice;
    NvU32    fd;
    NvU32    Status;
} nv_ioctl_alloc_os_event_t;

/* free event */
typedef struct nv_ioctl_free_os_event
{
    NvHandle hClient;
    NvHandle hDevice;
    NvU32    fd;
    NvU32    Status;
} nv_ioctl_free_os_event_t;

/* status code */
typedef struct nv_ioctl_status_code
{
    NvU32 domain;
    NvU8  bus;
    NvU8  slot;
    NvU32 status;
} nv_ioctl_status_code_t;

/* check version string */
#define NV_RM_API_VERSION_STRING_LENGTH 64

typedef struct nv_ioctl_rm_api_version
{
    NvU32 cmd;
    NvU32 reply;
    char versionString[NV_RM_API_VERSION_STRING_LENGTH];
} nv_ioctl_rm_api_version_t;

#define NV_RM_API_VERSION_CMD_STRICT         0
#define NV_RM_API_VERSION_CMD_RELAXED       '1'
#define NV_RM_API_VERSION_CMD_QUERY         '2'

#define NV_RM_API_VERSION_REPLY_UNRECOGNIZED 0
#define NV_RM_API_VERSION_REPLY_RECOGNIZED   1

typedef struct nv_ioctl_query_device_intr
{
    NvU32 intrStatus NV_ALIGN_BYTES(4);
    NvU32 status;
} nv_ioctl_query_device_intr;

/* system parameters that the kernel driver may use for configuration */
typedef struct nv_ioctl_sys_params
{
    NvU64 memblock_size NV_ALIGN_BYTES(8);
} nv_ioctl_sys_params_t;

typedef struct nv_ioctl_register_fd
{
    int ctl_fd;
} nv_ioctl_register_fd_t;

#define NV_DMABUF_EXPORT_MAX_HANDLES 128

#define NV_DMABUF_EXPORT_MAPPING_TYPE_DEFAULT        0
#define NV_DMABUF_EXPORT_MAPPING_TYPE_FORCE_PCIE     1

typedef struct nv_ioctl_export_to_dma_buf_fd
{
    int         fd;
    NvHandle    hClient;
    NvU32       totalObjects;
    NvU32       numObjects;
    NvU32       index;
    NvU64       totalSize NV_ALIGN_BYTES(8);
    NvU8        mappingType;
    NvHandle    handles[NV_DMABUF_EXPORT_MAX_HANDLES];
    NvU64       offsets[NV_DMABUF_EXPORT_MAX_HANDLES] NV_ALIGN_BYTES(8);
    NvU64       sizes[NV_DMABUF_EXPORT_MAX_HANDLES] NV_ALIGN_BYTES(8);
    NvU32       status;
} nv_ioctl_export_to_dma_buf_fd_t;

typedef struct nv_ioctl_wait_open_complete
{
    int rc;
    NvU32 adapterStatus;
} nv_ioctl_wait_open_complete_t;

/* GSP Fuzz Hook IOCTL structures */
// ⭐ 修复：将GSP Fuzz Hook相关定义添加到kernel-open层，使其可访问
// 子命令定义（用于payload中的subcmd字段）
#define GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG    1
#define GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG    2
#define GSP_FUZZ_HOOK_SUBCMD_GET_STATS     3
#define GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS     4
#define GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS   5

// Hook配置标志（用户态可见）
#define GSP_FUZZ_HOOK_ENABLED           0x00000001
#define GSP_FUZZ_HOOK_RECORD_SEED       0x00000002
#define GSP_FUZZ_HOOK_INLINE_FUZZ       0x00000004
#define GSP_FUZZ_HOOK_RECORD_RESPONSE   0x00000008

// 最大参数大小（与内核保持一致）
#define GSP_FUZZ_MAX_PARAMS_SIZE (64 * 1024)

// 用户态配置结构
typedef struct nv_ioctl_gsp_fuzz_hook_config
{
    NvU32 flags;
    NvU32 maxSeedRecords;
    NvU32 inlineFuzzProbability;
    NvU64 seedRecordBufferAddr NV_ALIGN_BYTES(8);    // 用户态缓冲区地址（保留，暂不使用）
    NvU32 seedRecordBufferSize;    // 用户态缓冲区大小（保留，暂不使用）
} nv_ioctl_gsp_fuzz_hook_config_t;

// 用户态统计结构
typedef struct nv_ioctl_gsp_fuzz_hook_stats
{
    NvU64 totalHooks NV_ALIGN_BYTES(8);
    NvU64 rpcHooks NV_ALIGN_BYTES(8);
    NvU64 localHooks NV_ALIGN_BYTES(8);
    NvU64 seedRecords NV_ALIGN_BYTES(8);
    NvU64 inlineFuzzCount NV_ALIGN_BYTES(8);
    NvU64 errors NV_ALIGN_BYTES(8);
} nv_ioctl_gsp_fuzz_hook_stats_t;

// 获取种子记录
typedef struct nv_ioctl_gsp_fuzz_hook_get_seeds
{
    NvU32 startIndex;               // 起始索引
    NvU32 count;                    // 请求数量
    NvU64 seedRecordBufferAddr NV_ALIGN_BYTES(8);     // 用户态缓冲区地址
    NvU32 seedRecordBufferSize;     // 用户态缓冲区大小
    NvU32 actualCount;               // 实际返回数量（输出）
} nv_ioctl_gsp_fuzz_hook_get_seeds_t;

// 统一的IOCTL请求结构（包含subcmd）
typedef struct nv_ioctl_gsp_fuzz_hook_request
{
    NvU32 subcmd;                   // 子命令（GSP_FUZZ_HOOK_SUBCMD_*）
    union {
        nv_ioctl_gsp_fuzz_hook_config_t config;
        nv_ioctl_gsp_fuzz_hook_stats_t stats;
        nv_ioctl_gsp_fuzz_hook_get_seeds_t get_seeds;
    } u;
} nv_ioctl_gsp_fuzz_hook_request_t;

// 种子记录结构（用户态可见，与内核GSP_FUZZ_SEED_RECORD保持一致）
// ⭐ 注意：必须与内核的GSP_FUZZ_SEED_RECORD结构体布局完全一致
typedef struct nv_gsp_fuzz_seed_record
{
    NvU32 hClient;              // NvHandle
    NvU32 hObject;              // NvHandle
    NvU32 cmd;
    NvU32 paramsSize;
    NvU32 ctrlFlags;
    NvU32 ctrlAccessRight;
    NvU8  params[GSP_FUZZ_MAX_PARAMS_SIZE];
    NvU64 timestamp NV_ALIGN_BYTES(8);
    NvU32 gpuInstance;  
    NvU32 bGspClient;           // NvBool
    NvU32 responseStatus;       // NV_STATUS
    NvU32 responseParamsSize;
    NvU8  responseParams[GSP_FUZZ_MAX_PARAMS_SIZE];
    NvU64 latencyUs NV_ALIGN_BYTES(8);
    NvU32 sequence;
} nv_gsp_fuzz_seed_record_t;

#endif
