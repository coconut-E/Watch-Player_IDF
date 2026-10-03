/*
 * 移植自参考工程 SDScan.cpp (SD 卡初始化与扫描)
 *
 * 移植说明:
 *   - 保留原文件结构、函数名、流程与注释;
 *   - 底层由 SdFat(SPI) 改为 ESP-IDF SDMMC + FATFS:
 *       sd.exists/mkdir/remove      → access() / mkdir() / remove()
 *       FsFile + openNext           → opendir()/readdir() + stat()
 *       file.println/printf         → fprintf()
 *       Arduino String              → snprintf() 到缓冲区
 *   - 路径统一加挂载点前缀 SD_ROOT ("/sdcard")。
 */

#include "sd_scan.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_vfs_fat.h"

#include "arduino_compat.h"
#include "sd_card.h"

// ==================== 挂载点前缀 ====================
#define SD_ROOT SD_CARD_MOUNT_POINT

// ==================== 文件夹配置 ====================
const char* SCANLIST_FOLDER = SD_ROOT "/ScanList";
const char* TRIGGER_FILE = SD_ROOT "/修改请删除此文件以触发重新扫描.txt";

// ==================== 扩展名数组定义 ====================
const char* IMAGE_EXTENSIONS[] = {".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG"};
const char* VIDEO_EXTENSIONS[] = {".mjpeg", ".MJPEG"};
const char* MUSIC_EXTENSIONS[] = {".mp3", ".MP3"};
const char* NOVEL_EXTENSIONS[] = {".txt", ".TXT"};
const char* COMIC_EXTENSIONS[] = {".cmj", ".CMJ"};

// ==================== 扫描配置定义 ====================
FileCategory categories[] = {
    {SD_ROOT "/图片", SD_ROOT "/ScanList/picture.txt", IMAGE_EXTENSIONS, 6, true},
    {SD_ROOT "/视频", SD_ROOT "/ScanList/video.txt", VIDEO_EXTENSIONS, 2, true},
    {SD_ROOT "/音乐", SD_ROOT "/ScanList/music.txt", MUSIC_EXTENSIONS, 2, true},
    {SD_ROOT "/小说", SD_ROOT "/ScanList/novel.txt", NOVEL_EXTENSIONS, 2, true},
    {SD_ROOT "/漫画", SD_ROOT "/ScanList/comic.txt", COMIC_EXTENSIONS, 2, true}
};
const int CATEGORY_COUNT = sizeof(categories) / sizeof(categories[0]);

// ==================== 回调函数指针 ====================
static ScanProgressCallback scanProgressCallback = nullptr;
static ScanErrorCallback scanErrorCallback = nullptr;
static ScanCompleteCallback scanCompleteCallback = nullptr;

// ==================== 全局状态变量定义 ====================
SDScanStatus scanStatus = {
    false,   // sdInitialized
    0,       // totalSizeMB
    0,       // freeSizeMB
    false,   // rescanRequired
    false,   // rescanPerformed
    0,       // scanTimeMs
    0,       // totalFilesFound
    {0, 0, 0, 0, 0},  // categoryFiles 初始化为5个0
    "",      // lastError
    -1,      // currentCategoryIndex
    0        // currentFileCount
};

// ==================== 文件系统小工具 (对应 SdFat 的 sd.exists/mkdir/remove) ====================
static bool sd_exists(const char* path) {
    return access(path, F_OK) == 0;
}

static bool sd_mkdir(const char* path) {
    return mkdir(path, 0777) == 0;
}

static bool sd_remove(const char* path) {
    return remove(path) == 0;
}

// ==================== 回调函数设置 ====================
void setScanProgressCallback(ScanProgressCallback callback) {
    scanProgressCallback = callback;
}

void setScanErrorCallback(ScanErrorCallback callback) {
    scanErrorCallback = callback;
}

void setScanCompleteCallback(ScanCompleteCallback callback) {
    scanCompleteCallback = callback;
}

// ==================== 函数实现 ====================

