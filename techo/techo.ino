/* Board libraries expose C++ APIs. The receiver tasks and decoder are C. */
#include <SPI.h>
#include <Adafruit_TinyUSB.h>
#include <InternalFileSystem.h>
#include <Adafruit_GFX.h>
#include "board.h"

void setup() { board_begin(); techo_start(); }
void loop() { delay(1000); }
