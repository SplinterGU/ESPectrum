///////////////////////////////////////////////////////////////////////////////
//
// ZX-ESPectrum-IDF - Sinclair ZX Spectrum emulator for ESP32 / IDF
//
// VIDEO EMULATION
//
// Copyright (c) 2023 Víctor Iborra [EremusOne]
// https://github.com/EremusOne/ZX-ESPectrum-IDF
//
// Based on ZX-ESPectrum-Wiimote
// Copyright (c) 2020, 2021 David Crespo [dcrespo3d]
// https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote
//
// Based on previous work by Ramón Martinez, Jorge Fuertes and many others
// https://github.com/rampa069/ZX-ESPectrum
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//

#include "Video.h"
#include "CPU.h"
#include "MemESP.h"
#include "Config.h"
#include "OSDMain.h"
#include "hardconfig.h"
#include "hardpins.h"

VGA6Bit VIDEO::vga;
uint8_t VIDEO::borderColor = 0;
unsigned int VIDEO::lastBorder[312] = { 0 };
uint8_t VIDEO::flashing = 0;
uint8_t VIDEO::flash_ctr= 0;
bool VIDEO::OSD = false;
uint8_t VIDEO::tStatesPerLine;
int VIDEO::tStatesScreen;


uint8_t (*VIDEO::getFloatBusData)() = &VIDEO::getFloatBusData48;

#ifdef NOVIDEO
void (*VIDEO::Draw)(unsigned int) = &VIDEO::NoVideo;
#else
void (*VIDEO::Draw)(unsigned int) = &VIDEO::Blank;
#endif

void (*VIDEO::DrawOSD43)(unsigned int) = &VIDEO::BottomBorder;
void (*VIDEO::DrawOSD169)(unsigned int) = &VIDEO::MainScreen;

void precalcColors() {
    for (int i = 0; i < NUM_SPECTRUM_COLORS; i++) {
        spectrum_colors[i] = (spectrum_colors[i] & VIDEO::vga.RGBAXMask) | VIDEO::vga.SBits;
    }
}

void precalcAluBytes() {

    uint16_t specfast_colors[128]; // Array for faster color calc in Draw

    unsigned int pal[2],b0,b1,b2,b3;

    // Calc array for faster color calcs in Draw
    for (int i = 0; i < (NUM_SPECTRUM_COLORS >> 1); i++) {
        // Normal
        specfast_colors[i] = spectrum_colors[i];
        specfast_colors[i << 3] = spectrum_colors[i];
        // Bright
        specfast_colors[i | 0x40] = spectrum_colors[i + (NUM_SPECTRUM_COLORS >> 1)];
        specfast_colors[(i << 3) | 0x40] = spectrum_colors[i + (NUM_SPECTRUM_COLORS >> 1)];
    }

    // Alloc ALUbytes
    AluBytes = new uint32_t*[16];
    for (int i = 0; i < 16; i++) {
        AluBytes[i] = new uint32_t[256];
    }

    for (int i = 0; i < 16; i++) {
        for (int n = 0; n < 256; n++) {
            pal[0] = specfast_colors[n & 0x78];
            pal[1] = specfast_colors[n & 0x47];
            b0 = pal[(i >> 3) & 0x01];
            b1 = pal[(i >> 2) & 0x01];
            b2 = pal[(i >> 1) & 0x01];
            b3 = pal[i & 0x01];
            AluBytes[i][n]=b2 | (b3<<8) | (b0<<16) | (b1<<24);
        }
    }    

}

void deallocAluBytes() {

    // For dealloc
    for (int i = 0; i < 16; i++) {
        delete[] AluBytes[i];
    }
    delete[] AluBytes;

};

uint16_t zxColor(uint8_t color, uint8_t bright) {
    if (bright) color += 8;
    return spectrum_colors[color];
}

// Precalc ULA_SWAP
#define ULA_SWAP(y) ((y & 0xC0) | ((y & 0x38) >> 3) | ((y & 0x07) << 3))
void precalcULASWAP() {
    for (int i = 0; i < SPEC_H; i++) {
        offBmp[i] = ULA_SWAP(i) << 5;
        offAtt[i] = ((i >> 3) << 5) + 0x1800;
    }
}

