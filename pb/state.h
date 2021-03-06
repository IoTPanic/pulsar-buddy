#ifndef _INCLUDE_STATE_H_
#define _INCLUDE_STATE_H_

#include <stdint.h>

/* SETTINGS */

struct Settings {
  uint8_t numberMeasures;
  uint8_t beatsPerMeasure;
  uint8_t beatUnit;

  uint8_t tupletCount;
  uint8_t tupletTime;
  uint8_t tupletUnit;
};


/* STATE */

enum SyncMode : uint8_t {
  syncFixed = 0,

  // sync based on incoming clock
  syncExternalFlag = 0x80,  // high bit indicates external sync
  syncPPQNMask = 0x3f,      // lower 6 bits are parts per beat
    // -- defined for generality, but only the values below are supported

  sync1ppqn = 0x81,    // clock = quarter note (beat)
  sync2ppqn = 0x82,    // clock = eigth note
  sync4ppqn = 0x84,    // clock = sixteenth note
  sync8ppqn = 0x88,    // clock = 32nd note (Pulsar sync)
  sync24ppqn = 0x98,   // clock = 24ppqn DIN sync
  sync48ppqn = 0xb0,   // clock = 48ppqn DIN sync

  syncMidiUSB = 0xd8,     // clock = 24ppqn MIDI sync over USB
};

enum PulseWidth : uint8_t {
  pulseFixedShort = 0x00,  // just a short, ~2ms pulse

  pulseDutyHalf = 0x12,   //
  pulseDutyThird = 0x13,
  pulseDutyQuarter = 0x14,

  pulseDuration16 = 0x21,
  pulseDuration32 = 0x22,

};

enum OutputMode : uint8_t {
  outputSequence  = 0x00,
  outputMeasure   = 0x01,
  outputBeat      = 0x02,
  otuputTuplet    = 0x03,
};

struct State {
  Settings    settings;
  uint8_t     memoryIndex;
  SyncMode    syncMode;
  uint16_t    userBpm;

  // added in V2 (first two bytes overwrite a 'reserved' area)
  PulseWidth  pulseWidthS = pulseFixedShort;
  PulseWidth  pulseWidthM = pulseFixedShort;
  PulseWidth  pulseWidthB = pulseFixedShort;
  PulseWidth  pulseWidthT = pulseFixedShort;

  OutputMode  outputModeS = outputSequence;
  OutputMode  outputModeM = outputMeasure;
  OutputMode  outputModeB = outputBeat;
  OutputMode  outputModeT = otuputTuplet;
};

// Settings and the memory index are buffered:
//  - the user version reflects what the user has choosen in the UI
//  - the active version reflects what is playing on the timers
// Normally, pending changes in the user version are committed to the activer
// version on a measure boundary.

State& userState();
  // the state the user wants, can be freely modified by UI code

const State& activeState();
  // the state that is currently active, used by the timer engine

inline       Settings& userSettings()   { return userState().settings; }
inline const Settings& activeSettings() { return activeState().settings; }

// These are true if the user state differs from the active state.
bool pendingLoop();
bool pendingTuplet();
bool pendingMemory();
bool pendingState();

void commitState();
  // make the user state the active state

// Settings are also saved to Flash so that they can be restored on start
// up. All user settings are saved a few seconds after they have changed
// but are stable. This conserves Flash by not writing on every UI change.
// When user state is committed to active state, it is also saved immediately.

void persistState();
  // simply call this repeatedly in loop()



void loadFromMemory(int index);
void storeToMemory(int index);
  // memory indexes are numbered from 1, because that's how users roll.

void showMemoryPreview(int index);
void endMemoryPreview();

void initializeState();

#endif // _INCLUDE_STATE_H_