// 初始化SD卡
bool initializeSDCard() {
    // 参考工程每次 initializeSDCard() 都会重新 sd.begin(),
    // 这里对应为: 已挂载则先卸载, 保证拿到的是"当前这张卡"的全新挂载
    if (sd_card_is_mounted()) {
        sd_card_deinit();
    }

    // 初始化 SD 卡 (SDMMC + FATFS, 见 sd_card.c)
    if (sd_card_init() != ESP_OK) {
        snprintf(scanStatus.lastError, SD_ERROR_LEN, "%s", "SD卡初始化失败");
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError);
        }
        scanStatus.sdInitialized = false;
        return false;
    }

    // 获取SD卡信息
    scanStatus.sdInitialized = true;

    uint64_t totalBytes = 0, freeBytes = 0;
    if (esp_vfs_fat_info(SD_ROOT, &totalBytes, &freeBytes) == ESP_OK) {
        scanStatus.totalSizeMB = (uint32_t)(totalBytes / (1024 * 1024));
        scanStatus.freeSizeMB  = (uint32_t)(freeBytes / (1024 * 1024));
    }

    return true;
}

// 检查是否需要重新扫描
bool checkNeedRescan() {
    scanStatus.rescanRequired = !sd_exists(TRIGGER_FILE);
    return scanStatus.rescanRequired;
}

// 确保目录和文件存在
void ensureDirectoriesAndFiles() {
    // 创建ScanList目录
    if (!sd_exists(SCANLIST_FOLDER)) {
        if (!sd_mkdir(SCANLIST_FOLDER)) {
            if (scanErrorCallback) {
                scanErrorCallback("创建ScanList目录失败");
            }
        }
    }

    // 创建目标扫描目录
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        const char* folder = categories[i].folder;
        if (!sd_exists(folder)) {
            if (!sd_mkdir(folder)) {
                char err[SD_ERROR_LEN];
                snprintf(err, sizeof(err), "创建目录失败: %s", folder);
                if (scanErrorCallback) {
                    scanErrorCallback(err);
                }
            }
        }
    }

    if (!sd_exists(SD_ROOT "/壁纸")) {
        sd_mkdir(SD_ROOT "/壁纸");
    }

    // 检查并初始化列表文件
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        const char* listFile = categories[i].listFile;
        if (!sd_exists(listFile)) {
            initEmptyListFile(listFile);
        }
    }
}

// 初始化空列表文件
void initEmptyListFile(const char* listFile) {
    FILE* file = fopen(listFile, "w");
    if (!file) {
        char err[SD_ERROR_LEN];
        snprintf(err, sizeof(err), "初始化空列表文件失败: %s", listFile);
        if (scanErrorCallback) {
            scanErrorCallback(err);
        }
        return;
    }
    unsigned long scanTime = millis();
    fprintf(file, "ScanTime:%lu\n", scanTime);
    fprintf(file, "FileCount:0\n");
    fflush(file);
    fclose(file);
}

// 创建触发文件
void createTriggerFile() {
    FILE* triggerFile = fopen(TRIGGER_FILE, "w");
    if (triggerFile) {
        fprintf(triggerFile, "此文件用于控制SD卡扫描。\n");
        fprintf(triggerFile, "如需重新扫描所有文件夹，请删除此文件。\n");
        fprintf(triggerFile, "\n");
        fprintf(triggerFile, "最后扫描时间: %lums\n", (unsigned long)millis());
        fflush(triggerFile);
        fclose(triggerFile);
    } else {
        if (scanErrorCallback) {
            scanErrorCallback("无法创建触发文件");
        }
    }
}

// 删除触发文件
void deleteTriggerFile() {
    if (sd_exists(TRIGGER_FILE)) {
        if (!sd_remove(TRIGGER_FILE)) {
            if (scanErrorCallback) {
                scanErrorCallback("无法删除触发文件");
            }
        }
    }
}