// Precalc border 32 bits values
static unsigned int border32[8];
void precalcborder32()
{
    for (int i = 0; i < 8; i++) {
        uint8_t border = zxColor(i,0);
        border32[i] = border | (border << 8) | (border << 16) | (border << 24);
    }
}

void VIDEO::Init() {

    const Mode& vgaMode = Config::aspect_16_9 ? vga.MODE360x200 : vga.MODE320x240;
    OSD::scrW = vgaMode.hRes;
    OSD::scrH = vgaMode.vRes / vgaMode.vDiv;
    
    const int redPins[] = {RED_PINS_6B};
    const int grePins[] = {GRE_PINS_6B};
    const int bluPins[] = {BLU_PINS_6B};
    vga.init(vgaMode, redPins, grePins, bluPins, HSYNC_PIN, VSYNC_PIN);

    precalcColors();    // precalculate colors for current VGA mode

    precalcAluBytes(); // Alloc and calc AluBytes

    precalcULASWAP();   // precalculate ULA SWAP values

    precalcborder32();  // Precalc border 32 bits values

    for (int i=0;i<312;i++) lastBorder[i]=8; // 8 -> Force repaint of border
    borderColor = 0;

    is169 = Config::aspect_16_9 ? 1 : 0;

    if (Config::getArch() == "48K") {
        tStatesPerLine = TSTATES_PER_LINE;
        tStatesScreen = is169 ? TS_SCREEN_360x200 : TS_SCREEN_320x240;
    } else {
        tStatesPerLine = TSTATES_PER_LINE_128;
        tStatesScreen = is169 ? TS_SCREEN_360x200_128 : TS_SCREEN_320x240_128;
    }

    #ifdef NOVIDEO
        Draw = &NoVideo;
    #else
        Draw = &Blank;
    #endif

}

void VIDEO::Reset() {

    for (int i=0;i<312;i++) lastBorder[i]=8; // 8 -> Force repaint of border
    borderColor = 7;

    is169 = Config::aspect_16_9 ? 1 : 0;
    
    if (Config::getArch() == "48K") {
        tStatesPerLine = TSTATES_PER_LINE;
        tStatesScreen = is169 ? TS_SCREEN_360x200 : TS_SCREEN_320x240;

    } else {
        tStatesPerLine = TSTATES_PER_LINE_128;
        tStatesScreen = is169 ? TS_SCREEN_360x200_128 : TS_SCREEN_320x240_128;
    }

    #ifdef NOVIDEO
        Draw = &NoVideo;
    #else
        Draw = &Blank;
    #endif

}

uint8_t IRAM_ATTR VIDEO::getFloatBusData48() {

    unsigned int currentTstates = CPU::tstates;

	// each line spans 224 t-states
	unsigned short int line = currentTstates / 224; // int line
    // only the 192 lines between 64 and 255 have graphic data, the rest is border
	if (line < 64 || line >= 256) return 0xFF;

    // only the first 128 t-states of each line correspond to a graphic data transfer
	// the remaining 96 t-states correspond to border
	unsigned char halfpix = currentTstates % 224;
	if (halfpix >= 128) return 0xFF;

    switch (halfpix & 0x07) {
        case 3: { // Bitmap
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int bmpOffset = offBmp[line - 64];
            int hpoffset = (halfpix - 3) >> 2;
            return(grmem[bmpOffset + hpoffset]);
        }
        case 4: { // Attr
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int attOffset = offAtt[line - 64];
            int hpoffset = (halfpix - 3) >> 2;
            return(grmem[attOffset + hpoffset]);
        }
        case 5: { // Bitmap + 1
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int bmpOffset = offBmp[line - 64];
            int hpoffset = ((halfpix - 3) >> 2) + 1;
            return(grmem[bmpOffset + hpoffset]);
        }
        case 6: { // Attr + 1
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int attOffset = offAtt[line - 64];
            int hpoffset = ((halfpix - 3) >> 2) + 1;
            return(grmem[attOffset + hpoffset]);
        }
    }

    return(0xFF);

}

