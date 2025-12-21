#ifndef _GSP_FUZZ_HOOK_H_
#define _GSP_FUZZ_HOOK_H_

#include "nvtypes.h"
#include "nvstatus.h"

// 前向声明（Forward declarations）
typedef struct OBJGPU OBJGPU;
typedef struct PORT_SPINLOCK PORT_SPINLOCK;

// 最大参数大小（64KB，可根据需要调整）
#define GSP_FUZZ_MAX_PARAMS_SIZE (64 * 1024)

// Hook配置标志
#define GSP_FUZZ_HOOK_ENABLED           0x00000001
#define GSP_FUZZ_HOOK_RECORD_SEED       0x00000002
#define GSP_FUZZ_HOOK_INLINE_FUZZ       0x00000004
#define GSP_FUZZ_HOOK_RECORD_RESPONSE   0x00000008

// 种子记录结构
typedef struct gsp_fuzz_seed_record
{
    // 基本信息
    NvHandle hClient;
    NvHandle hObject;
    NvU32    cmd;
    NvU32    paramsSize;
    NvU32    ctrlFlags;          // RMCTRL_FLAGS_*
    NvU32    ctrlAccessRight;     // 访问权限
    
    // 参数数据（固定大小，避免动态分配）
    NvU8     params[GSP_FUZZ_MAX_PARAMS_SIZE];
    
    // 元数据
    NvU64    timestamp;          // 时间戳（纳秒）
    NvU32    gpuInstance;          // GPU实例ID
    NvBool   bGspClient;          // 是否为GSP客户端模式
    
    // 响应数据（可选）
    NV_STATUS responseStatus;      // 响应状态码
    NvU32     responseParamsSize;  // 响应参数大小
    NvU8      responseParams[GSP_FUZZ_MAX_PARAMS_SIZE];  // 响应参数
    
    // 统计信息
    NvU64    latencyUs;           // 延迟（微秒）
    NvU32    sequence;             // 序列号（用于去重）
} GSP_FUZZ_SEED_RECORD;

// Hook统计信息
typedef struct gsp_fuzz_hook_stats
{
    NvU64 totalHooks;              // 总Hook次数
    NvU64 rpcHooks;                // RPC路径Hook次数
    NvU64 localHooks;               // 本地路径Hook次数
    NvU64 seedRecords;              // 种子记录数
    NvU64 inlineFuzzCount;         // 在线Fuzz次数
    NvU64 errors;                   // 错误次数
} GSP_FUZZ_HOOK_STATS;

// Hook配置
typedef struct gsp_fuzz_hook_config
{
    NvU32 flags;                   // 配置标志
    NvU32 maxSeedRecords;           // 最大种子记录数
    NvU32 inlineFuzzProbability;   // 在线Fuzz概率（0-100）
    NvU32 seedRecordBufferSize;    // 种子记录缓冲区大小
    void  *pSeedRecordBuffer;       // 种子记录缓冲区（内核地址）
} GSP_FUZZ_HOOK_CONFIG;

// Hook模块函数声明
NV_STATUS gspFuzzHookInit(void);
void gspFuzzHookCleanup(void);
void *gspFuzzHook_RmresControlPrologue(
    OBJGPU *pGpu,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pParams,
    NvU32 paramsSize,
    NvU32 ctrlFlags,
    NvU32 *pMutatedParamsSize
);
void gspFuzzHook_RmresControlPrologueResponse(
    OBJGPU *pGpu,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pOriginalParams,
    NvU32 originalParamsSize,
    NvU32 ctrlFlags,
    NvU32 ctrlAccessRight,
    NV_STATUS responseStatus,
    void *pResponseParams,
    NvU32 responseParamsSize,
    NvU64 latencyUs
);
void gspFuzzHookGetStats(GSP_FUZZ_HOOK_STATS *pStats);
NV_STATUS gspFuzzHookSetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig);
void gspFuzzHookGetConfig(GSP_FUZZ_HOOK_CONFIG *pConfig);
NvBool gspFuzzHookIsResponseRecordingEnabled(void);
NvBool gspFuzzHookIsEnabled(void);

// IOCTL处理辅助函数（导出内部状态访问）
NvU32 gspFuzzHookGetSeedRecordIndex(void);
GSP_FUZZ_SEED_RECORD *gspFuzzHookGetSeedRecordBuffer(void);
PORT_SPINLOCK *gspFuzzHookGetSeedRecordLock(void);
GSP_FUZZ_HOOK_CONFIG *gspFuzzHookGetConfigPtr(void);
GSP_FUZZ_HOOK_STATS *gspFuzzHookGetStatsPtr(void);
void gspFuzzHookClearStats(void);

// ⭐ 新增：kernel-open层辅助函数（避免在kernel-open层直接使用nvport函数）
// 在锁保护下复制seed记录到用户提供的缓冲区
// 返回：实际复制的记录数
NvU32 gspFuzzHookCopySeedsLocked(
    NvU32 startIndex,         // 起始索引
    NvU32 requestedCount,     // 请求的记录数
    GSP_FUZZ_SEED_RECORD *pDestBuffer,  // 目标缓冲区（调用者分配）
    NvU32 destBufferCount     // 目标缓冲区可容纳的记录数
);

#endif // _GSP_FUZZ_HOOK_H_