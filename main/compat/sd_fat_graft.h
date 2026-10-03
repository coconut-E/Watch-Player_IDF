/*
 * Arduino SdFat → ESP-IDF VFS (FATFS on SDMMC) 嫁接层
 *
 * 目的: 让参考工程 (Arduino/SdFat 2.x) 的源码原样编译,
 *       只把底层文件系统换成已挂载在 /sdcard 的 ESP-IDF FATFS VFS。
 *
 * 映射关系:
 *   SdFs / SdFat      → SdFs  (typedef)
 *   FsFile / SdFile   → FsFile (typedef, 内部持有一个 fd)
 *   sd.open(path,flg) → ::open("/sdcard" + path, flg, 0666)
 *   O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND
 *                     → 直接用 newlib <fcntl.h> 的常量, 原样透传给 ::open()
 *
 * 注意: 这里刻意不自己定义 O_RDONLY 等宏 (newlib 已定义, 重复定义会冲突),
 *       只补 SdFat 专有、newlib 没有的 O_READ / O_AT_END。
 */

#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arduino_compat.h"   /* String */

/* ─────────────── SdFat 专有标志 ─────────────── */
#ifndef O_READ
#define O_READ   O_RDONLY
#endif
#ifndef O_AT_END
#define O_AT_END O_APPEND
#endif

/* SD 卡 VFS 挂载点 (与 drivers/sd_card.h 的 SD_CARD_MOUNT_POINT 一致) */
#define SD_VFS_ROOT "/sdcard"

/* 参考工程里最长路径: /图片/xxx + 256 字节文件名, 留足余量 */
#define SD_VFS_PATH_MAX 384

/* 把 Arduino 风格绝对路径 "/xxx/yyy" 嫁接成 "/sdcard/xxx/yyy" */
inline void sd_graft_path(const char *path, char *out, size_t out_len)
{
    if (!path) {
        out[0] = '\0';
        return;
    }
    if (path[0] == '/') {
        snprintf(out, out_len, SD_VFS_ROOT "%s", path);
    } else {
        snprintf(out, out_len, SD_VFS_ROOT "/%s", path);
    }
}

/* ─────────────── FsFile (对应 SdFat 的 FsFile / SdFile) ─────────────── */
class FsFile {
public:
    FsFile() : _fd(-1), _dir(nullptr), _is_dir_handle(false), _entry_is_dir(false)
    {
        _name[0] = '\0';
        _path[0] = '\0';
    }

