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
#include "src/vgm_decoder/vgm_file.h"
#include "vampire_killer.h"
#include "bucky_ohare.h"

#include <string.h>

/**
 Create our Vgm decoder engine. It is used to decode Vgm or Nsf data.
 Vgm format is widely used as storage format for different platforms: MSX/MSX2, NES.
 Nsf format is valid only for NES platform. The decoder supports both.
 */
VgmFile vgm;

void openNextTrack();

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
           sizeof(buffer_tx[0])/2,         // Since the buffer contains 16-bit data, number of samples will be twice less
           DMA_BEAT_SIZE_BYTE,             // Transfer only single byte to PWM timer
           true,                           // Source address is incremented every sample
           false,                          // Destination address is the same - it is PWM timer reg
           DMA_ADDRESS_INCREMENT_STEP_SIZE_2, // Shift by 2 bytes with each sample - follow 16-bit PCM format
           DMA_STEPSEL_SRC);               // That all valid for data source
    /** When buffer transfer is complete, tell DMA to generate interrupt */
    dmac_descriptor_1->BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_INT;
    /** Setup descriptor for the second buffer */
    dmac_descriptor_2 = pwmDMA.addDescriptor(
           (void *)(&buffer_tx[1][0] + 1),
           (void *)(&TCC0->CCBUF[4].reg),
           sizeof(buffer_tx[1])/2,
           DMA_BEAT_SIZE_BYTE,
           true,
           false,
           DMA_ADDRESS_INCREMENT_STEP_SIZE_2,
           DMA_STEPSEL_SRC);
    dmac_descriptor_2->BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_INT;
    /** Loop DMA buffers: 0->1->0->1 and etc. */
    pwmDMA.loop(true);
    /** Set callback for every buffer transfer complete event */
    pwmDMA.setCallback(dma_callback);
}


void fillDmaBuffer(bool ignore_active)
{
    /** If buffer to fill is the same as active, just exit */
    if ( buffer_to_fill == active_buffer && !ignore_active )
    {
        return;
    }
    /** Decode vgm file to buffer */
    int len = vgm.decodePcm(buffer_tx[buffer_to_fill], sizeof(buffer_tx[0]));
    /**
     If len of decoded bytes is less than size of the buffer, that means the end of track.
     Fill the rest buffer with 00000
     */
    if ( len < sizeof(buffer_tx[0]) )
    {
        memset(buffer_tx[buffer_to_fill] + len, 0, sizeof(buffer_tx[0]) - len );
        openTrack( bucky_ohare_nsf, bucky_ohare_nsf_len, 0 );
    }
    /** And switch to the next buffer */
    buffer_to_fill = !buffer_to_fill;
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
     Set timer prescaler to 1. That will give us 24MHz / 1 = 24 millons of timer ticks per second. For byte-size PWM, every PWM cycle needs
     at least 256 ticks. That will give us ~ 46.875 KhZ. Yeah, it is not the same as 44.1 kHz, but it is still
     acceptable.
    */
    TCC0->CTRLA.reg = TC_CTRLA_PRESCALER_DIV1 |        // Set prescaler to 1, 23MHz/1 = 12MHz should be 1
                      TC_CTRLA_PRESCSYNC_PRESC;        // Set the reset/reload to trigger on prescaler clock
    TCC0->WAVE.reg = TCC_WAVE_WAVEGEN_NPWM;      // Set-up TC2 timer for Normal PWM mode (NPWM)
    /** 
     In Normal PWM mode period is defined by PER register. As you remember, we calculated frequency as 46.875 kHz, that is not 44.1 kHz.
     So, to compensate that difference we can add error ro PWM counter period... 256 * (46.875 - 44.1) / 44.1 = 16.
     That will reduce volume slightly, but will fix the frequency.
     */
    TCC0->PER.reg = 255 + 16;                    // Use PER register as TOP value, set for 48kHz PWM 
    while (TCC0->SYNCBUSY.bit.PER);              // Wait for synchronization
    TCC0->CC[4].reg = 0;                         // Set the duty cycle to 0%
    while (TCC0->SYNCBUSY.bit.CC4);              // Wait for synchronization
    TCC0->CTRLA.bit.ENABLE = 1;                  // Enable timer TC2
    while (TCC0->SYNCBUSY.bit.ENABLE);           // Wait for synchronization
}

// =========================================== Main ===================================

void openTrack( const uint8_t *data, int len, int track )
{
    vgm.close();
    vgm.open(data, len);
    /** Set max duration to 120 seconds for the track */
    vgm.setMaxDuration(120000);
    vgm.setFading(true);
    vgm.setSampleFrequency( 44100 );
    /** Some NSF files has several tracks inside, here we choose default */
    vgm.setTrack( track );
    /** Set volume to 120% */
    vgm.setVolume( 120 );
}

// Output 44100Hz PWM on Metro M4 pin D12/PA17 using the TCC0 timer
void setup()
{
    pinMode(12, OUTPUT);

    pwmInit();
    dmaInit();

    /** Open music file */
    openTrack( vampire_killer_vgm, vampire_killer_vgm_len, 0 );

    /** Fill first DMA buffer before starting DMA */
    fillDmaBuffer( true );

    /** Let's go */
    pwmDMA.startJob();
}

void loop()
{
    /** Fill next DMA buffer if it is not used by DMA */
    fillDmaBuffer( false );
}
