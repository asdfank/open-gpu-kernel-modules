#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>

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
#define NV_ESC_GSP_FUZZ_HOOK (NV_IOCTL_BASE + 19)

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
} nv_gsp_fuzz_seed_record_t;

// ============================================================================
// 全局变量
// ============================================================================
#define NVIDIA_DEVICE_PATH "/dev/nvidia0"
static volatile int g_running = 1;

// 信号处理函数
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    printf("\n收到退出信号，正在停止...\n");
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
    
    ret = ioctl(fd, NV_ESC_GSP_FUZZ_HOOK, &req);
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
    
    ret = ioctl(fd, NV_ESC_GSP_FUZZ_HOOK, &req);
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
    
    ret = ioctl(fd, NV_ESC_GSP_FUZZ_HOOK, &req);
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
    printf("  -e, --enable         启用Hook并开始记录种子\n");
    printf("  -d, --disable        禁用Hook\n");
    printf("  -c, --clear          清除统计信息\n");
    printf("  -g, --get-seeds N    获取前N个种子记录\n");
    printf("  -m, --monitor        持续监控模式（每5秒打印统计）\n");
    printf("\n示例:\n");
    printf("  sudo %s -s           # 查看当前统计信息\n", prog);
    printf("  sudo %s -e           # 启用Hook\n", prog);
    printf("  sudo %s -m           # 持续监控\n", prog);
    printf("  sudo %s -g 10        # 获取10个种子\n", prog);
    printf("\n");
}

// 清除统计信息
int gsp_fuzz_hook_clear_stats(int fd)
{
    nv_ioctl_gsp_fuzz_hook_request_t req = {0};
    int ret;
    
    req.subcmd = GSP_FUZZ_HOOK_SUBCMD_CLEAR_STATS;
    
    ret = ioctl(fd, NV_ESC_GSP_FUZZ_HOOK, &req);
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
    
    ret = ioctl(fd, NV_ESC_GSP_FUZZ_HOOK, &req);
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
    printf("总 Hook 次数:      %llu\n", (unsigned long long)stats->totalHooks);
    printf("RPC 路径 Hook:     %llu\n", (unsigned long long)stats->rpcHooks);
    printf("本地路径 Hook:     %llu\n", (unsigned long long)stats->localHooks);
    printf("种子记录数:       %llu\n", (unsigned long long)stats->seedRecords);
    printf("在线 Fuzz 次数:   %llu\n", (unsigned long long)stats->inlineFuzzCount);
    printf("错误次数:         %llu\n", (unsigned long long)stats->errors);
    printf("================================================\n");
}

// 打印配置信息
static void print_config(const nv_ioctl_gsp_fuzz_hook_config_t *config)
{
    printf("\n============ GSP Fuzz Hook 配置 ============\n");
    printf("Hook 状态:         %s\n", (config->flags & GSP_FUZZ_HOOK_ENABLED) ? "已启用" : "已禁用");
    printf("记录种子:         %s\n", (config->flags & GSP_FUZZ_HOOK_RECORD_SEED) ? "开启" : "关闭");
    printf("在线 Fuzz:        %s\n", (config->flags & GSP_FUZZ_HOOK_INLINE_FUZZ) ? "开启" : "关闭");
    printf("记录响应:         %s\n", (config->flags & GSP_FUZZ_HOOK_RECORD_RESPONSE) ? "开启" : "关闭");
    printf("最大种子记录数:   %u\n", config->maxSeedRecords);
    printf("在线 Fuzz 概率:   %u%%\n", config->inlineFuzzProbability);
    printf("=============================================\n");
}

// 打印种子记录摘要
static void print_seed_summary(const nv_gsp_fuzz_seed_record_t *seed, NvU32 index)
{
    printf("\n--- 种子 #%u ---\n", index);
    printf("  序列号:     %u\n", seed->sequence);
    printf("  命令:       0x%08X\n", seed->cmd);
    printf("  hClient:    0x%08X\n", seed->hClient);
    printf("  hObject:    0x%08X\n", seed->hObject);
    printf("  参数大小:   %u 字节\n", seed->paramsSize);
    printf("  控制标志:   0x%08X\n", seed->ctrlFlags);
    printf("  GPU实例:    %u\n", seed->gpuInstance);
    printf("  GSP客户端: %s\n", seed->bGspClient ? "是" : "否");
    printf("  响应状态:   0x%08X\n", seed->responseStatus);
    printf("  延迟:       %llu 微秒\n", (unsigned long long)seed->latencyUs);
    
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
    int opt_disable = 0;
    int opt_clear = 0;
    int opt_monitor = 0;
    int opt_get_seeds = 0;
    NvU32 seed_count = 0;
    
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
                if (seed_count == 0 || seed_count > 100)
                {
                    seed_count = 10;  // 默认10个
                }
            }
            else
            {
                seed_count = 10;
            }
        }
        else
        {
            printf("未知选项: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // 如果没有指定任何选项，显示统计和配置
    if (!opt_stats && !opt_enable && !opt_disable && !opt_clear && !opt_monitor && !opt_get_seeds)
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
    if (opt_enable)
    {
        printf("正在启用 GSP Fuzz Hook...\n");
        if (gsp_fuzz_hook_set_config(
                fd,
                GSP_FUZZ_HOOK_ENABLED | GSP_FUZZ_HOOK_RECORD_SEED | GSP_FUZZ_HOOK_RECORD_RESPONSE,
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
    
    // 获取种子
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
                printf("[%s] Total: %llu | RPC: %llu | Local: %llu | Seeds: %llu | Fuzz: %llu | Errors: %llu\n",
                       __TIME__,
                       (unsigned long long)stats.totalHooks,
                       (unsigned long long)stats.rpcHooks,
                       (unsigned long long)stats.localHooks,
                       (unsigned long long)stats.seedRecords,
                       (unsigned long long)stats.inlineFuzzCount,
                       (unsigned long long)stats.errors);
            }
        }
    }
    
    close(fd);
    printf("\n完成\n");
    return 0;
}