uint8_t IRAM_ATTR VIDEO::getFloatBusData128() {

    unsigned int currentTstates = CPU::tstates;

    currentTstates++;

	// each line spans 224 t-states
	unsigned short int line = currentTstates / 228; // int line
    // only the 192 lines between 64 and 255 have graphic data, the rest is border
	if (line < 63 || line >= 255) return 0xFF;

    // only the first 128 t-states of each line correspond to a graphic data transfer
	// the remaining 96 t-states correspond to border
	unsigned char halfpix = currentTstates % 228;
	if (halfpix >= 128) return 0xFF;

    switch (halfpix & 0x07) {
        case 0: { // Bitmap
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int bmpOffset = offBmp[line - 63];
            int hpoffset = (halfpix) >> 2;
            return(grmem[bmpOffset + hpoffset]);
        }
        case 1: { // Attr
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int attOffset = offAtt[line - 63];
            int hpoffset = (halfpix) >> 2;
            return(grmem[attOffset + hpoffset]);
        }
        case 2: { // Bitmap + 1
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int bmpOffset = offBmp[line - 63];
            int hpoffset = ((halfpix) >> 2) + 1;
            return(grmem[bmpOffset + hpoffset]);
        }
        case 3: { // Attr + 1
            grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
            unsigned int attOffset = offAtt[line - 63];
            int hpoffset = ((halfpix) >> 2) + 1;
            return(grmem[attOffset + hpoffset]);
        }
    }

    return(0xFF);

}

///////////////////////////////////////////////////////////////////////////////
//  VIDEO DRAW FUNCTIONS
///////////////////////////////////////////////////////////////////////////////

void IRAM_ATTR VIDEO::NoVideo(unsigned int statestoadd) {
   CPU::tstates += statestoadd;
}

void IRAM_ATTR VIDEO::TopBorder_Blank(unsigned int statestoadd) {

    CPU::tstates += statestoadd;

    if (CPU::tstates > tstateDraw) {
        video_rest = CPU::tstates - tstateDraw;
        tstateDraw += tStatesPerLine;
        lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
        if (is169) lineptr32 += 5;
        coldraw_cnt = 0;
        Draw = &TopBorder;
        Draw(0);
    }

}

void IRAM_ATTR VIDEO::TopBorder(unsigned int statestoadd) {

    CPU::tstates += statestoadd;

    statestoadd += video_rest;
    video_rest = statestoadd & 0x03; // Mod 4
    brd = border32[borderColor];
    for (int i=0; i < (statestoadd >> 2); i++) {
        *lineptr32++ = brd;
        *lineptr32++ = brd;
        if (++coldraw_cnt == 40) {
            Draw = ++linedraw_cnt == (is169 ? 4 : 24) ? &MainScreen_Blank : &TopBorder_Blank;
            return;
        }
    }

}

void IRAM_ATTR VIDEO::MainScreen_Blank(unsigned int statestoadd) {
    
    CPU::tstates += statestoadd;

    if (CPU::tstates > tstateDraw) {
        video_rest = CPU::tstates - tstateDraw;
        tstateDraw += tStatesPerLine;
        lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
        if (is169) lineptr32 += 5;
        coldraw_cnt = 0;
        bmpOffset = offBmp[linedraw_cnt-(is169 ? 4 : 24)];
        attOffset = offAtt[linedraw_cnt-(is169 ? 4 : 24)];
        grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
        Draw = DrawOSD169;
        Draw(0);
    }

}    

void IRAM_ATTR VIDEO::MainScreen(unsigned int statestoadd) {

    uint8_t att, bmp;

    CPU::tstates += statestoadd;

    statestoadd += video_rest;
    video_rest = statestoadd & 0x03; // Mod 4
    brd = border32[borderColor];

    for (int i=0; i < (statestoadd >> 2); i++) {    
        
        if ((coldraw_cnt>3) && (coldraw_cnt<36)) {
            att = grmem[attOffset++];       // get attribute byte
            if (att & flashing) {
                bmp = ~grmem[bmpOffset++];  // get inverted bitmap byte
            } else 
                bmp = grmem[bmpOffset++];   // get bitmap byte

            *lineptr32++ = AluBytes[bmp >> 4][att];
            *lineptr32++ = AluBytes[bmp & 0xF][att];

        } else {

            *lineptr32++ = brd;
            *lineptr32++ = brd;

        }

        if (++coldraw_cnt == 40) {      
            Draw = ++linedraw_cnt == (is169 ? 196 : 216) ? &BottomBorder_Blank : &MainScreen_Blank;
            return;
        }

    }

}

