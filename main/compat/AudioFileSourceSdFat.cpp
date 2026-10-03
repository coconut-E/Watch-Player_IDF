#include "AudioFileSourceSdFat.h"

AudioFileSourceSdFat::AudioFileSourceSdFat()
{
}

AudioFileSourceSdFat::AudioFileSourceSdFat(const char *filename)
{
    open(filename);
}

bool AudioFileSourceSdFat::open(const char *filename)
{
    f = sd.open(filename, O_RDONLY);
    return f.isOpen();
}

AudioFileSourceSdFat::~AudioFileSourceSdFat()
{
    if (f.isOpen()) f.close();
}

uint32_t AudioFileSourceSdFat::read(void *data, uint32_t len)
{
    return f.read(data, len);
}

bool AudioFileSourceSdFat::seek(int32_t pos, int dir)
{
    if (!f.isOpen()) return false;
    switch (dir) {
        case SEEK_SET:
            return f.seek(pos);
        case SEEK_CUR:
            return f.seekCur(pos);
        case SEEK_END:
            return f.seekEnd(pos);
        default:
            return false;
    }
}

bool AudioFileSourceSdFat::close()
{
    f.close();
    return true;
}

bool AudioFileSourceSdFat::isOpen()
{
    return f.isOpen();
}

uint32_t AudioFileSourceSdFat::getSize()
{
    if (!f.isOpen()) return 0;
    return f.fileSize(); 
}

uint32_t AudioFileSourceSdFat::getPos()
{
    if (!f.isOpen()) return 0;
    return f.curPosition(); 
}