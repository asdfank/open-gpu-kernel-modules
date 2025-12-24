#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <sys/ioctl.h>  // for _IOWR
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <dirent.h>

// 引入命令码对照表
#include "nv_ctrl_cmd_table.h"

// ============================================================================
// 用户态类型定义（与内核头文件保持一致）
// ============================================================================
typedef uint8_t  NvU8;
typedef uint16_t NvU16;
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef int32_t  NvS32;
typedef NvU32    NvHandle;
typedef NvU32    NvBool;

// NV_ALIGN_BYTES 宏（用户态版本）
#ifndef NV_ALIGN_BYTES
#define NV_ALIGN_BYTES(x) __attribute__((aligned(x)))
#endif

// ============================================================================
// IOCTL 命令号定义
// ============================================================================
#define NV_IOCTL_MAGIC      'F'
#define NV_IOCTL_BASE       200
#define NV_ESC_IOCTL_XFER_CMD   (NV_IOCTL_BASE + 11)
#define NV_ESC_GSP_FUZZ_HOOK    (NV_IOCTL_BASE + 19)

// ============================================================================
// NV IOCTL XFER 结构（用于传递大型数据）
// ============================================================================
typedef struct nv_ioctl_xfer
{
    NvU32   cmd;
    NvU32   size;
    NvU64   ptr NV_ALIGN_BYTES(8);
} nv_ioctl_xfer_t;

// 构造正确的 IOCTL 命令号（包含 size 信息）
// 注意：内核用 _IOC_NR(cmd) 提取的是完整的 nr，即 211，不是 11
#define NV_IOCTL_XFER_CMD  _IOWR(NV_IOCTL_MAGIC, NV_ESC_IOCTL_XFER_CMD, nv_ioctl_xfer_t)

// ============================================================================
// GSP Fuzz Hook 子命令定义
// ============================================================================
#define GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG    1
#define GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG    2
#define GSP_FUZZ_HOOK_SUBCMD_GET_STATS     3
#define GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS     4
#define GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS   5

// Hook配置标志
#define GSP_FUZZ_HOOK_ENABLED           0x00000001
#define GSP_FUZZ_HOOK_RECORD_SEED       0x00000002
#define GSP_FUZZ_HOOK_INLINE_FUZZ       0x00000004
#define GSP_FUZZ_HOOK_RECORD_RESPONSE   0x00000008
#define GSP_FUZZ_HOOK_HOOK2_ENABLED     0x00000010  // ⭐ 启用 Hook 点 2

// ⭐ 种子来源类型
#define GSP_FUZZ_SEED_SOURCE_HOOK1_PROLOGUE     0x01  // 来自 Hook 点 1
#define GSP_FUZZ_SEED_SOURCE_HOOK2_RPC          0x02  // 来自 Hook 点 2
#define GSP_FUZZ_SEED_SOURCE_HOOK2_BYPASS       0x04  // Hook 点 2: 绕过 Prologue
#define GSP_FUZZ_SEED_SOURCE_HOOK2_INTERNAL     0x08  // Hook 点 2: 驱动内部触发
#define GSP_FUZZ_SEED_SOURCE_SERIALIZED         0x10  // 参数已序列化

// 最大参数大小
#define GSP_FUZZ_MAX_PARAMS_SIZE (64 * 1024)

// ============================================================================
// IOCTL 结构体定义（与内核保持一致）
// ============================================================================

// 用户态配置结构
typedef struct nv_ioctl_gsp_fuzz_hook_config
{
    NvU32 flags;
    NvU32 maxSeedRecords;
    NvU32 inlineFuzzProbability;
    NvU64 seedRecordBufferAddr NV_ALIGN_BYTES(8);
    NvU32 seedRecordBufferSize;
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
    // ⭐ Hook 点 2 统计
    NvU64 hook2TotalHooks NV_ALIGN_BYTES(8);
    NvU64 hook2BypassHooks NV_ALIGN_BYTES(8);
    NvU64 hook2InternalHooks NV_ALIGN_BYTES(8);
    NvU64 hook2SerializedHooks NV_ALIGN_BYTES(8);
    NvU64 hook2Duplicates NV_ALIGN_BYTES(8);
    NvU64 hook2SeedRecords NV_ALIGN_BYTES(8);
} nv_ioctl_gsp_fuzz_hook_stats_t;

// 获取种子记录
typedef struct nv_ioctl_gsp_fuzz_hook_get_seeds
{
    NvU32 startIndex;
    NvU32 count;
    NvU64 seedRecordBufferAddr NV_ALIGN_BYTES(8);
    NvU32 seedRecordBufferSize;
    NvU32 actualCount;
} nv_ioctl_gsp_fuzz_hook_get_seeds_t;