void IRAM_ATTR VIDEO::MainScreen_OSD(unsigned int statestoadd) {

    uint8_t att, bmp;

    CPU::tstates += statestoadd;

    statestoadd += video_rest;
    video_rest = statestoadd & 0x03; // Mod 4
    brd = border32[borderColor];
    for (int i=0; i < (statestoadd >> 2); i++) {    

        if ((linedraw_cnt>175) && (linedraw_cnt<192) && (coldraw_cnt>20) && (coldraw_cnt<39)) {
            lineptr32+=2;
            attOffset++;
            bmpOffset++;
            coldraw_cnt++;
            continue;
        }

        if ((coldraw_cnt>3) && (coldraw_cnt<36)) {
            att = grmem[attOffset++];       // get attribute byte
            if (att & flashing) {
                bmp = ~grmem[bmpOffset++];  // get inverted bitmap byte
            } else 
                bmp = grmem[bmpOffset++];   // get bitmap byte

            *lineptr32++ = AluBytes[bmp >> 4][att];
            *lineptr32++ = AluBytes[bmp & 0xF][att];
        } else {
            *lineptr32++ = brd;
            *lineptr32++ = brd;
        }

        if (++coldraw_cnt == 40) {
            Draw = ++linedraw_cnt == 196 ? &BottomBorder_Blank : &MainScreen_Blank;
            return;
        }
    }

}

void IRAM_ATTR VIDEO::BottomBorder_Blank(unsigned int statestoadd) {

    CPU::tstates += statestoadd;

    if (CPU::tstates > tstateDraw) {
        video_rest = CPU::tstates - tstateDraw;
        tstateDraw += tStatesPerLine;
        lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
        if (is169) lineptr32 += 5;        
        coldraw_cnt = 0;
        Draw = DrawOSD43;
        Draw(0);
    }

}

void IRAM_ATTR VIDEO::BottomBorder(unsigned int statestoadd) {

    CPU::tstates += statestoadd;

    statestoadd += video_rest;
    video_rest = statestoadd & 0x03; // Mod 4
    brd = border32[borderColor];
    for (int i=0; i < (statestoadd >> 2); i++) {    
        *lineptr32++ = brd;
        *lineptr32++ = brd;
        if (++coldraw_cnt == 40) {
            Draw = ++linedraw_cnt == (is169 ? 200 : 240) ? &Blank : &BottomBorder_Blank ;
            return;
        }
    }
}

void IRAM_ATTR VIDEO::BottomBorder_OSD(unsigned int statestoadd) {

    CPU::tstates += statestoadd;

    statestoadd += video_rest;
    video_rest = statestoadd & 0x03; // Mod 4
    brd = border32[borderColor];
    for (int i=0; i < (statestoadd >> 2); i++) {    
        
        if ((linedraw_cnt<220) || (linedraw_cnt>235)) {
            *lineptr32++ = brd;
            *lineptr32++ = brd;
        } else {
            if ((coldraw_cnt<21) || (coldraw_cnt>38)) {
                *lineptr32++ = brd;
                *lineptr32++ = brd;
            } else lineptr32+=2;
        }
        
        if (++coldraw_cnt == 40) {
            Draw = ++linedraw_cnt == 240 ? &Blank : &BottomBorder_Blank ;
            return;
        }
    }

}

void IRAM_ATTR VIDEO::Blank(unsigned int statestoadd) {

    CPU::tstates += statestoadd;

    if (CPU::tstates < tStatesPerLine) {
        linedraw_cnt = 0;
        tstateDraw = tStatesScreen;
        Draw = &TopBorder_Blank;
    }

}

// ///////////////////////////////////////////////////////////////////////////////
// // Flush -> Flush screen after HALT
// ///////////////////////////////////////////////////////////////////////////////
void VIDEO::Flush() {

    // TO DO: Write faster version: there's no need to use Draw function
    while (CPU::tstates < CPU::statesInFrame) {
        Draw(tStatesPerLine);        
    }

}

///////////////////////////////////////////////////////////////////////////////
// Draw_43 -> Multicolour and Border effects support
// 4:3 (320x240)
///////////////////////////////////////////////////////////////////////////////
// void IRAM_ATTR VIDEO::Draw_43(unsigned int statestoadd) {

// uint8_t att, bmp;

// CPU::tstates += statestoadd;

// #ifndef NO_VIDEO

