#include <Arduino.h>
#include <TFT_eSPI.h>

#pragma once

struct imgParam {
    bool hasRed;
    uint8_t dataType;
    bool dither;
    bool grayLut = false;
    uint8_t bpp = 8;
    uint8_t rotate = 0;

    char segments[12];
    uint16_t symbols;
    bool invert;
};

void spr2buffer(TFT_eSprite &spr, String &fileout, imgParam &imageParams);
bool jpg2buffer(String filein, String fileout, imgParam &imageParams);