// 统一的IOCTL请求结构
typedef struct nv_ioctl_gsp_fuzz_hook_request
{
    NvU32 subcmd;
    union {
        nv_ioctl_gsp_fuzz_hook_config_t config;
        nv_ioctl_gsp_fuzz_hook_stats_t stats;
        nv_ioctl_gsp_fuzz_hook_get_seeds_t get_seeds;
    } u;
} nv_ioctl_gsp_fuzz_hook_request_t;

// 种子记录结构（简化版，用于输出显示）
typedef struct nv_gsp_fuzz_seed_record
{
    NvU32 hClient;
    NvU32 hObject;
    NvU32 cmd;
    NvU32 paramsSize;
    NvU32 ctrlFlags;
    NvU32 ctrlAccessRight;
    NvU8  params[GSP_FUZZ_MAX_PARAMS_SIZE];
    NvU64 timestamp NV_ALIGN_BYTES(8);
    NvU32 gpuInstance;
    NvU32 bGspClient;
    NvU32 responseStatus;
    NvU32 responseParamsSize;
    NvU8  responseParams[GSP_FUZZ_MAX_PARAMS_SIZE];
    NvU64 latencyUs NV_ALIGN_BYTES(8);
    NvU32 sequence;
    // ⭐ Hook 点 2 扩展字段
    NvU8  seedSource;      // 种子来源: GSP_FUZZ_SEED_SOURCE_*
    NvU8  bSerialized;     // 参数是否已序列化
    NvU16 reserved;        // 保留对齐
} nv_gsp_fuzz_seed_record_t;

// ============================================================================
// 全局变量
// ============================================================================
#define NVIDIA_DEVICE_PATH "/dev/nvidia0"
#define SEED_OUTPUT_BASE_DIR "./gsp_fuzz_seeds"
#define SEED_FILE_PREFIX "seed_"
#define SUMMARY_FILENAME "summary.csv"

static volatile int g_running = 1;
static char g_session_dir[512] = {0};  // 当前会话的输出目录
static FILE *g_summary_fp = NULL;      // Summary文件指针
static NvU32 g_saved_seed_count = 0;   // 已保存的种子数量

// 信号处理函数
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    printf("\n收到退出信号，正在停止...\n");
}

// ============================================================================
// 种子存储相关函数
// ============================================================================

// 创建基础目录（如果不存在）
static int ensure_base_dir(void)
{
    struct stat st = {0};
    if (stat(SEED_OUTPUT_BASE_DIR, &st) == -1) {
        if (mkdir(SEED_OUTPUT_BASE_DIR, 0755) != 0) {
            perror("创建基础目录失败");
            return -1;
        }
    }
    return 0;
}

// 创建会话目录（以时间戳命名）
static int create_session_dir(void)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
    snprintf(g_session_dir, sizeof(g_session_dir), "%s/%s", 
             SEED_OUTPUT_BASE_DIR, timestamp);
    
    if (mkdir(g_session_dir, 0755) != 0) {
        perror("创建会话目录失败");
        return -1;
    }
    
    printf("📁 创建种子输出目录: %s\n", g_session_dir);
    return 0;
}

// 打开summary文件 (CSV格式)
static int open_summary_file(void)
{
    char summary_path[576];
    snprintf(summary_path, sizeof(summary_path), "%s/%s", 
             g_session_dir, SUMMARY_FILENAME);
    
    g_summary_fp = fopen(summary_path, "w");
    if (!g_summary_fp) {
        perror("打开summary文件失败");
        return -1;
    }
    
    // 写入CSV头部
    fprintf(g_summary_fp, "序号,序列号,命令码,命令名称,类别,种子来源,已序列化,参数大小,响应大小,hClient,hObject,响应状态,延迟(us),GPU实例,GSP客户端,控制标志,时间戳\n");
    fflush(g_summary_fp);
    
    return 0;
}

// 关闭summary文件
static void close_summary_file(void)
{
    if (g_summary_fp) {
        fclose(g_summary_fp);
        g_summary_fp = NULL;
        printf("📝 Summary CSV已保存: %s/%s (共 %u 条记录)\n", 
               g_session_dir, SUMMARY_FILENAME, g_saved_seed_count);
    }
}

