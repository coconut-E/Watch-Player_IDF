// AudioFileSourceSdFat.h
#ifndef _AUDIOFILESOURCESDFAT_H
#define _AUDIOFILESOURCESDFAT_H

#include <Arduino.h>
#include <SdFat.h>
#include "AudioFileSource.h"

extern SdFs sd;

class AudioFileSourceSdFat : public AudioFileSource
{
public:
    AudioFileSourceSdFat();
    AudioFileSourceSdFat(const char *filename);
    virtual ~AudioFileSourceSdFat() override;

    virtual bool open(const char *filename) override;
    virtual uint32_t read(void *data, uint32_t len) override;
    virtual bool seek(int32_t pos, int dir) override;
    virtual bool close() override;
    virtual bool isOpen() override;
    virtual uint32_t getSize() override;
    virtual uint32_t getPos() override;

private:
    FsFile f;  
};

#endif