// if (DrawStatus==TOPBORDER_BLANK) {
//     if (CPU::tstates > tstateDraw) {
//         statestoadd = CPU::tstates - tstateDraw;
//         tstateDraw += TSTATES_PER_LINE;
//         lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
//         coldraw_cnt = 0;
//         DrawStatus = TOPBORDER;
//         video_rest = 0;
//     } else return;
// }

// if (DrawStatus==TOPBORDER) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {
//         *lineptr32++ = brd;
//         *lineptr32++ = brd;
//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 24 ? MAINSCREEN_BLANK : TOPBORDER_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==MAINSCREEN_BLANK) {
//     if (CPU::tstates > tstateDraw) {
//         statestoadd = CPU::tstates - tstateDraw;
//         tstateDraw += TSTATES_PER_LINE;
//         lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
//         coldraw_cnt = 0;
//         video_rest = 0;
//         DrawStatus = LINEDRAW;
//         bmpOffset = offBmp[linedraw_cnt-24];
//         attOffset = offAtt[linedraw_cnt-24];
//         grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
//     } else return;
// }    

// if (DrawStatus==LINEDRAW) {

//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];

//     for (int i=0; i < (statestoadd >> 2); i++) {    
        
//         if ((coldraw_cnt>3) && (coldraw_cnt<36)) {
//             att = grmem[attOffset++];       // get attribute byte
//             if (att & flashing) {
//                 bmp = ~grmem[bmpOffset++];  // get inverted bitmap byte
//             } else 
//                 bmp = grmem[bmpOffset++];   // get bitmap byte

//             *lineptr32++ = AluBytes[bmp >> 4][att];
//             *lineptr32++ = AluBytes[bmp & 0xF][att];

//         } else {

//             *lineptr32++ = brd;
//             *lineptr32++ = brd;

//         }

//         if (++coldraw_cnt == 40) {      
//             DrawStatus = ++linedraw_cnt == 216 ? BOTTOMBORDER_BLANK : MAINSCREEN_BLANK;
//             return;
//         }

//     }
//     return;
// }

// if (DrawStatus==BOTTOMBORDER_BLANK) {
//     if (CPU::tstates > tstateDraw) {
//         statestoadd = CPU::tstates - tstateDraw;
//         tstateDraw += TSTATES_PER_LINE;
//         lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
//         coldraw_cnt = 0;
//         video_rest = 0;
//         DrawStatus = BottomDraw;
//     } else return;
// }

// if (DrawStatus==BOTTOMBORDER) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {    
//         *lineptr32++ = brd;
//         *lineptr32++ = brd;
//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 240 ? BLANK : BOTTOMBORDER_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==BOTTOMBORDER_FPS) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {    
        
//         if ((linedraw_cnt<220) || (linedraw_cnt>235)) {
//             *lineptr32++ = brd;
//             *lineptr32++ = brd;
//         } else {
//             if ((coldraw_cnt<21) || (coldraw_cnt>38)) {
//                 *lineptr32++ = brd;
//                 *lineptr32++ = brd;
//             } else lineptr32+=2;
//         }
        
//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 240 ? BLANK : BOTTOMBORDER_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==BLANK) {
//     if (CPU::tstates < TSTATES_PER_LINE) {
//         linedraw_cnt = 0;
//         tstateDraw = TS_SCREEN_320x240;
//         DrawStatus = TOPBORDER_BLANK;
//     }
// }

// #endif

// }

// ///////////////////////////////////////////////////////////////////////////////
// // Draw_169 -> Multicolour and Border effects support
// // 16:9 (360x200)
// ///////////////////////////////////////////////////////////////////////////////
// void IRAM_ATTR VIDEO::Draw_169(unsigned int statestoadd) {

// uint8_t att, bmp;

// CPU::tstates += statestoadd;

// #ifndef NO_VIDEO

// if (DrawStatus==TOPBORDER_BLANK) {
//     if (CPU::tstates > tstateDraw) {
//         statestoadd = CPU::tstates - tstateDraw;
//         tstateDraw += TSTATES_PER_LINE;
//         lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
//         lineptr32 += 5; // Offset for 360x200
//         coldraw_cnt = 0;
//         DrawStatus = TOPBORDER;
//         video_rest = 0;
//     } else return;
// }