// 保存单个种子到文件
static int save_seed_to_file(const nv_gsp_fuzz_seed_record_t *seed, NvU32 index)
{
    char seed_path[576];
    const char *cmd_name;
    const char *cat_name;
    FILE *fp;
    
    // 获取命令信息
    cmd_name = nv_lookup_ctrl_cmd_name(seed->cmd);
    cat_name = nv_get_cmd_category_name(seed->cmd);
    
    if (!cmd_name) {
        cmd_name = "UNKNOWN";
    }
    
    // 构建文件名: seed_XXXX_CMD.bin
    snprintf(seed_path, sizeof(seed_path), "%s/%s%04u_0x%08X.bin",
             g_session_dir, SEED_FILE_PREFIX, index, seed->cmd);
    
    // 写入二进制数据
    fp = fopen(seed_path, "wb");
    if (!fp) {
        perror("创建种子文件失败");
        return -1;
    }
    
    // 写入完整的种子记录结构
    fwrite(seed, sizeof(nv_gsp_fuzz_seed_record_t), 1, fp);
    fclose(fp);
    
    // 写入CSV记录
    if (g_summary_fp) {
        // 获取种子来源字符串
        const char *source_str = "";
        if (seed->seedSource & GSP_FUZZ_SEED_SOURCE_HOOK1_PROLOGUE)
            source_str = "Hook1";
        else if (seed->seedSource & GSP_FUZZ_SEED_SOURCE_HOOK2_RPC) {
            if (seed->seedSource & GSP_FUZZ_SEED_SOURCE_HOOK2_BYPASS)
                source_str = "Hook2-Bypass";
            else if (seed->seedSource & GSP_FUZZ_SEED_SOURCE_HOOK2_INTERNAL)
                source_str = "Hook2-Internal";
            else
                source_str = "Hook2";
        }
        
        // 处理命令名称中可能的逗号（用引号包裹）
        fprintf(g_summary_fp, "%u,%u,0x%08X,\"%s\",%s,%s,%s,%u,%u,0x%08X,0x%08X,0x%08X,%llu,%u,%s,0x%08X,%llu\n",
                index, seed->sequence, seed->cmd, cmd_name, cat_name,
                source_str,
                seed->bSerialized ? "Yes" : "No",
                seed->paramsSize, seed->responseParamsSize,
                seed->hClient, seed->hObject,
                seed->responseStatus, (unsigned long long)seed->latencyUs,
                seed->gpuInstance,
                seed->bGspClient ? "Yes" : "No",
                seed->ctrlFlags,
                (unsigned long long)seed->timestamp);
        fflush(g_summary_fp);
    }
    
    g_saved_seed_count++;
    return 0;
}

// 批量保存种子
static int save_seeds_batch(const nv_gsp_fuzz_seed_record_t *seeds, NvU32 count, NvU32 start_index)
{
    for (NvU32 i = 0; i < count; i++) {
        if (save_seed_to_file(&seeds[i], start_index + i) != 0) {
            return -1;
        }
    }
    return 0;
}

// 初始化种子存储系统
static int init_seed_storage(void)
{
    if (ensure_base_dir() != 0) {
        return -1;
    }
    if (create_session_dir() != 0) {
        return -1;
    }
    if (open_summary_file() != 0) {
        return -1;
    }
    g_saved_seed_count = 0;
    return 0;
}

// 清理种子存储系统
static void cleanup_seed_storage(void)
{
    close_summary_file();
    if (g_session_dir[0] != '\0') {
        printf("✅ 共保存 %u 个种子到 %s\n", g_saved_seed_count, g_session_dir);
    }
}

// 打开NVIDIA设备
static int open_nvidia_device(void)
{
    int fd = open(NVIDIA_DEVICE_PATH, O_RDWR);
    if (fd < 0)
    {
        perror("无法打开NVIDIA设备");
        printf("请确保：\n");
        printf("  1. NVIDIA驱动已加载 (lsmod | grep nvidia)\n");
        printf("  2. 您有足够的权限访问/dev/nvidia0\n");
        printf("  3. 如果需要，请使用 sudo 运行\n");
        return -1;
    }
    return fd;
}

// ============================================================================
// 辅助函数：使用 XFER 机制调用 GSP Fuzz Hook IOCTL
// ============================================================================
static int gsp_fuzz_hook_ioctl(int fd, nv_ioctl_gsp_fuzz_hook_request_t *req)
{
    nv_ioctl_xfer_t xfer;
    int ret;
    
    memset(&xfer, 0, sizeof(xfer));
    xfer.cmd = NV_ESC_GSP_FUZZ_HOOK;
    xfer.size = sizeof(nv_ioctl_gsp_fuzz_hook_request_t);
    xfer.ptr = (NvU64)(uintptr_t)req;
    
    ret = ioctl(fd, NV_IOCTL_XFER_CMD, &xfer);
    return ret;
}

// 设置Hook配置
// ⭐ 修复问题4：使用统一的NV_ESC_GSP_FUZZ_HOOK接口，通过subcmd分发
int gsp_fuzz_hook_set_config(
    int fd,
    NvU32 flags,
    NvU32 maxSeedRecords,
    NvU32 inlineFuzzProbability
)
{
    nv_ioctl_gsp_fuzz_hook_request_t req = {0};
    int ret;
    
    req.subcmd = GSP_FUZZ_HOOK_SUBCMD_SET_CONFIG;
    req.u.config.flags = flags;
    req.u.config.maxSeedRecords = maxSeedRecords;
    req.u.config.inlineFuzzProbability = inlineFuzzProbability;
    
    ret = gsp_fuzz_hook_ioctl(fd, &req);
    if (ret < 0)
    {
        perror("Failed to set hook config");
        return -1;
    }
    
    return 0;
}

