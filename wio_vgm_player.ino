/*
MIT License

Copyright (c) 2021 Alexey Dynda

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include "Adafruit_ZeroDMA.h"
#include "vgm_file.h"
#include "vampire_killer.h"
#include "bucky_ohare.h"
#include "contra.h"
#include "lcdgfx.h"
#include "lcd_backlight.hpp"
//#include "Seeed_FS.h" //Including SD card library

#include <string.h>
#include <stdio.h>

typedef enum
{
    MODE_PLAY,
    MODE_BROWSE,
} PlayerMode;

PlayerMode playerMode = MODE_BROWSE;

typedef struct
{
    char name[32];
    const uint8_t *data;
    int dataLen;
    int track;  // Some nsf files have several tracks inside
} TrackInfo;

/**
 * Available tracks
 */
static const TrackInfo tracks[] =
{
    { "Vampire Killer", vampire_killer_vgm, vampire_killer_vgm_len, 0 },
    { "Bucky Ohare 1", bucky_ohare_nsf, bucky_ohare_nsf_len, 0 },
    { "Bucky Ohare 2", bucky_ohare_nsf, bucky_ohare_nsf_len, 1 },
    { "Bucky Ohare 3", bucky_ohare_nsf, bucky_ohare_nsf_len, 2 },
    { "Bucky Ohare 4", bucky_ohare_nsf, bucky_ohare_nsf_len, 3 },
    { "Contra 1", contra_nsf, contra_nsf_len, 0 },
    { "Contra 2", contra_nsf, contra_nsf_len, 1 },
    { "Contra 3", contra_nsf, contra_nsf_len, 2 },
    { "Contra 4", contra_nsf, contra_nsf_len, 3 },
};

/**
 Create our Vgm decoder engine. It is used to decode Vgm or Nsf data.
 Vgm format is widely used as storage format for different platforms: MSX/MSX2, NES.
 Nsf format is valid only for NES platform. The decoder supports both.
 */
static VgmFile vgm;
static uint16_t volume = 110;
static bool updateVolume = false;
static bool skipTrack = false;
static int trackIndex = 0;

DisplayWioTerminal_320x240x16 display;
#define LCD_BACKLIGHT (72Ul) // Control Pin of LCD
static LCDBackLight backLight;

void openTrack();
void updatePlayingStatus(bool updateTrackName, bool updateTrackVolume);

// =========================================== DMA ===================================

/**
 Adafruit DMA engine will be used to transfer decoded PCM-data to
 our PWM peripheral timer. On WIO platform built-in buzzer is connected
 to digital pin 12 (PD11), which cannot be connected to DAC hardware module.
 So, we're going to use PWM as DAC analog.
 */
Adafruit_ZeroDMA pwmDMA;
DmacDescriptor *dmac_descriptor_1;
DmacDescriptor *dmac_descriptor_2;

/**
 We need 2 buffers. One of the buffers will always be in use by DMA, while
 the second buffer is free, and our Vgm decoder will put data to it.
 */
static uint8_t buffer_tx[2][8192] = {};
uint8_t active_buffer = 0;
uint8_t buffer_to_fill = 0;

/**
 Each time DMA completes transfer for one buffer, it will switch to the second one.
 Here we remember, which one is active at every mo
nent.
 */
void dma_callback(Adafruit_ZeroDMA *dma)
{
    active_buffer = !active_buffer;
}

void dmaInit()
{
    /** Allocate DMA channel */
    pwmDMA.allocate();
    /** Connect DMA channel to TCC0 timer as source for data requests */
    pwmDMA.setTrigger(0x16); // 0x16 = TCC0 - refer to SAMD51 datasheet
    /** Tell DMA, that we need transfer trigger (request) for every sample */
    pwmDMA.setAction(DMA_TRIGGER_ACTON_BEAT);
    /**
     Register DMA descriptor for first buffer. Here is the trick. Vgm Decoder decodes
     audio in 16-bit PCM format, while for TCC0 we need to provide 8-bit data. Thus,
     we tell DMA to take every second (MSB-byte), and omit not-needed LSB byte.
     */
    dmac_descriptor_1 = pwmDMA.addDescriptor(
           (void *)(&buffer_tx[0][0] + 1), // + 1 is required to start from MSB byte
           (void *)(&TCC0->CCBUF[4].reg),  // CCBUF[4] is used as desination for PWM timer
           sizeof(buffer_tx[0])/4,         // Since the buffer contains 16-bit stereo data, number of samples will be 4 times less
           DMA_BEAT_SIZE_BYTE,             // Transfer only single byte to PWM timer
           true,                           // Source address is incremented every sample
           false,                          // Destination address is the same - it is PWM timer reg
           DMA_ADDRESS_INCREMENT_STEP_SIZE_4, // Shift by 4 bytes with each sample - follow 16-bit PCM stereo format
           DMA_STEPSEL_SRC);               // That all valid for data source
    /** When buffer transfer is complete, tell DMA to generate interrupt */
    dmac_descriptor_1->BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_INT;
    /** Setup descriptor for the second buffer */
    dmac_descriptor_2 = pwmDMA.addDescriptor(
           (void *)(&buffer_tx[1][0] + 1),
           (void *)(&TCC0->CCBUF[4].reg),
           sizeof(buffer_tx[1])/4,
           DMA_BEAT_SIZE_BYTE,
           true,
           false,
           DMA_ADDRESS_INCREMENT_STEP_SIZE_4,
           DMA_STEPSEL_SRC);
    dmac_descriptor_2->BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_INT;
    /** Loop DMA buffers: 0->1->0->1 and etc. */
    pwmDMA.loop(true);
    /** Set callback for every buffer transfer complete event */
    pwmDMA.setCallback(dma_callback);
}