    bool open(const char *path, int mode = O_RDONLY)
    {
        close();
        if (!path) return false;

        sd_graft_path(path, _path, sizeof(_path));
        _fd = ::open(_path, mode, 0666);

        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        strncpy(_name, base, sizeof(_name) - 1);
        _name[sizeof(_name) - 1] = '\0';

        if (_fd < 0) {
            /* 目录: FatFs 不一定允许 ::open(), 用 stat 识别后标记成目录句柄,
             * 真正的遍历由 openNext() 里的 opendir(_path) 完成。 */
            struct stat st;
            if (::stat(_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                _is_dir_handle = true;
                return true;
            }
            return false;
        }
        return true;
    }

    bool close()
    {
        if (_dir) {
            closedir(_dir);
            _dir = nullptr;
        }
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
        _is_dir_handle = false;
        _entry_is_dir = false;
        return true;
    }

    bool isOpen() const { return (_fd >= 0) || _is_dir_handle; }
    explicit operator bool() const { return isOpen(); }

    const char *getName() { return _name; }

    /* SdFat: void getName(char* name, size_t size) —— 调用方自己给缓冲 */
    void getName(char *name, size_t size)
    {
        if (!name || size == 0) return;
        size_t n = strlen(_name);
        if (n > size - 1) n = size - 1;
        memcpy(name, _name, n);
        name[n] = '\0';
    }

    /* VFS 取不到 FAT 的隐藏/系统属性, 只能按 SdFat 对长文件名的规则判断 */
    bool isHidden() { return _name[0] == '.'; }

    /* SdFat: 遍历目录, 每次取下一个目录项 (含 "." 与 "..", 由调用方过滤) */
    bool openNext(FsFile *dir, int mode = O_RDONLY)
    {
        close();
        if (!dir) return false;

        if (!dir->_dir) {
            dir->_dir = opendir(dir->_path);
            if (!dir->_dir) return false;
        }

        struct dirent *ent;
        while ((ent = readdir(dir->_dir)) != nullptr) {
            strncpy(_name, ent->d_name, sizeof(_name) - 1);
            _name[sizeof(_name) - 1] = '\0';

            /* 手写拼接 (不用 snprintf), 避免 -Wformat-truncation */
            size_t plen = strlen(dir->_path);
            if (plen >= sizeof(_path) - 1) plen = sizeof(_path) - 2;
            memcpy(_path, dir->_path, plen);
            if (plen == 0 || _path[plen - 1] != '/') _path[plen++] = '/';
            strncpy(_path + plen, _name, sizeof(_path) - plen - 1);
            _path[sizeof(_path) - 1] = '\0';

            struct stat st;
            _entry_is_dir = (::stat(_path, &st) == 0) && S_ISDIR(st.st_mode);
            if (!_entry_is_dir) {
                _fd = ::open(_path, mode, 0666);
            }
            return true;
        }
        return false;
    }

    bool isDirectory()
    {
        if (_fd < 0) return _is_dir_handle || _entry_is_dir;
        struct stat st;
        if (fstat(_fd, &st) != 0) return false;
        return S_ISDIR(st.st_mode);
    }

    /* ── 读 ── */
    int read(void *buf, size_t len)
    {
        if (_fd < 0 || !buf) return -1;
        ssize_t n = ::read(_fd, buf, len);
        return (int)n;
    }
    int read(uint8_t *buf, size_t len) { return read((void *)buf, len); }

    int available()
    {
        if (_fd < 0) return 0;
        struct stat st;
        if (fstat(_fd, &st) != 0) return 0;
        off_t cur = lseek(_fd, 0, SEEK_CUR);
        if (cur < 0) return 0;
        if (st.st_size <= cur) return 0;
        return (int)(st.st_size - cur);
    }

    /* ── 写 ── */
    size_t write(const void *buf, size_t len)
    {
        if (_fd < 0 || !buf) return 0;
        ssize_t n = ::write(_fd, buf, len);
        return (n > 0) ? (size_t)n : 0;
    }
    size_t write(const uint8_t *buf, size_t len) { return write((const void *)buf, len); }

    size_t print(const char *s) { return s ? write(s, strlen(s)) : 0; }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t print(int v)
    {
        char b[24];
        int n = snprintf(b, sizeof(b), "%d", v);
        return (n > 0) ? write(b, (size_t)n) : 0;
    }
    size_t println(const char *s) { return print(s) + write("\n", 1); }
    size_t println(const String &s) { return println(s.c_str()); }
    size_t println() { return write("\n", 1); }

    /* ── 定位 ── */
    bool seekSet(uint64_t pos) { return (_fd >= 0) && (lseek(_fd, (off_t)pos, SEEK_SET) >= 0); }
    bool seek(uint64_t pos) { return seekSet(pos); }
    bool seekCur(int64_t off) { return (_fd >= 0) && (lseek(_fd, (off_t)off, SEEK_CUR) >= 0); }
    bool seekEnd(int64_t off) { return (_fd >= 0) && (lseek(_fd, (off_t)off, SEEK_END) >= 0); }

    uint64_t position()
    {
        if (_fd < 0) return 0;
        off_t cur = lseek(_fd, 0, SEEK_CUR);
        return (cur < 0) ? 0 : (uint64_t)cur;
    }
    /* SdFat 的旧名 (AudioFileSourceSdFat.cpp 用) */
    uint64_t curPosition() { return position(); }

    uint64_t size()
    {
        if (_fd < 0) return 0;
        struct stat st;
        if (fstat(_fd, &st) != 0) return 0;
        return (uint64_t)st.st_size;
    }
    size_t fileSize() { return (size_t)size(); }

    /* ── 行读写 ── */
    int readByte()
    {
        if (_fd < 0) return -1;
        uint8_t b;
        return (::read(_fd, &b, 1) == 1) ? (int)b : -1;
    }

    char *fgets(char *str, int len)
    {
        if (!str || len <= 1 || _fd < 0) return nullptr;
        int i = 0;
        while (i < len - 1) {
            int c = readByte();
            if (c < 0) break;
            str[i++] = (char)c;
            if (c == '\n') break;
        }
        if (i == 0) return nullptr;
        str[i] = '\0';
        return str;
    }

    String readStringUntil(char terminator)
    {
        String out;
        if (_fd < 0) return out;
        int c;
        while ((c = readByte()) >= 0) {
            if ((char)c == terminator) break;
            out += (char)c;
        }
        return out;
    }

private:
    int   _fd;
    DIR  *_dir;              /* 仅目录句柄使用: openNext() 的迭代状态 */
    bool  _is_dir_handle;    /* 本对象是"目录句柄" (open() 于目录) */
    bool  _entry_is_dir;     /* 本对象是 openNext() 取出的目录项且为目录 */
    char  _name[256];        /* 当前文件名 (参考工程用 256 字节缓冲取长中文名) */
    char  _path[SD_VFS_PATH_MAX]; /* 真实 VFS 路径 (目录遍历需要) */
};

/* SdFat 2.x 里 SdFile 是 FsFile 的旧名 */
typedef FsFile SdFile;

/* ─────────────── SdFs / SdFat (卷) ─────────────── */
class SdFs {
public:
    /* 参考工程用 sd.card()->errorCode() 判断 SD 卡是否可用。
     * 本移植 SDMMC 已由 sd_task 挂载在 /sdcard, 所以这里用"挂载点能否 stat"
     * 作为等价判断, 并返回一个恒为"无错误"的假 card 句柄。 */
    struct SdCardStub {
        int errorCode() { return 0; }
    };

    SdCardStub *card()
    {
        struct stat st;
        if (::stat(SD_VFS_ROOT, &st) == 0) return &_stub;
        return nullptr;
    }

    FsFile open(const char *path, int mode = O_RDONLY)
    {
        FsFile f;
        f.open(path, mode);
        return f;
    }

    bool exists(const char *path)
    {
        char full[SD_VFS_PATH_MAX];
        sd_graft_path(path, full, sizeof(full));
        struct stat st;
        return ::stat(full, &st) == 0;
    }

    bool remove(const char *path)
    {
        char full[SD_VFS_PATH_MAX];
        sd_graft_path(path, full, sizeof(full));
        return ::remove(full) == 0;
    }

    bool mkdir(const char *path)
    {
        char full[SD_VFS_PATH_MAX];
        sd_graft_path(path, full, sizeof(full));
        return ::mkdir(full, 0777) == 0;
    }

    bool rmdir(const char *path)
    {
        char full[SD_VFS_PATH_MAX];
        sd_graft_path(path, full, sizeof(full));
        return ::rmdir(full) == 0;
    }

    /* 供外部做底层操作时取真实路径 */
    void getVfsPath(const char *path, char *out, size_t out_len)
    {
        sd_graft_path(path, out, out_len);
    }

private:
    SdCardStub _stub;
};

typedef SdFs SdFat;

extern SdFs sd;