// 扫描指定类别
bool scanCategory(FileCategory& category, int categoryIndex) {
    scanStatus.currentCategoryIndex = categoryIndex;
    scanStatus.currentFileCount = 0;

    // 通知开始扫描当前类别
    if (scanProgressCallback) {
        scanProgressCallback(categoryIndex, 0, category.folder);
    }

    // 前置检查
    if (!sd_exists(category.folder)) {
        snprintf(scanStatus.lastError, SD_ERROR_LEN, "目标文件夹不存在: %s", category.folder);
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError);
        }
        return false;
    }

    // 打开列表文件（覆盖写入）
    FILE* listFile = fopen(category.listFile, "w");
    if (!listFile) {
        snprintf(scanStatus.lastError, SD_ERROR_LEN, "无法创建/打开列表文件: %s", category.listFile);
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError);
        }
        return false;
    }


    // 打开目标文件夹
    DIR* rootFolder = opendir(category.folder);
    if (!rootFolder) {
        snprintf(scanStatus.lastError, SD_ERROR_LEN, "无法打开目标文件夹: %s", category.folder);
        if (scanErrorCallback) {
            scanErrorCallback(scanStatus.lastError);
        }
        fclose(listFile);
        return false;
    }

    struct dirent* subFile;
    int fileCount = 0;
    char** fileList = new char*[MAX_FILES_PER_CATEGORY];

    // 收集文件信息
    while ((subFile = readdir(rootFolder)) != NULL) {
        char filename[MAX_PATH_LENGTH];
        snprintf(filename, sizeof(filename), "%s", subFile->d_name);

        if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
            continue;
        }

        // 判断是否为普通文件 (对应 subFile.isFile())
        char fullPath[MAX_PATH_LENGTH + MAX_PATH_LENGTH];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", category.folder, filename);
        struct stat st;
        if (stat(fullPath, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (isFileType(filename, category.extensions, category.extCount)) {

            // 仅存储文件名
            fileList[fileCount] = new char[strlen(filename) + 1];
            strcpy(fileList[fileCount], filename);

            fileCount++;
            scanStatus.currentFileCount = fileCount;

            // 更新状态信息
            scanStatus.totalFilesFound++;

            // 回调进度更新
            if (scanProgressCallback) {
                scanProgressCallback(categoryIndex, fileCount, category.folder);
            }

            if (fileCount >= MAX_FILES_PER_CATEGORY) {
                if (scanErrorCallback) {
                    scanErrorCallback("文件数量超过限制，停止扫描");
                }
                break;
            }
        }
    }
    closedir(rootFolder);

    // 写入文件列表：仅写文件名，每行一个
    for (int i = 0; i < fileCount; i++) {
        fprintf(listFile, "%s\n", fileList[i]); // 直接打印文件名，换行分隔
    }

    // 文件数量放到最后一行，格式FileCount:数字
    fprintf(listFile, "FileCount:%d\n", fileCount);

    fflush(listFile);
    fclose(listFile);

    // 清理内存：仅释放文件名数组
    for (int i = 0; i < fileCount; i++) {
        delete[] fileList[i];
    }
    delete[] fileList;


    // 更新类别文件计数（边界改为 CATEGORY_COUNT）
    if (categoryIndex >= 0 && categoryIndex < CATEGORY_COUNT) {
        scanStatus.categoryFiles[categoryIndex] = fileCount;
    }

    // 重置当前扫描状态
    scanStatus.currentCategoryIndex = -1;
    scanStatus.currentFileCount = 0;

    return true;
}

// 写入文件列表
void writeFileList(FileCategory& category) {
    // 查找类别索引
    int categoryIndex = -1;
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (strcmp(category.folder, categories[i].folder) == 0) {
            categoryIndex = i;
            break;
        }
    }
    scanCategory(category, categoryIndex);
}

