// Minimal stand-in so Elecrow's EPD.cpp (drawing only) compiles on a desktop.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 1; }
inline void pinMode(int, int) {}
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