bool fillDmaBuffer(bool ignore_active)
{
    /** If buffer to fill is the same as active, just exit */
    if ( buffer_to_fill == active_buffer && !ignore_active )
    {
        return false;
    }
    /** Decode vgm file to buffer */
    int len = vgm.decodePcm(buffer_tx[buffer_to_fill], sizeof(buffer_tx[0]));
    /**
     If len of decoded bytes is less than size of the buffer, that means the end of track.
     Fill the rest buffer with 0x00
     */
    if ( len < sizeof(buffer_tx[0]) or skipTrack )
    {
        skipTrack = false;
        memset(buffer_tx[buffer_to_fill] + len, 0, sizeof(buffer_tx[0]) - len );
        trackIndex++;
        // Check for cycling the tracks
        if ( trackIndex >= sizeof(tracks)/sizeof(tracks[0]) )
        {
            trackIndex = 0;
        }
        openTrack( );
        updatePlayingStatus(true, false);
    }
    if ( updateVolume )
    {
        updateVolume = false;
        vgm.setVolume( volume );
        updatePlayingStatus(false, true);
    }
    /** And switch to the next buffer */
    buffer_to_fill = !buffer_to_fill;
    return true;
}


// =========================================== PWM ===================================

void pwmInit()
{
    /**
     According to SAMD51 datasheet, D12 (PD11) can be connected to TCC0 timer only, channel 4.
     That's why TCC0 is the only timer, which can be used with built-in BUZZER.
     Vgm Decoder generates samples at 44100 Hz, that's standard frequency for NES computers.
     */
    MCLK->APBBMASK.reg |= MCLK_APBBMASK_TCC0;           // Activate timer TCC0

    // Set up the generic clock (GCLK7) used to clock timers
    GCLK->GENCTRL[7].reg = GCLK_GENCTRL_DIV(2) |       // Divide the 48MHz clock source by divisor 2: 48MHz/2 = 24MHz
                           GCLK_GENCTRL_IDC |          // Set the duty cycle to 50/50 HIGH/LOW
                           GCLK_GENCTRL_GENEN |        // Enable GCLK7
                           GCLK_GENCTRL_SRC_DFLL;      // Generate from 48MHz DFLL clock source
    while (GCLK->SYNCBUSY.bit.GENCTRL7);               // Wait for synchronization

    /** Again, according to datasheet, we must use peripheral 25 */
    GCLK->PCHCTRL[25].reg = GCLK_PCHCTRL_CHEN |         // Enable perhipheral channel
                            GCLK_PCHCTRL_GEN_GCLK7;     // Connect generic clock 7 to TCC0 at 24MHz

    /** Enable the peripheral multiplexer on digital pin 12 */
    PORT->Group[g_APinDescription[12].ulPort].PINCFG[g_APinDescription[12].ulPin].bit.PMUXEN = 1;
    /** Set the peripheral multiplexer for D12 to peripheral F(5): TCC0, Channel 4. That is ALT function 5 for PD11 pin */
    PORT->Group[g_APinDescription[12].ulPort].PMUX[g_APinDescription[12].ulPin >> 1].reg =
        (PORT->Group[g_APinDescription[12].ulPort].PMUX[g_APinDescription[12].ulPin >> 1].reg & 0x0F) | ( 5 << 4 );

    /**
     Set timer prescaler to 2. That will give us 24MHz / 2 = 12 millons of timer ticks per second. For byte-size PWM, every PWM cycle needs
     at least 256 ticks. That will give us ~ 12'000'000 / 256 = 46.875 kHz. Yeah, it is not the same as 44.1 kHz, but it is still
     acceptable.
    */
    TCC0->CTRLA.reg = TC_CTRLA_PRESCALER_DIV2 |        // Set prescaler to 2, 24MHz/2 = 12MHz
                      TC_CTRLA_PRESCSYNC_PRESC;        // Set the reset/reload to trigger on prescaler clock
    TCC0->WAVE.reg = TCC_WAVE_WAVEGEN_NPWM;      // Set-up TC2 timer for Normal PWM mode (NPWM)
    /** 
     In Normal PWM mode period is defined by PER register. As you remember, we calculated frequency as 46.875 kHz, that is not 44.1 kHz.
     So, to compensate that difference we can add error ro PWM counter period... 256 * (46.875 - 44.1) / 44.1 = 16.
     That will reduce volume slightly, but will fix the frequency.
     */
    TCC0->PER.reg = 255 + 16;                    // Use PER register as TOP value, set for 44.1kHz PWM 
    while (TCC0->SYNCBUSY.bit.PER);              // Wait for synchronization
    TCC0->CC[4].reg = 0;                         // Set the duty cycle to 0%
    while (TCC0->SYNCBUSY.bit.CC4);              // Wait for synchronization
    TCC0->CTRLA.bit.ENABLE = 1;                  // Enable timer TC2
    while (TCC0->SYNCBUSY.bit.ENABLE);           // Wait for synchronization
}