// 获取统计信息
// ⭐ 修复问题4：使用统一的NV_ESC_GSP_FUZZ_HOOK接口
int gsp_fuzz_hook_get_stats(int fd, nv_ioctl_gsp_fuzz_hook_stats_t *pStats)
{
    nv_ioctl_gsp_fuzz_hook_request_t req = {0};
    int ret;
    
    req.subcmd = GSP_FUZZ_HOOK_SUBCMD_GET_STATS;
    
    ret = gsp_fuzz_hook_ioctl(fd, &req);
    if (ret < 0)
    {
        perror("Failed to get hook stats");
        return -1;
    }
    
    // 复制结果
    *pStats = req.u.stats;
    
    return 0;
}

// 获取种子记录
// ⭐ 修复问题4：使用统一的NV_ESC_GSP_FUZZ_HOOK接口
int gsp_fuzz_hook_get_seeds(
    int fd,
    NvU32 startIndex,
    NvU32 count,
    nv_gsp_fuzz_seed_record_t *pSeeds,
    NvU32 *pActualCount
)
{
    nv_ioctl_gsp_fuzz_hook_request_t req = {0};
    int ret;
    
    req.subcmd = GSP_FUZZ_HOOK_SUBCMD_GET_SEEDS;
    req.u.get_seeds.startIndex = startIndex;
    req.u.get_seeds.count = count;
    req.u.get_seeds.seedRecordBufferAddr = (NvU64)(uintptr_t)pSeeds;
    req.u.get_seeds.seedRecordBufferSize = count * sizeof(nv_gsp_fuzz_seed_record_t);
    
    ret = gsp_fuzz_hook_ioctl(fd, &req);
    if (ret < 0)
    {
        perror("Failed to get seeds");
        return -1;
    }
    
    if (pActualCount != NULL)
    {
        *pActualCount = req.u.get_seeds.actualCount;
    }
    
    return 0;
}

// 打印帮助信息
static void print_usage(const char *prog)
{
    printf("\n用法: %s [options]\n", prog);
    printf("\n选项:\n");
    printf("  -h, --help           显示此帮助信息\n");
    printf("  -s, --stats          仅获取并显示统计信息\n");
    printf("  -e, --enable         启用 Hook 点 1 和 Hook 点 2 并开始记录种子\n");
    printf("  -1, --enable-hook1   仅启用 Hook 点 1 (Prologue)\n");
    printf("  -2, --enable-hook2   启用 Hook 点 2 (序列化后 RPC，需要同时启用 Hook 点 1)\n");
    printf("  -d, --disable        禁用 Hook\n");
    printf("  -c, --clear          清除统计信息\n");
    printf("  -g, --get-seeds N    获取前N个种子记录(仅显示)\n");
    printf("  -S, --save-seeds N   获取并保存前N个种子到文件\n");
    printf("  -m, --monitor        持续监控模式（每5秒打印统计）\n");
    printf("  -M, --monitor-save   持续监控并自动保存新种子\n");
    printf("\n示例:\n");
    printf("  sudo %s -s           # 查看当前统计信息\n", prog);
    printf("  sudo %s -e           # 启用 Hook 点 1 和 Hook 点 2\n", prog);
    printf("  sudo %s -1           # 仅启用 Hook 点 1\n", prog);
    printf("  sudo %s -m           # 持续监控\n", prog);
    printf("  sudo %s -g 10        # 获取并显示10个种子\n", prog);
    printf("  sudo %s -S 100       # 获取并保存100个种子到文件\n", prog);
    printf("  sudo %s -e -M        # 启用Hook并持续保存种子\n", prog);
    printf("\n种子保存位置: %s/<时间戳>/\n", SEED_OUTPUT_BASE_DIR);
    printf("\nHook 点说明:\n");
    printf("  Hook 点 1: rmresControl_Prologue - 捕获标准 RM 路径的 RPC（原始参数）\n");
    printf("  Hook 点 2: rpcRmApiControl_GSP - 捕获所有 RPC（包括绕过 Prologue 和已序列化的）\n");
    printf("\n统计口径:\n");
    printf("  Hook2 总数 = 标准路径(duplicate) + 绕过Prologue + 内部触发\n");
    printf("  - 标准路径: Hook1 已记录，Hook2 不重复记录种子\n");
    printf("  - 绕过Prologue: 用户态调用但未经 Hook1，Hook2 记录种子\n");
    printf("  - 内部触发: 驱动内部调用，Hook2 记录种子\n");
    printf("\n");
}

// 清除统计信息
int gsp_fuzz_hook_clear_stats(int fd)
{
    nv_ioctl_gsp_fuzz_hook_request_t req = {0};
    int ret;
    
    req.subcmd = GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS;
    
    ret = gsp_fuzz_hook_ioctl(fd, &req);
    if (ret < 0)
    {
        perror("清除统计信息失败");
        return -1;
    }
    
    return 0;
}