// if (DrawStatus==TOPBORDER) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {
//         *lineptr32++ = brd;
//         *lineptr32++ = brd;
//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 4 ? MAINSCREEN_BLANK : TOPBORDER_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==MAINSCREEN_BLANK) {
//     if (CPU::tstates > tstateDraw) {
//         statestoadd = CPU::tstates - tstateDraw;
//         tstateDraw += TSTATES_PER_LINE;
//         lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
//         lineptr32 += 5;
//         coldraw_cnt = 0;
//         video_rest = 0;
//         DrawStatus = LineDraw;
//         bmpOffset = offBmp[linedraw_cnt-4];
//         attOffset = offAtt[linedraw_cnt-4];
//         grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;
//     } else return;
// }    

// if (DrawStatus==LINEDRAW) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {    
        
//         if ((coldraw_cnt>3) && (coldraw_cnt<36)) {
//             att = grmem[attOffset++];       // get attribute byte
//             if (att & flashing) {
//                 bmp = ~grmem[bmpOffset++];  // get inverted bitmap byte
//             } else 
//                 bmp = grmem[bmpOffset++];   // get bitmap byte

//             *lineptr32++ = AluBytes[bmp >> 4][att];
//             *lineptr32++ = AluBytes[bmp & 0xF][att];
//         } else {
//             *lineptr32++ = brd;
//             *lineptr32++ = brd;
//         }

//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 196 ? BOTTOMBORDER_BLANK : MAINSCREEN_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==LINEDRAW_FPS) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {    

//         if ((linedraw_cnt>175) && (linedraw_cnt<192) && (coldraw_cnt>20) && (coldraw_cnt<39)) {
//             lineptr32+=2;
//             attOffset++;
//             bmpOffset++;
//             coldraw_cnt++;
//             continue;
//         }

//         if ((coldraw_cnt>3) && (coldraw_cnt<36)) {
//             att = grmem[attOffset++];       // get attribute byte
//             if (att & flashing) {
//                 bmp = ~grmem[bmpOffset++];  // get inverted bitmap byte
//             } else 
//                 bmp = grmem[bmpOffset++];   // get bitmap byte

//             *lineptr32++ = AluBytes[bmp >> 4][att];
//             *lineptr32++ = AluBytes[bmp & 0xF][att];
//         } else {
//             *lineptr32++ = brd;
//             *lineptr32++ = brd;
//         }

//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 196 ? BOTTOMBORDER_BLANK : MAINSCREEN_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==BOTTOMBORDER_BLANK) {
//     if (CPU::tstates > tstateDraw) {
//         statestoadd = CPU::tstates - tstateDraw;
//         tstateDraw += TSTATES_PER_LINE;
//         lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);
//         lineptr32 += 5;
//         coldraw_cnt = 0;
//         video_rest = 0;
//         DrawStatus = BOTTOMBORDER;
//     } else return;
// }

// if (DrawStatus==BOTTOMBORDER) {
//     statestoadd += video_rest;
//     video_rest = statestoadd & 0x03; // Mod 4
//     brd = border32[borderColor];
//     for (int i=0; i < (statestoadd >> 2); i++) {    
//         *lineptr32++ = brd;
//         *lineptr32++ = brd;
//         if (++coldraw_cnt == 40) {
//             DrawStatus = ++linedraw_cnt == 200 ? BLANK : BOTTOMBORDER_BLANK;
//             return;
//         }
//     }
//     return;
// }

// if (DrawStatus==BLANK) {
//     if (CPU::tstates < TSTATES_PER_LINE) {
//         linedraw_cnt = 0;
//         tstateDraw = TS_SCREEN_360x200;
//         DrawStatus = TOPBORDER_BLANK;
//     }
// }

// #endif

// }

// ///////////////////////////////////////////////////////////////////////////////
// // TO DO: Draw_43_fast -> Fast draw (no multicolour, border effects support)
// ///////////////////////////////////////////////////////////////////////////////
// static void IRAM_ATTR Draw_43_fast(unsigned int statestoadd) {

// uint8_t att, bmp;

//     CPU::tstates += statestoadd;

// #ifndef NO_VIDEO

//     if (DrawStatus==TOPBORDER) {
//         if (CPU::tstates > tstateDraw) {
            
//             // Has border changed?
//             if (lastBorder[0] != borderColor) {

