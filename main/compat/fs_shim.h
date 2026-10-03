/*
 * Arduino LittleFS / File 嫁接层
 *
 * 参考工程里 LittleFS 挂在 flash 上的 "littlefs" 分区 (分区表已与参考工程逐字节一致:
 * littlefs, data, littlefs, 0x510000, 0xAE0000 → 10.875MB)。
 * Arduino-ESP32 的 LittleFS 本身就是 esp_littlefs 的封装, 所以这里直接映射到
 * joltwallet/littlefs 组件, 语义与参考工程一致:
 *
 *   LittleFS.begin(true, "/littlefs", 10, "littlefs")
 *       → esp_vfs_littlefs_register({base_path="/littlefs",
 *                                    partition_label="littlefs",
 *                                    format_if_mount_failed=true})
 *   LittleFS.open("/novel_hist.txt", "r+")
 *       → fopen("/littlefs/novel_hist.txt", "r+")
 *   LittleFS.end() → 见 end() 的说明
 *
 * 用途: 小说/漫画阅读进度 (/novel_hist.txt, /comic_hist.txt) 与壁纸
 *       (/wallpaper0..2.bin)。都是频繁写的小文件, 放 flash 才不依赖 SD 卡插着。
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <memory>

#include "esp_littlefs.h"
#include "arduino_compat.h"   /* String */

/* ─────────────── Arduino 文件打开模式宏 (参考工程用到 FILE_READ/FILE_WRITE) ─────────────── */
#ifndef FILE_READ
#define FILE_READ   "r"
#endif
#ifndef FILE_WRITE
#define FILE_WRITE  "w"
#endif
#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

/* ─────────────── LittleFS VFS 挂载点 ─────────────── */
#define LFS_VFS_ROOT     "/littlefs"
#define LFS_VFS_PATH_MAX 320

/* 把 LittleFS 内部路径 "/xxx" 嫁接成 "/littlefs/xxx" */
inline void lfs_graft_path(const char *path, char *out, size_t out_len)
{
    if (!path) {
        out[0] = '\0';
        return;
    }
    if (path[0] == '/') {
        snprintf(out, out_len, LFS_VFS_ROOT "%s", path);
    } else {
        snprintf(out, out_len, LFS_VFS_ROOT "/%s", path);
    }
}

/* ─────────────── File (对应 Arduino fs::File) ─────────────── */
struct _FileCloser {
    void operator()(FILE *f) const
    {
        if (f) fclose(f);
    }
};

class File {
public:
    File() {}
    explicit File(FILE *fp) : _fp(fp, _FileCloser()) {}

    explicit operator bool() const { return (bool)_fp; }
    bool operator!() const { return !_fp; }

    FILE *get() const { return _fp.get(); }

    void close() { _fp.reset(); }

    /* Arduino File::size() —— 参考工程 fs_settings/fs_time 会用到 */
    size_t size()
    {
        if (!_fp) return 0;
        struct stat st;
        if (fstat(fileno(_fp.get()), &st) != 0) return 0;
        return (size_t)st.st_size;
    }

    bool seek(uint32_t pos)
    {
        if (!_fp) return false;
        return fseek(_fp.get(), (long)pos, SEEK_SET) == 0;
    }

    uint32_t position()
    {
        if (!_fp) return 0;
        long p = ftell(_fp.get());
        return (p < 0) ? 0 : (uint32_t)p;
    }

    int available()
    {
        if (!_fp) return 0;
        struct stat st;
        if (fstat(fileno(_fp.get()), &st) != 0) return 0;
        long cur = ftell(_fp.get());
        if (cur < 0) return 0;
        if (st.st_size <= cur) return 0;
        return (int)(st.st_size - cur);
    }

    int read(uint8_t *buf, size_t len)
    {
        if (!_fp || !buf) return -1;
        size_t n = fread(buf, 1, len, _fp.get());
        return (int)n;
    }

    size_t write(const uint8_t *buf, size_t len)
    {
        if (!_fp || !buf) return 0;
        size_t n = fwrite(buf, 1, len, _fp.get());
        return n;
    }