// 获取配置
int gsp_fuzz_hook_get_config(int fd, nv_ioctl_gsp_fuzz_hook_config_t *pConfig)
{
    nv_ioctl_gsp_fuzz_hook_request_t req = {0};
    int ret;
    
    req.subcmd = GSP_FUZZ_HOOK_SUBCMD_GET_CONFIG;
    
    ret = gsp_fuzz_hook_ioctl(fd, &req);
    if (ret < 0)
    {
        perror("获取配置失败");
        return -1;
    }
    
    *pConfig = req.u.config;
    return 0;
}

// 打印统计信息
static void print_stats(const nv_ioctl_gsp_fuzz_hook_stats_t *stats)
{
    printf("\n============ GSP Fuzz Hook 统计信息 ============\n");
    printf("--- Hook 点 1 (Prologue) ---\n");
    printf("总 Hook 次数:      %llu\n", (unsigned long long)stats->totalHooks);
    printf("RPC 路径 Hook:     %llu\n", (unsigned long long)stats->rpcHooks);
    printf("本地路径 Hook:     %llu\n", (unsigned long long)stats->localHooks);
    printf("在线 Fuzz 次数:   %llu\n", (unsigned long long)stats->inlineFuzzCount);
    printf("错误次数:         %llu\n", (unsigned long long)stats->errors);
    printf("--- Hook 点 2 (RPC 发送点) ---\n");
    printf("总 Hook 次数:      %llu  (= 实际 RPC 发送次数)\n", (unsigned long long)stats->hook2TotalHooks);
    printf("--- RPC 来源分类 (互斥统计) ---\n");
    printf("来自 Prologue:   %llu  (标准 RM 路径，已被 Hook1 记录)\n", (unsigned long long)stats->hook2Duplicates);
    printf("绕过 Prologue:   %llu  (用户态调用但未经 Hook1)\n", (unsigned long long)stats->hook2BypassHooks);
    printf("内部触发:        %llu  (驱动内部触发，无用户上下文)\n", (unsigned long long)stats->hook2InternalHooks);
    printf("已序列化 API:    %llu  (Hook2 独有，FINN 序列化后)\n", (unsigned long long)stats->hook2SerializedHooks);
    printf("--- 种子统计 ---\n");
    printf("Hook2 新增种子:  %llu  (非 duplicate，Hook2 独有)\n", (unsigned long long)stats->hook2SeedRecords);
    printf("种子总数:        %llu (Hook1: %llu + Hook2: %llu)\n", 
           (unsigned long long)stats->seedRecords,
           (unsigned long long)(stats->seedRecords - stats->hook2SeedRecords),
           (unsigned long long)stats->hook2SeedRecords);
    printf("================================================\n");
    printf("\n统计口径说明:\n");
    printf("  - hook2TotalHooks = 来自Prologue + 绕过Prologue + 内部触发\n");
    printf("  - 来自Prologue/绕过/内部 是互斥的来源分类，总和 = hook2TotalHooks\n");
    printf("  - 已序列化 只统计 Hook2 独有的（因为 Hook1 记录原始参数）\n");
    printf("  - 新增种子 只统计 Hook2 独有的（非 duplicate）\n");
}

// 打印配置信息
static void print_config(const nv_ioctl_gsp_fuzz_hook_config_t *config)
{
    printf("\n============ GSP Fuzz Hook 配置 ============\n");
    printf("Hook 点 1 状态:    %s\n", (config->flags & GSP_FUZZ_HOOK_ENABLED) ? "已启用" : "已禁用");
    printf("Hook 点 2 状态:    %s\n", (config->flags & GSP_FUZZ_HOOK_HOOK2_ENABLED) ? "已启用" : "已禁用");
    printf("记录种子:         %s\n", (config->flags & GSP_FUZZ_HOOK_RECORD_SEED) ? "开启" : "关闭");
    printf("在线 Fuzz:        %s\n", (config->flags & GSP_FUZZ_HOOK_INLINE_FUZZ) ? "开启" : "关闭");
    printf("记录响应:         %s\n", (config->flags & GSP_FUZZ_HOOK_RECORD_RESPONSE) ? "开启" : "关闭");
    printf("最大种子记录数:   %u\n", config->maxSeedRecords);
    printf("在线 Fuzz 概率:   %u%%\n", config->inlineFuzzProbability);
    printf("=============================================\n");
}

// 获取种子来源字符串
static const char* get_seed_source_str(NvU8 seedSource)
{
    static char buf[64];
    buf[0] = '\0';
    
    if (seedSource & GSP_FUZZ_SEED_SOURCE_HOOK1_PROLOGUE)
        strcat(buf, "Hook1 ");
    if (seedSource & GSP_FUZZ_SEED_SOURCE_HOOK2_RPC)
        strcat(buf, "Hook2 ");
    if (seedSource & GSP_FUZZ_SEED_SOURCE_HOOK2_BYPASS)
        strcat(buf, "绕过Prologue ");
    if (seedSource & GSP_FUZZ_SEED_SOURCE_HOOK2_INTERNAL)
        strcat(buf, "内部触发 ");
    if (seedSource & GSP_FUZZ_SEED_SOURCE_SERIALIZED)
        strcat(buf, "已序列化");
    
    if (buf[0] == '\0')
        strcpy(buf, "未知");
    
    return buf;
}