//                 // Draw border
//                 brd = border32[borderColor];
//                 for (int n=0; n < 24; n++) {
//                     memset((unsigned char *)vga.backBuffer[n],brd,320);
//                 }
//                 for (int n=24; n < 216; n++) {
//                     uint8_t *lineptr = (uint8_t *)(vga.backBuffer[n]);
//                     memset(lineptr,brd,32);
//                     memset(lineptr + 288,brd,32);
//                 }
//                 for (int n=216; n < 240; n++) {
//                     memset((unsigned char *)vga.backBuffer[n],brd,320);
//                 }
//                 lastBorder[0]=borderColor;

//             }
 
//             // Draw mainscreen

//             grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;

//             for (int n=24; n < 216; n++) {

//                 lineptr32 = (uint32_t *)(vga.backBuffer[n]);
//                 lineptr32 += 8;

//                 bmpOffset = offBmp[n - 24];
//                 attOffset = offAtt[n - 24];

//                 for (int i=0; i < 32; i++) {
//                         att = grmem[attOffset++];       // get attribute byte
//                         if (att & flashing) {
//                             bmp = ~grmem[bmpOffset++];  // get inverted bitmap byte
//                         } else 
//                             bmp = grmem[bmpOffset++];   // get bitmap byte
//                         *lineptr32++ = AluBytes[bmp >> 4][att];
//                         *lineptr32++ = AluBytes[bmp & 0xF][att];
//                 }

//             }
 
//             DrawStatus = BLANK;
//         }
//         return;
//     }

//     if (DrawStatus==BLANK)
//         if (CPU::tstates < TSTATES_PER_LINE) {
//                 DrawStatus = TOPBORDER;
//                 tstateDraw = TS_PHASE_1_320x240;
//         }

// #endif

// }

///////////////////////////////////////////////////////////////////////////////
// Draw_169_fast -> Fast Border 16:9
///////////////////////////////////////////////////////////////////////////////
// void IRAM_ATTR VIDEO::Draw_169_fast(unsigned int statestoadd) {

// uint8_t att, bmp;

//     CPU::tstates += statestoadd;

// #ifndef NO_VIDEO

//     if (DrawStatus==TOPBORDER) {

//         if (CPU::tstates > tstateDraw) {

//             tstateDraw += TSTATES_PER_LINE;

//             brd = borderColor;
//             if (lastBorder[linedraw_cnt] != brd) {

//                 lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);

//                 for (int i=0; i < 90 ; i++) {
//                     *lineptr32++ = border32[brd];
//                 }

//                 lastBorder[linedraw_cnt]=brd;
//             }

//             linedraw_cnt++;

//             if (linedraw_cnt == 4) {
//                 DrawStatus = LEFTBORDER;
//                 tstateDraw = TS_PHASE_2_360x200;
//             }

//         }

//         return;

//     }

//     if (DrawStatus==LEFTBORDER) {
     
//         if (CPU::tstates > tstateDraw) {

//             lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);                

//             brd = borderColor;
//             if (lastBorder[linedraw_cnt] != brd) {
//                 for (int i=0; i < 13; i++) {    
//                     *lineptr32++ = border32[brd];
//                 }
//             } else lineptr32 += 13;

//             tstateDraw += 24;

//             DrawStatus = LINEDRAW_SYNC;

//         }
        
//         return;

//     }    

//     if (DrawStatus == LINEDRAW_SYNC) {

//         if (CPU::tstates > tstateDraw) {

//             statestoadd = CPU::tstates - tstateDraw;

//             bmpOffset = offBmp[mainscrline_cnt];
//             attOffset = offAtt[mainscrline_cnt];

//             grmem = MemESP::videoLatch ? MemESP::ram7 : MemESP::ram5;

//             coldraw_cnt = 0;
//             video_rest = 0;
//             DrawStatus = LineDraw;

//         } else return;

//     }

//     if (DrawStatus == LINEDRAW) {
   
//         statestoadd += video_rest;
//         video_rest = statestoadd & 0x03; // Mod 4

//         for (int i=0; i < (statestoadd >> 2); i++) {    

//             att = grmem[attOffset];  // get attribute byte

//             if (att & flashing) {
//                 bmp = ~grmem[bmpOffset];  // get inverted bitmap byte
//             } else 
//                 bmp = grmem[bmpOffset];   // get bitmap byte

