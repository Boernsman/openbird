#pragma once

// Door chime on an I2S amplifier (e.g. MAX98357A). The "ding-dong" is
// synthesized while playing and runs in its own task.

void chimeBegin();
void chimePlay(); // ignored while a chime is already playing
bool chimeAvailable();