    String readStringUntil(char terminator)
    {
        String out;
        if (!_fp) return out;
        int c;
        while ((c = fgetc(_fp.get())) >= 0) {
            if ((char)c == terminator) break;
            out += (char)c;
        }
        return out;
    }

private:
    std::shared_ptr<FILE> _fp;
};

/* ─────────────── LittleFS (对应 Arduino LittleFS 全局对象) ─────────────── */
class LittleFSClass {
public:
    /*
     * 参数与 Arduino LittleFS::begin 完全一致 (参考工程 10 处调用都是
     * begin(true, "/littlefs", 10, "littlefs"))。
     * 幂等: 已挂载则直接返回 true —— 参考工程会反复 begin, 而
     * esp_vfs_littlefs_register 对已挂载分区会返回 ESP_ERR_INVALID_STATE。
     */
    bool begin(bool formatOnFail = false, const char *basePath = LFS_VFS_ROOT,
               uint8_t maxOpenFiles = 10, const char *partitionLabel = "littlefs")
    {
        (void)maxOpenFiles;   /* esp_littlefs 无文件数上限 */

        if (!partitionLabel) partitionLabel = "littlefs";
        if (!basePath)       basePath = LFS_VFS_ROOT;

        if (_mounted && strcmp(_label, partitionLabel) == 0) return true;
        /* 也可能是别的实例/别处挂过同一分区 */
        if (esp_littlefs_mounted(partitionLabel)) {
            set_label(partitionLabel);
            _mounted = true;
            return true;
        }

        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path             = basePath;
        conf.partition_label       = partitionLabel;
        conf.format_if_mount_failed = formatOnFail;
        conf.dont_mount            = false;

        esp_err_t ret = esp_vfs_littlefs_register(&conf);
        if (ret != ESP_OK) {
            printf("[LittleFS] 挂载失败 (%s): %s\n",
                   partitionLabel, esp_err_to_name(ret));
            return false;   /* 不置 _mounted, 下次调用会重试 */
        }

        set_label(partitionLabel);
        _mounted = true;
        printf("[LittleFS] 挂载成功: %s → %s\n", partitionLabel, basePath);
        return true;
    }

    /*
     * 参考工程里 begin()/end() 成对出现 (22 处 end), 但这里刻意"保持挂载":
     *   - 避免每次存阅读进度都 lfs_unmount + lfs_mount (省 1~5ms, 无收益)
     *   - 避免 fs_time 的 MJPEG 任务正持句柄时被 unregister 造成 use-after-free
     *   - esp_littlefs 每挂载点自带递归互斥锁, 多任务共享本就安全
     * 空闲时开销为 0 (littlefs 无后台任务/定时器), 常驻约 1.8KB 内部 RAM。
     * 如确需卸载/重新格式化, 直接调 esp_vfs_littlefs_unregister()/esp_littlefs_format()。
     */
    void end() {}

    File open(const char *path, const char *mode = "r")
    {
        if (!path || !mode) return File();
        char full[LFS_VFS_PATH_MAX];
        lfs_graft_path(path, full, sizeof(full));
        return File(fopen(full, mode));
    }

    bool exists(const char *path)
    {
        if (!path) return false;
        char full[LFS_VFS_PATH_MAX];
        lfs_graft_path(path, full, sizeof(full));
        struct stat st;
        return ::stat(full, &st) == 0;
    }

    bool remove(const char *path)
    {
        if (!path) return false;
        char full[LFS_VFS_PATH_MAX];
        lfs_graft_path(path, full, sizeof(full));
        return ::remove(full) == 0;
    }

    /* Arduino LittleFS 的空间查询 (参考工程 fs_settings 写壁纸前做空间检查) */
    size_t totalBytes()
    {
        size_t total = 0, used = 0;
        if (esp_littlefs_info(_label, &total, &used) != ESP_OK) return 0;
        return total;
    }

    size_t usedBytes()
    {
        size_t total = 0, used = 0;
        if (esp_littlefs_info(_label, &total, &used) != ESP_OK) return 0;
        return used;
    }

private:
    void set_label(const char *label)
    {
        strncpy(_label, label, sizeof(_label) - 1);
        _label[sizeof(_label) - 1] = '\0';
    }

    bool _mounted = false;
    char _label[17] = {0};   /* esp_littlefs 内部按 17 字节比较 label */
};

extern LittleFSClass LittleFS;