//             *lineptr32++ = AluBytes[bmp >> 4][att];
//             *lineptr32++ = AluBytes[bmp & 0xF][att];

//             attOffset++;
//             bmpOffset++;

//             coldraw_cnt++;

//             if (coldraw_cnt == 32) {
//                 DrawStatus = RIGHTBORDER;
//                 return;
//             }

//         }

//         return;

//     }

//     if (DrawStatus == LINEDRAW_FPS) {
   
//         statestoadd += video_rest;
//         video_rest = statestoadd & 0x03; // Mod 4

//         for (int i=0; i < (statestoadd >> 2); i++) {    

//             if ((linedraw_cnt>175) && (linedraw_cnt<192) && (coldraw_cnt>18)) {
//                 lineptr32+=2;
//                 attOffset++;
//                 bmpOffset++;
//                 coldraw_cnt++;
//                 if (coldraw_cnt == 32) {
//                     DrawStatus = RIGHTBORDER_FPS;
//                     return;
//                 }
//                 continue;
//             }

//             att = grmem[attOffset];  // get attribute byte

//             if (att & flashing) {
//                 bmp = ~grmem[bmpOffset];  // get inverted bitmap byte
//             } else 
//                 bmp = grmem[bmpOffset];   // get bitmap byte

//             *lineptr32++ = AluBytes[bmp >> 4][att];
//             *lineptr32++ = AluBytes[bmp & 0xF][att];

//             attOffset++;
//             bmpOffset++;

//             coldraw_cnt++;

//             if (coldraw_cnt == 32) {
//                 DrawStatus = RIGHTBORDER;
//                 return;
//             }

//         }

//         return;

//     }

//     if (DrawStatus==RIGHTBORDER) {
     
//         if (lastBorder[linedraw_cnt] != brd) {
//             for (int i=0; i < 13; i++) {
//                 *lineptr32++ = border32[brd];
//             }
//             lastBorder[linedraw_cnt]=brd;
//         }
//         mainscrline_cnt++;
//         linedraw_cnt++;
//         if (mainscrline_cnt < 192) {
//             DrawStatus = LEFTBORDER;
//             tstateDraw += 200;
//         } else {
//             mainscrline_cnt = 0;
//             DrawStatus = BOTTOMBORDER;
//             tstateDraw = TS_PHASE_3_360x200;
//         }
//         return;
//     }    

//     if (DrawStatus==RIGHTBORDER_FPS) {
     
//         if (lastBorder[linedraw_cnt] != brd) {

//             if ((linedraw_cnt>175) && (linedraw_cnt<192)) {
//                 lineptr32+=10;
//                 for (int i=0; i < 3; i++) {
//                     *lineptr32++ = border32[brd];
//                 }
//             } else {
//                 for (int i=0; i < 13; i++) {
//                     *lineptr32++ = border32[brd];
//                 }
//             }
//             lastBorder[linedraw_cnt]=brd;

//         }

//         mainscrline_cnt++;
//         linedraw_cnt++;

//         if (mainscrline_cnt < 192) {
//             DrawStatus = LEFTBORDER;
//             tstateDraw += 200;
//         } else {
//             mainscrline_cnt = 0;
//             DrawStatus = BOTTOMBORDER;
//             tstateDraw = TS_PHASE_3_360x200;
//         }

//         return;

//     }    

//     if (DrawStatus==BOTTOMBORDER) {

//         if (CPU::tstates > tstateDraw) {

//             tstateDraw += TSTATES_PER_LINE;

//             brd = borderColor;
//             if (lastBorder[linedraw_cnt] != brd) {

//                 lineptr32 = (uint32_t *)(vga.backBuffer[linedraw_cnt]);

//                 for (int i=0; i < 90; i++) {    
//                     *lineptr32++ = border32[brd];
//                 }

//                 lastBorder[linedraw_cnt]=brd;            
//             }

//             linedraw_cnt++;
//             if (linedraw_cnt == 200) {
//                 linedraw_cnt=0;
//                 DrawStatus = BLANK;
//             }

//         }

//         return;

//     }

//     if (DrawStatus==BLANK)
//         if (CPU::tstates < TSTATES_PER_LINE) {
//                 DrawStatus = TOPBORDER;
//                 linedraw_cnt=0;
//                 tstateDraw = TS_PHASE_1_360x200;
//         }

// #endif

// }