// =========================================== BUTTONS ================================

void button3Handler()
{
    volume = volume >= 10 ? (volume - 10) : 0;
    updateVolume = true;
}

void button2Handler()
{
    volume = volume + 10;
    updateVolume = true;
}

void button1Handler()
{
    skipTrack = true;
}
void initButtons()
{
    pinMode(BUTTON_3, INPUT);
    pinMode(BUTTON_2, INPUT);
    pinMode(BUTTON_1, INPUT);
    attachInterrupt(digitalPinToInterrupt(BUTTON_3), button3Handler, FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_2), button2Handler, FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_1), button1Handler, FALLING);
}

// =========================================== Main ===================================

void openTrack( )
{
    vgm.close();
    vgm.open(tracks[trackIndex].data, tracks[trackIndex].dataLen);
    /** Set max duration to 120 seconds for the track */
    vgm.setMaxDuration(120000);
    vgm.setFading(true);
    vgm.setSampleFrequency( 44100 );
    /** Some NSF files has several tracks inside, here we choose default */
    vgm.setTrack( tracks[trackIndex].track );
    /** Set volume to volume % */
    vgm.setVolume( volume );
}

void updatePlayingStatus(bool updateTrackName, bool updateTrackVolume)
{
    if ( updateTrackName )
    {
        display.setColor( 0 );
        display.fillRect( 16, 16, 319, 31 );
        display.setColor( RGB_COLOR16(0, 255, 255) );
        display.printFixed( 16, 16, tracks[trackIndex].name );
    }
    if ( updateTrackVolume )
    {
        if ( volume > 110 )
            display.setColor( RGB_COLOR16(255, 64, 64) );
        else
            display.setColor( RGB_COLOR16(128, 255, 64) );

        display.printFixed( 16, 32, "Volume level: " );
        char level[12];
        sprintf( level, "%u%%  ", volume );
        display.write( level );
    }
    else
    {
        display.setColor( RGB_COLOR16(0, 255, 0) );
        display.drawProgressBar( 100 * vgm.getDecodedSamples() / vgm.getTotalSamples() );
    }
}

void switchToPlayMode()
{
    /** Fill first DMA buffer before starting DMA */
    fillDmaBuffer( true );
    /** Let's go */
    pwmDMA.startJob();
    updatePlayingStatus(true, true);
    playerMode = MODE_PLAY;
}

// Output 44100Hz PWM on Metro M4 pin D12/PA17 using the TCC0 timer
void setup()
{
    display.begin();
    digitalWrite( LCD_BACKLIGHT, HIGH );
    backLight.initialize();
    backLight.setBrightness(120);
    display.setFixedFont(ssd1306xled_font8x16);
    display.clear();
    
    pinMode(12, OUTPUT);

    initButtons();
    pwmInit();
    dmaInit();

    /** Open music file */
    trackIndex = 0;
    openTrack( );


    switchToPlayMode();
}

void loop()
{
    if ( playerMode == MODE_PLAY )
    {
        /** Fill next DMA buffer if it is not used by DMA */
        if ( fillDmaBuffer( false ) )
        {
            updatePlayingStatus(false, false);
        }
    }
    else
    {
        // TODO: Implement browse mode
    }
}
