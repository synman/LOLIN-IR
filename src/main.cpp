/***************************************************************************
Copyright © 2023 Shell M. Shrader <shell at shellware dot com>
----------------------------------------------------------------------------
This work is free. You can redistribute it and/or modify it under the
terms of the Do What The Fuck You Want To Public License, Version 2,
as published by Sam Hocevar. See the COPYING file for more details.
****************************************************************************/
#include "main.h"

void setup() {
  INIT_LED;
  LOG_WELCOME_MSG("\nLOLIN-IR diagnostics - Press ? for a list of commands\n");
  LOG_BEGIN(1500000);

  LOG_PRINTLN("\n\nLOLIN-IR Sensor Event Publisher v1.0.0");

  coreSetup();

#if DECODE_HASH
  // Ignore messages with less than minimum on or off pulses.
  irrecv.setUnknownThreshold(MIN_UNKNOWN_SIZE);
#endif  // DECODE_HASH

  irrecv.enableIRIn();  // Start the receiver
  LOG_PRINT("IRrecv is running and waiting for IR input on Pin ");
  LOG_PRINTLN(RECV_PIN);

  irsend.begin();
  LOG_PRINT("IRsend is running and using Pin ");
  LOG_PRINTLN(IR_LED);

  // LittleFS.remove("/last_signal.txt");
  // LittleFS.remove("/signals.txt");

  // setup done
  LOG_PRINTLN("\nSystem Ready");
}

void loop() {
  coreLoop();

  // Check if the IR code has been received.
  if (irrecv.decode(&results) && !results.repeat && !results.overflow) {
    uint16_t* rawBuf = resultToRawArray(&results);

    LOG_PRINTF("IRrecv: [%s]\n", (char*)rawBuf);

    File file = LittleFS.open("/signals.txt", FILE_APPEND);
    file.printf("%s: [%s]\n", getTimestamp().c_str(), (char*)rawBuf);
    file.close();

    file = LittleFS.open("/last_signal.txt", FILE_WRITE);
    file.write((char*)rawBuf, results.rawlen);
    file.close();

    free(rawBuf);
  }

  watchDogRefresh();
}
