## Wio Terminal Nes Buzzer player

Wio Terminal NES music player. It uses standard BUZZER and doesn't require any
hardware except Wio Terminal itself

## Dependencies

1. [Adafruit_ZeroDMA](https://github.com/adafruit/Adafruit_ZeroDMA) (already included to Wio Terminal Arduino IDE board package)
2. [Vgm Decoder](https://github.com/lexus2k/vgm_decoder/tree/arduino) (Already included to this project)

## How to build

1. You need Wio Terminal hardware, based on SAMD51
2. Open project in Arduino
3. Compile and flash

## How to convert binary music files to c++ code

> xxd -i bucky_ohare.nsf > bucky_ohare.cpp


## License

MIT License

Copyright (c) 2021 Aleksei Dynda

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