// 打印种子记录摘要
static void print_seed_summary(const nv_gsp_fuzz_seed_record_t *seed, NvU32 index)
{
    const char *cmd_name = nv_lookup_ctrl_cmd_name(seed->cmd);
    const char *cls_name = nv_get_cmd_class_name(seed->cmd);
    const char *cat_name = nv_get_cmd_category_name(seed->cmd);
    
    printf("\n--- 种子 #%u ---\n", index);
    printf("  序列号:     %u\n", seed->sequence);
    printf("  命令:       0x%08X\n", seed->cmd);
    printf("  命令名称:   %s\n", cmd_name ? cmd_name : "UNKNOWN");
    printf("  命令类别:   %s::%s\n", cls_name, cat_name);
    printf("  hClient:    0x%08X\n", seed->hClient);
    printf("  hObject:    0x%08X\n", seed->hObject);
    printf("  参数大小:   %u 字节\n", seed->paramsSize);
    printf("  控制标志:   0x%08X\n", seed->ctrlFlags);
    printf("  GPU实例:    %u\n", seed->gpuInstance);
    printf("  GSP客户端: %s\n", seed->bGspClient ? "是" : "否");
    printf("  响应状态:   0x%08X\n", seed->responseStatus);
    printf("  延迟:       %llu 微秒\n", (unsigned long long)seed->latencyUs);
    printf("  ⭐ 种子来源: %s\n", get_seed_source_str(seed->seedSource));
    printf("  ⭐ 已序列化: %s\n", seed->bSerialized ? "是" : "否");
    
    // 打印前16字节参数（如果有）
    if (seed->paramsSize > 0)
    {
        printf("  参数前16字节: ");
        NvU32 printLen = seed->paramsSize < 16 ? seed->paramsSize : 16;
        for (NvU32 i = 0; i < printLen; i++)
        {
            printf("%02X ", seed->params[i]);
        }
        printf("\n");
    }
}