// 检查文件类型
bool isFileType(const char* filename, const char** extensions, int extCount) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;

    for (int i = 0; i < extCount; i++) {
        if (strcasecmp(dot, extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

// 执行扫描流程
void performScan() {
    if (scanProgressCallback) {
        scanProgressCallback(-1, 0, "开始执行扫描流程...");
    }

    // 删除旧列表文件
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (sd_exists(categories[i].listFile)) {
            sd_remove(categories[i].listFile);
        }
    }

    // 重新创建列表文件
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        initEmptyListFile(categories[i].listFile);
    }

    // 重置扫描状态（使用 CATEGORY_COUNT）
    scanStatus.totalFilesFound = 0;
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        scanStatus.categoryFiles[i] = 0;
    }

    // 执行扫描
    unsigned long totalStartTime = millis();

    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (categories[i].shouldScan) {
            scanCategory(categories[i], i);
        }
    }

    scanStatus.scanTimeMs = millis() - totalStartTime;
    scanStatus.rescanPerformed = true;

    // 创建标记文件
    createTriggerFile();

    // 回调扫描完成
    if (scanCompleteCallback) {
        scanCompleteCallback();
    }
}

// ==================== 状态信息获取函数 ====================

// 获取扫描状态
SDScanStatus getScanStatus() {
    return scanStatus;
}

// 打印扫描状态
void printScanStatus() {
    Serial.println("\n========== SD卡扫描状态 ==========");
    Serial.printf("SD卡初始化: %s\n", scanStatus.sdInitialized ? "成功" : "失败");
    Serial.printf("SD卡总容量: %u MB\n", scanStatus.totalSizeMB);
    Serial.printf("SD卡剩余空间: %u MB\n", scanStatus.freeSizeMB);
    Serial.printf("重新扫描需要: %s\n", scanStatus.rescanRequired ? "是" : "否");
    Serial.printf("重新扫描已执行: %s\n", scanStatus.rescanPerformed ? "是" : "否");
    Serial.printf("扫描总耗时: %lu ms\n", scanStatus.scanTimeMs);
    Serial.printf("总共找到文件: %d 个\n", scanStatus.totalFilesFound);

    if (scanStatus.rescanPerformed) {
        Serial.println("各类别文件数量:");
        Serial.printf("  图片: %d 个\n", scanStatus.categoryFiles[0]);
        Serial.printf("  视频: %d 个\n", scanStatus.categoryFiles[1]);
        Serial.printf("  音乐: %d 个\n", scanStatus.categoryFiles[2]);
        Serial.printf("  小说: %d 个\n", scanStatus.categoryFiles[3]);
        Serial.printf("  漫画: %d 个\n", scanStatus.categoryFiles[4]);
    }

    if (scanStatus.lastError[0] != '\0') {
        Serial.printf("最后错误: %s\n", scanStatus.lastError);
    }
    Serial.println("=================================\n");
}

// 检查SD卡是否就绪
bool isSDCardReady() {
    return scanStatus.sdInitialized;
}

// 获取SD卡总容量
uint32_t getSDCardSizeMB() {
    return scanStatus.totalSizeMB;
}

// 获取SD卡剩余空间
uint32_t getSDCardFreeSpaceMB() {
    return scanStatus.freeSizeMB;
}

// 获取总共找到的文件数
int getTotalFilesFound() {
    return scanStatus.totalFilesFound;
}

// 获取指定类别的文件数
int getCategoryFileCount(int categoryIndex) {
    if (categoryIndex >= 0 && categoryIndex < CATEGORY_COUNT) {
        return scanStatus.categoryFiles[categoryIndex];
    }
    return 0;
}

// 获取当前正在扫描的类别索引
int getCurrentCategoryIndex() {
    return scanStatus.currentCategoryIndex;
}

// 获取当前类别已扫描的文件数
int getCurrentFileCount() {
    return scanStatus.currentFileCount;
}

// 重置扫描状态
void resetScanStatus() {
    scanStatus = {
        scanStatus.sdInitialized,  // 保持SD卡初始化状态
        scanStatus.totalSizeMB,    // 保持容量信息
        scanStatus.freeSizeMB,     // 保持剩余空间
        false,                     // 重置重新扫描需要
        false,                     // 重置重新扫描已执行
        0,                         // 重置扫描时间
        0,                         // 重置总文件数
        {0, 0, 0, 0, 0},           // 重置各分类文件数（5个0）
        "",                        // 清空错误信息
        -1,                        // 重置当前类别索引
        0                          // 重置当前文件数
    };
}
