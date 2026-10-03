/*
 * 移植自参考工程 SDScan.h (SD 卡初始化与扫描)
 * 说明: 底层由 SdFat(SPI) 改为 ESP-IDF SDMMC + FATFS (POSIX), 其余结构与函数名保持不变。
 */

#ifndef SD_SCAN_H
#define SD_SCAN_H

#include <stdbool.h>
#include <stdint.h>

// ==================== SD卡硬件引脚定义 (参考工程, 现由 sd_card.c 使用 SDMMC) ====================
#define SD_CS 21
#define SD_MOSI 18
#define SD_CLK 17
#define SD_MISO 16

// ==================== 宏定义和常量 ====================
#define SPI_SPEED 20000000UL
#define MAX_PATH_LENGTH 256
#define MAX_FILES_PER_CATEGORY 1000
#define SD_ERROR_LEN 64

// ==================== 回调函数类型定义 ====================
typedef void (*ScanProgressCallback)(int categoryIndex, int currentCount, const char* categoryName);
typedef void (*ScanErrorCallback)(const char* errorMessage);
typedef void (*ScanCompleteCallback)();

// ==================== 文件夹配置 ====================
extern const char* SCANLIST_FOLDER;
extern const char* TRIGGER_FILE;

// ==================== 文件类型配置 ====================
struct FileCategory {
    const char* folder;        // 要扫描的目标文件夹
    const char* listFile;      // 生成的列表文件路径
    const char** extensions;   // 匹配的文件扩展名
    int extCount;              // 扩展名数量
    bool shouldScan;           // 是否需要扫描
};

// 扩展名数组声明
extern const char* IMAGE_EXTENSIONS[];
extern const char* VIDEO_EXTENSIONS[];
extern const char* MUSIC_EXTENSIONS[];
extern const char* NOVEL_EXTENSIONS[];
extern const char* COMIC_EXTENSIONS[];

// 扫描配置数组声明
extern FileCategory categories[];
extern const int CATEGORY_COUNT;

// ==================== 状态信息结构体 ====================
struct SDScanStatus {
    bool sdInitialized;        // SD卡是否初始化成功
    uint32_t totalSizeMB;      // SD卡总容量(MB)
    uint32_t freeSizeMB;       // SD卡剩余容量(MB)
    bool rescanRequired;       // 是否需要重新扫描
    bool rescanPerformed;      // 本次启动是否执行了重新扫描
    unsigned long scanTimeMs;  // 扫描总耗时(ms)
    int totalFilesFound;       // 总共找到的文件数
    int categoryFiles[5];      // 每个类别找到的文件数 [图片, 视频, 音乐, 小说, 漫画]
    char lastError[SD_ERROR_LEN];  // 最后一次错误信息
    int currentCategoryIndex;  // 当前正在扫描的类别索引
    int currentFileCount;      // 当前类别已扫描到的文件数
};

// ==================== 全局状态变量 ====================
extern SDScanStatus scanStatus;

// ==================== 回调函数设置 ====================
void setScanProgressCallback(ScanProgressCallback callback);
void setScanErrorCallback(ScanErrorCallback callback);
void setScanCompleteCallback(ScanCompleteCallback callback);

// ==================== 函数声明 ====================
// 初始化函数
bool initializeSDCard();
bool checkNeedRescan();

// 目录和文件管理
void ensureDirectoriesAndFiles();
void initEmptyListFile(const char* listFile);
void createTriggerFile();
void deleteTriggerFile();

// 扫描功能
bool scanCategory(FileCategory& category, int categoryIndex);
void writeFileList(FileCategory& category);
bool isFileType(const char* filename, const char** extensions, int extCount);

// 扫描主流程
void performScan();

// 状态信息获取
SDScanStatus getScanStatus();
void printScanStatus();
bool isSDCardReady();
uint32_t getSDCardSizeMB();
uint32_t getSDCardFreeSpaceMB();
int getTotalFilesFound();
int getCategoryFileCount(int categoryIndex);
int getCurrentCategoryIndex();
int getCurrentFileCount();

// 调试和重置
void resetScanStatus();

#endif // SD_SCAN_H