// 主函数
int main(int argc, char *argv[])
{
    int fd;
    nv_ioctl_gsp_fuzz_hook_stats_t stats;
    nv_ioctl_gsp_fuzz_hook_config_t config;
    int opt_stats = 0;
    int opt_enable = 0;
    int opt_enable_hook1 = 0;
    int opt_enable_hook2 = 0;
    int opt_disable = 0;
    int opt_clear = 0;
    int opt_monitor = 0;
    int opt_monitor_save = 0;
    int opt_get_seeds = 0;
    int opt_save_seeds = 0;
    NvU32 seed_count = 0;
    NvU32 save_seed_count = 0;
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stats") == 0)
        {
            opt_stats = 1;
        }
        else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--enable") == 0)
        {
            opt_enable = 1;
        }
        else if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--enable-hook1") == 0)
        {
            opt_enable_hook1 = 1;
        }
        else if (strcmp(argv[i], "-2") == 0 || strcmp(argv[i], "--enable-hook2") == 0)
        {
            opt_enable_hook2 = 1;
        }
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--disable") == 0)
        {
            opt_disable = 1;
        }
        else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clear") == 0)
        {
            opt_clear = 1;
        }
        else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--monitor") == 0)
        {
            opt_monitor = 1;
        }
        else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--get-seeds") == 0)
        {
            opt_get_seeds = 1;
            if (i + 1 < argc)
            {
                seed_count = atoi(argv[++i]);
                if (seed_count == 0 || seed_count > 1000)
                {
                    seed_count = 10;  // 默认10个
                }
            }
            else
            {
                seed_count = 10;
            }
        }
        else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--save-seeds") == 0)
        {
            opt_save_seeds = 1;
            if (i + 1 < argc)
            {
                save_seed_count = atoi(argv[++i]);
                if (save_seed_count == 0 || save_seed_count > 10000)
                {
                    save_seed_count = 100;  // 默认100个
                }
            }
            else
            {
                save_seed_count = 100;
            }
        }
        else if (strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "--monitor-save") == 0)
        {
            opt_monitor_save = 1;
        }
        else
        {
            printf("未知选项: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // 如果没有指定任何选项，显示统计和配置
    if (!opt_stats && !opt_enable && !opt_disable && !opt_clear && !opt_monitor && !opt_monitor_save && !opt_get_seeds && !opt_save_seeds)
    {
        opt_stats = 1;
    }
    
    // 打开设备
    fd = open_nvidia_device();
    if (fd < 0)
    {
        return 1;
    }
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 清除统计
    if (opt_clear)
    {
        if (gsp_fuzz_hook_clear_stats(fd) == 0)
        {
            printf("✅ 统计信息已清除\n");
        }
    }
    
    // 禁用Hook
    if (opt_disable)
    {
        if (gsp_fuzz_hook_set_config(fd, 0, 1024, 0) == 0)
        {
            printf("✅ Hook 已禁用\n");
        }
    }
    
    // 启用Hook
    if (opt_enable || opt_enable_hook1 || opt_enable_hook2)
    {
        NvU32 flags = GSP_FUZZ_HOOK_ENABLED | GSP_FUZZ_HOOK_RECORD_SEED | GSP_FUZZ_HOOK_RECORD_RESPONSE;
        
        // -e 默认启用两个 Hook 点
        if (opt_enable)
        {
            flags |= GSP_FUZZ_HOOK_HOOK2_ENABLED;
            printf("正在启用 GSP Fuzz Hook (点1 + 点2)...\n");
        }
        // -1 仅启用 Hook 点 1
        else if (opt_enable_hook1 && !opt_enable_hook2)
        {
            printf("正在启用 GSP Fuzz Hook (仅点1 Prologue)...\n");
        }
        // -2 启用 Hook 点 2 (需要同时启用 Hook 点 1)
        else if (opt_enable_hook2)
        {
            flags |= GSP_FUZZ_HOOK_HOOK2_ENABLED;
            printf("正在启用 GSP Fuzz Hook (点1 + 点2)...\n");
        }
        
        if (gsp_fuzz_hook_set_config(
                fd,
                flags,
                1024,  // 最多记录1024个种子
                0      // 不进行在线Fuzz（测试时建议先禁用）
            ) == 0)
        {
            printf("✅ Hook 已启用，正在记录种子...\n");
        }
    }
    
    // 显示统计和配置
    if (opt_stats)
    {
        if (gsp_fuzz_hook_get_config(fd, &config) == 0)
        {
            print_config(&config);
        }
        
        if (gsp_fuzz_hook_get_stats(fd, &stats) == 0)
        {
            print_stats(&stats);
        }
    }
    
    // 获取种子（仅显示）
    if (opt_get_seeds && seed_count > 0)
    {
        printf("\n正在获取前 %u 个种子记录...\n", seed_count);
        
        // 分配缓冲区
        nv_gsp_fuzz_seed_record_t *seeds = malloc(seed_count * sizeof(nv_gsp_fuzz_seed_record_t));
        if (seeds == NULL)
        {
            printf("内存分配失败\n");
        }
        else
        {
            NvU32 actualCount = 0;
            memset(seeds, 0, seed_count * sizeof(nv_gsp_fuzz_seed_record_t));
            
            if (gsp_fuzz_hook_get_seeds(fd, 0, seed_count, seeds, &actualCount) == 0)
            {
                printf("✅ 获取到 %u 个种子记录\n", actualCount);
                
                for (NvU32 i = 0; i < actualCount; i++)
                {
                    print_seed_summary(&seeds[i], i);
                }
            }
            else
            {
                printf("❌ 获取种子失败\n");
            }
            
            free(seeds);
        }
    }
    
    // 获取并保存种子到文件
    if (opt_save_seeds && save_seed_count > 0)
    {
        printf("\n正在获取并保存前 %u 个种子记录...\n", save_seed_count);
        
        // 初始化存储系统
        if (init_seed_storage() != 0)
        {
            printf("❌ 初始化种子存储失败\n");
        }
        else
        {
            // 逐个获取种子（每个种子约128KB，批量获取会导致内核内存分配失败）
            const NvU32 BATCH_SIZE = 1;
            NvU32 totalSaved = 0;
            NvU32 remaining = save_seed_count;
            NvU32 startIndex = 0;
            
            nv_gsp_fuzz_seed_record_t *seeds = malloc(BATCH_SIZE * sizeof(nv_gsp_fuzz_seed_record_t));
            if (seeds == NULL)
            {
                printf("内存分配失败\n");
            }
            else
            {
                printf("每个种子约 %zu KB，逐个获取中...\n", sizeof(nv_gsp_fuzz_seed_record_t) / 1024);
                while (remaining > 0 && g_running)
                {
                    NvU32 batchCount = remaining < BATCH_SIZE ? remaining : BATCH_SIZE;
                    NvU32 actualCount = 0;
                    
                    memset(seeds, 0, BATCH_SIZE * sizeof(nv_gsp_fuzz_seed_record_t));
                    
                    if (gsp_fuzz_hook_get_seeds(fd, startIndex, batchCount, seeds, &actualCount) == 0)
                    {
                        if (actualCount == 0)
                        {
                            printf("没有更多种子可获取\n");
                            break;
                        }
                        
                        // 保存到文件
                        if (save_seeds_batch(seeds, actualCount, totalSaved) != 0)
                        {
                            printf("❌ 保存种子失败\n");
                            break;
                        }
                        
                        totalSaved += actualCount;
                        startIndex += actualCount;
                        remaining -= actualCount;
                        
                        printf("已保存: %u/%u\r", totalSaved, save_seed_count);
                        fflush(stdout);
                    }
                    else
                    {
                        printf("❌ 获取种子失败\n");
                        break;
                    }
                }
                
                printf("\n");
                free(seeds);
            }
            
            cleanup_seed_storage();
        }
    }
    
    // 监控模式
    if (opt_monitor)
    {
        printf("\n进入监控模式，每5秒打印统计信息... (按 Ctrl+C 退出)\n");
        
        while (g_running)
        {
            sleep(5);
            if (!g_running) break;
            
            if (gsp_fuzz_hook_get_stats(fd, &stats) == 0)
            {
                time_t now = time(NULL);
                struct tm *tm_info = localtime(&now);
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
                
                printf("[%s] H1:%llu (RPC:%llu) | H2:%llu (标准:%llu 绕过:%llu 内部:%llu) | Seeds:%llu | Err:%llu\n",
                       time_str,
                       (unsigned long long)stats.totalHooks,
                       (unsigned long long)stats.rpcHooks,
                       (unsigned long long)stats.hook2TotalHooks,
                       (unsigned long long)stats.hook2Duplicates,
                       (unsigned long long)stats.hook2BypassHooks,
                       (unsigned long long)stats.hook2InternalHooks,
                       (unsigned long long)stats.seedRecords,
                       (unsigned long long)stats.errors);
            }
        }
    }
    
    // 持续监控并保存种子模式
    if (opt_monitor_save)
    {
        printf("\n进入持续监控并保存模式... (按 Ctrl+C 退出)\n");
        
        // 初始化存储系统
        if (init_seed_storage() != 0)
        {
            printf("❌ 初始化种子存储失败\n");
        }
        else
        {
            // 逐个获取种子（每个种子约128KB，批量获取会导致内核内存分配失败）
            const NvU32 BATCH_SIZE = 1;
            NvU64 lastSeedCount = 0;
            NvU32 savedCount = 0;
            
            nv_gsp_fuzz_seed_record_t *seeds = malloc(BATCH_SIZE * sizeof(nv_gsp_fuzz_seed_record_t));
            if (seeds == NULL)
            {
                printf("内存分配失败\n");
            }
            else
            {
                printf("每个种子约 %zu KB，逐个获取中...\n", sizeof(nv_gsp_fuzz_seed_record_t) / 1024);
                // 获取初始统计
                if (gsp_fuzz_hook_get_stats(fd, &stats) == 0)
                {
                    lastSeedCount = stats.seedRecords;
                    printf("当前已有 %llu 个种子记录\n", (unsigned long long)lastSeedCount);
                }
                
                while (g_running)
                {
                    sleep(2);  // 每2秒检查一次
                    if (!g_running) break;
                    
                    if (gsp_fuzz_hook_get_stats(fd, &stats) == 0)
                    {
                        NvU64 currentSeedCount = stats.seedRecords;
                        
                        // 如果有新种子
                        if (currentSeedCount > lastSeedCount)
                        {
                            NvU32 newSeeds = (NvU32)(currentSeedCount - lastSeedCount);
                            printf("发现 %u 个新种子，正在保存...\n", newSeeds);
                            
                            // 获取新种子
                            NvU32 startIndex = (NvU32)lastSeedCount;
                            while (newSeeds > 0 && g_running)
                            {
                                NvU32 batchCount = newSeeds < BATCH_SIZE ? newSeeds : BATCH_SIZE;
                                NvU32 actualCount = 0;
                                
                                memset(seeds, 0, BATCH_SIZE * sizeof(nv_gsp_fuzz_seed_record_t));
                                
                                if (gsp_fuzz_hook_get_seeds(fd, startIndex, batchCount, seeds, &actualCount) == 0)
                                {
                                    if (actualCount == 0) break;
                                    
                                    if (save_seeds_batch(seeds, actualCount, savedCount) == 0)
                                    {
                                        savedCount += actualCount;
                                        startIndex += actualCount;
                                        newSeeds -= actualCount;
                                    }
                                }
                                else
                                {
                                    break;
                                }
                            }
                            
                            lastSeedCount = currentSeedCount;
                        }
                        
                        // 打印状态
                        time_t now = time(NULL);
                        struct tm *tm_info = localtime(&now);
                        char time_str[32];
                        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
                        
                        printf("[%s] H1:%llu H2:%llu | Seeds(kernel): %llu+%llu | Saved: %u\r",
                               time_str,
                               (unsigned long long)stats.totalHooks,
                               (unsigned long long)stats.hook2TotalHooks,
                               (unsigned long long)stats.seedRecords,
                               (unsigned long long)stats.hook2SeedRecords,
                               savedCount);
                        fflush(stdout);
                    }
                }
                
                printf("\n");
                free(seeds);
            }
            
            cleanup_seed_storage();
        }
    }
    
    close(fd);
    printf("\n完成\n");
    return 0;
}