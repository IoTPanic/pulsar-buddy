#include "timer_hw.h"

#include <initializer_list>

#include <Arduino.h>
#include <wiring_private.h>

#include "timing.h"


// *** NOTE: This implementation is for the SAMD21 only.
#if defined(__SAMD21__) || defined(TC4) || defined(TC5)

/* Timer Diagram:

  QUANTUM --> event --+--> BEAT --> waveform
   (TC4)              |    (TC5)
                      |
                      +--> SEQUENCE --> waveform
                      |    (TCC0)
                      |
                      +--> MEASURE --> waveform
                      |    (TCC1)
                      |
                      +--> TUPLET --> waveform
                           (TCC2)

*/

namespace {

  void defaultCallback() { }
  void (*tcc1callback)() = defaultCallback;

#define QUANTUM_EVENT_CHANNEL   5
#define EXTCLK_EVENT_CHANNEL    6

  void startQuantumEvents() {
    EVSYS->CHANNEL.reg
      = EVSYS_CHANNEL_CHANNEL(QUANTUM_EVENT_CHANNEL)
      | EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_TC4_OVF)
      | EVSYS_CHANNEL_PATH_ASYNCHRONOUS
      ;
  }

  void stopQuantumEvents() {
    EVSYS->CHANNEL.reg
      = EVSYS_CHANNEL_CHANNEL(QUANTUM_EVENT_CHANNEL)
      | EVSYS_CHANNEL_EVGEN(0)
      | EVSYS_CHANNEL_PATH_ASYNCHRONOUS
      ;
  }

  void initializeEvents() {
    for ( uint16_t user :
          { EVSYS_ID_USER_TC5_EVU,
            EVSYS_ID_USER_TCC0_EV_0,
            EVSYS_ID_USER_TCC1_EV_0,
            EVSYS_ID_USER_TCC2_EV_0}
        ) {
      EVSYS->USER.reg
        = EVSYS_USER_USER(user)
        | EVSYS_USER_CHANNEL(QUANTUM_EVENT_CHANNEL + 1)
            // yes, +1 as per data sheet
        ;
    }
    startQuantumEvents();

    EVSYS->USER.reg
      = EVSYS_USER_USER(EVSYS_ID_USER_TCC0_MC_1)
      | EVSYS_USER_CHANNEL(EXTCLK_EVENT_CHANNEL + 1)
      ;
    EVSYS->CHANNEL.reg
      = EVSYS_CHANNEL_CHANNEL(EXTCLK_EVENT_CHANNEL)
      | EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_EIC_EXTINT_8)
      | EVSYS_CHANNEL_PATH_ASYNCHRONOUS
      ;
  }


  Tc* const quantumTc = TC4;
  Tc* const beatTc = TC5;
  Tcc* const sequenceTcc = TCC0;
  Tcc* const measureTcc = TCC1;
  Tcc* const tupletTcc = TCC2;

  // NOTE: All TC units are used in 16 bit mode.

  void sync(Tc* tc) {
    while(tc->COUNT16.STATUS.bit.SYNCBUSY);
  }

  void disableAndReset(Tc* tc) {
    tc->COUNT16.CTRLA.reg &= ~TC_CTRLA_ENABLE;
    sync(tc);

    tc->COUNT16.CTRLA.reg = TC_CTRLA_SWRST;
    sync(tc);
  }

  void enable(Tc* tc) {
    tc->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
    sync(tc);
  }

  void sync(Tcc* tcc, uint32_t mask) {
    while(tcc->SYNCBUSY.reg & mask);
  }

  void initializeTcc(Tcc* tcc) {
    tcc->CTRLA.bit.ENABLE = 0;
    sync(tcc, TCC_SYNCBUSY_ENABLE);

    tcc->CTRLA.bit.SWRST = 1;
    sync(tcc, TCC_SYNCBUSY_SWRST);

    tcc->CTRLA.bit.RUNSTDBY = 1;

    if (tcc == sequenceTcc) {
      tcc->CTRLA.bit.CPTEN1 = 1;
    }

    tcc->EVCTRL.reg
      = TCC_EVCTRL_TCEI0
      | TCC_EVCTRL_EVACT0_COUNTEV
      | ((tcc == sequenceTcc) ? TCC_EVCTRL_MCEI1 : 0)
      ;

    if (tcc == measureTcc) {
      tcc->INTENSET.reg = TCC_INTENSET_OVF;
    }
    if (tcc == sequenceTcc) {
      tcc->INTENSET.reg = TCC_INTENSET_MC1;
    }

    tcc->WEXCTRL.reg
      = TCC_WEXCTRL_OTMX(2);  // use CC[0] for all outputs

    tcc->WAVE.reg
      = TCC_WAVE_WAVEGEN_NPWM;
    sync(tcc, TCC_SYNCBUSY_WAVE);

    tcc->CTRLA.bit.ENABLE = 1;
    sync(tcc, TCC_SYNCBUSY_ENABLE);
  }

  void syncAll( uint32_t mask) {
    sync(measureTcc, mask);
    sync(sequenceTcc, mask);
    sync(tupletTcc, mask);
  }

  void readCounts(Timing& counts) {
    counts.measure = measureTcc->COUNT.reg;
    counts.sequence = sequenceTcc->COUNT.reg;
    counts.beat = qcast(beatTc->COUNT16.COUNT.reg);
    counts.tuplet = tupletTcc->COUNT.reg;
    syncAll(TCC_SYNCBUSY_COUNT);
    sync(beatTc);
  }

  void writeCounts(Timing& counts) {
    measureTcc->COUNT.reg = counts.measure;
    sequenceTcc->COUNT.reg = counts.sequence;
    beatTc->COUNT16.COUNT.reg = static_cast<uint16_t>(counts.beat);
    tupletTcc->COUNT.reg = counts.tuplet;
    syncAll(TCC_SYNCBUSY_COUNT);
    sync(beatTc);
  }

  void writePeriods(Timing& counts) {
    measureTcc->PER.reg = counts.measure;
    sequenceTcc->PER.reg = counts.sequence;
    beatTc->COUNT16.CC[0].reg = static_cast<uint16_t>(counts.beat);
    tupletTcc->PER.reg = counts.tuplet;
    syncAll(TCC_SYNCBUSY_PER);
    sync(beatTc);

    measureTcc->CC[0].reg = min(Q_PER_B, counts.measure) / 4;
    sequenceTcc->CC[0].reg = min(Q_PER_B, counts.sequence) / 4;
    beatTc->COUNT16.CC[1].reg = static_cast<uint16_t>(counts.beat / 4);
    tupletTcc->CC[0].reg = min(Q_PER_B, counts.tuplet) / 4;
    syncAll(TCC_SYNCBUSY_CC0);
    sync(beatTc);
  }

  uint16_t  CpuClockDivisor(double bpm) {
    static const double factor = 60.0 * F_CPU / Q_PER_B;
    return round(factor / bpm);
  }


  SyncMode captureSync = syncFixed;
  const int captureBufferSize = 64;
  q_t captureBuffer[captureBufferSize];
  int captureNext = 0;
  int capturesPerBeat = 0;
  int capturesNeeded = 0;
  const q_t qMod = 4 * Q_PER_B;

  void capture(q_t sample) {
    if (capturesPerBeat == 0)
      return;

    captureBuffer[captureNext] = sample % qMod;
    captureNext = (captureNext + 1) % captureBufferSize;
    if (capturesNeeded > 0)
      capturesNeeded -= 1;
      return;
  }

  void clearCapture(int perBeat) {
    capturesPerBeat = 0;
    captureNext = 0;
    capturesNeeded = perBeat;
    capturesPerBeat = perBeat;
  }
}

void setTimerBpm(double bpm) {
  // FIXME: would be better to adjust count to match remaining fration used
  // would be more efficient to keep previous divisor so no need to sync
  // read it back from the timer again

  quantumTc->COUNT16.CC[0].bit.CC = CpuClockDivisor(bpm);
  sync(quantumTc);
}

void resetSync(SyncMode sync) {
  if (captureSync == sync)
    return;
  captureSync = sync;

  int perBeat = 0;
  switch (sync) {
    case syncFixed: perBeat = 0; break;
    case syncTap: perBeat = 1; break;
    default:
      if (sync & syncExternalFlag) {
        perBeat = sync & syncPPQNMask;
      } else {
        perBeat = 0;
      }
  }

  clearCapture(perBeat);
}

void syncBPM() {
  if (capturesNeeded > 0) {
    Serial.printf("still need %d captures\n", capturesNeeded);
    return;
  }

  int i = (captureNext + captureBufferSize - 1) % captureBufferSize;
  int j = (i + captureBufferSize - capturesPerBeat) % captureBufferSize;

  q_t qdiff = (captureBuffer[i] + qMod - captureBuffer[j]) % qMod;

  Serial.printf("from %d to %d -> %dq\n", j, i, qdiff);
  if (qdiff <= 0)
    return;

  double extbpm = bpm * (double)Q_PER_B / (double)qdiff;
  double newbpm = (bpm + 15 * extbpm) / 16;

  Serial.printf("calc bpm %d, update bpm %d, delta %dppm\n",
      round(extbpm), round(newbpm), round(1000000*abs(extbpm-bpm)/bpm));

  bpm = newbpm;
  setTimerBpm(bpm);
  // clearCapture(capturesPerBeat);
}



void initializeTimers(double bpm, void (*onMeasure)()) {

  tcc1callback = onMeasure;


  /** POWER **/

  PM->APBAMASK.reg
    |= PM_APBAMASK_EIC;

  PM->APBCMASK.reg
    |= PM_APBCMASK_EVSYS
    | PM_APBCMASK_TCC0
    | PM_APBCMASK_TCC1
    | PM_APBCMASK_TCC2
    | PM_APBCMASK_TC4
    | PM_APBCMASK_TC5;


  /** CLOCKS **/

  for ( uint16_t id : { GCM_EIC, GCM_TCC0_TCC1, GCM_TCC2_TC3, GCM_TC4_TC5 }) {
    GCLK->CLKCTRL.reg
      = GCLK_CLKCTRL_CLKEN
      | GCLK_CLKCTRL_GEN_GCLK0
      | GCLK_CLKCTRL_ID(id);
    while (GCLK->STATUS.bit.SYNCBUSY);
  }


  /** EVENTS **/

  initializeEvents();


  /** EXTERNAL INTERRUPT **/

  EIC->CTRL.reg = EIC_CTRL_SWRST;
  while (EIC->STATUS.bit.SYNCBUSY);
  EIC->EVCTRL.reg
    |= EIC_EVCTRL_EXTINTEO8;
  EIC->CONFIG[1].bit.FILTEN0 = 1;
  EIC->CONFIG[1].bit.SENSE0 = EIC_CONFIG_SENSE0_RISE_Val;
  EIC->CTRL.reg = EIC_CTRL_ENABLE;
  while (EIC->STATUS.bit.SYNCBUSY);


  /** QUANTUM TIMER **/

  disableAndReset(quantumTc);

  quantumTc->COUNT16.CTRLA.reg
    = TC_CTRLA_MODE_COUNT16
    | TC_CTRLA_WAVEGEN_MFRQ
    | TC_CTRLA_PRESCALER_DIV1
    | TC_CTRLA_RUNSTDBY  // FIXME: should this be here?
    | TC_CTRLA_PRESCSYNC_GCLK
    ;
  sync(quantumTc);

  quantumTc->COUNT16.EVCTRL.reg
    = TC_EVCTRL_OVFEO
    ;

  setTimerBpm(bpm);

  enable(quantumTc);


  /** BEAT TIMER **/

  disableAndReset(beatTc);

  beatTc->COUNT16.CTRLA.reg
    = TC_CTRLA_MODE_COUNT16
    | TC_CTRLA_WAVEGEN_MPWM
    | TC_CTRLA_PRESCALER_DIV1
    | TC_CTRLA_RUNSTDBY  // FIXME: should this be here?
    | TC_CTRLA_PRESCSYNC_GCLK
    ;
  sync(beatTc);

  beatTc->COUNT16.EVCTRL.reg
    = TC_EVCTRL_TCEI
    | TC_EVCTRL_EVACT_COUNT
    ;

  beatTc->COUNT16.CC[0].bit.CC = Q_PER_B;        // period
  beatTc->COUNT16.CC[1].bit.CC = Q_PER_B / 4;    // pulse width

  enable(beatTc);

  pinPeripheral(PIN_SPI_SCK, PIO_TIMER);
    // Despite the Adafruit pinout diagram, this is actuall pin 30
    // on the Feather M0 Express. It is SAMD21's port PB11, and
    // configuring it for function "E" (PIO_TIMER) maps it to TC5.WO1.


  initializeTcc(measureTcc);
  pinPeripheral(PAD_SPI_RX, PIO_TIMER);
  pinPeripheral(PAD_SPI_TX, PIO_TIMER);
  NVIC_SetPriority(TCC1_IRQn, 0);
  NVIC_EnableIRQ(TCC1_IRQn);

  initializeTcc(sequenceTcc);
  pinPeripheral(PIN_SPI_MOSI, PIO_TIMER_ALT);
  pinMode(PIN_A1, INPUT_PULLUP);
  pinPeripheral(PIN_A1, PIO_EXTINT);
  NVIC_SetPriority(TCC0_IRQn, 0);
  NVIC_EnableIRQ(TCC0_IRQn);

  initializeTcc(tupletTcc);
  pinPeripheral(PIN_SPI_MISO, PIO_TIMER);
}

void resetTimers(const Settings& settings) {
  Timing periods;
  Timing counts = { 0, 0, 0 };

  computePeriods(settings, periods);

  stopQuantumEvents();
  writeCounts(counts);
  writePeriods(periods);
  startQuantumEvents();
}

void updateTimers(const Settings& settings) {
  Timing periods;
  Timing counts;

  computePeriods(settings, periods);

  stopQuantumEvents();
  readCounts(counts);
  adjustOffsets(periods, counts);
  writeCounts(counts);
  writePeriods(periods);
  startQuantumEvents();
}

void dumpTimer() {
  uint16_t quantumCount = quantumTc->COUNT16.COUNT.reg;
  uint16_t beatCount = beatTc->COUNT16.COUNT.reg;

  Serial.print("q count = ");
  Serial.print(quantumCount);
  Serial.print(", b count = ");
  Serial.println(beatCount);
}

void dumpCapture() {

  int32_t sumOfDeltas = 0;
  uint32_t n = 0;

  for (int i = 0; i < captureBufferSize; ++i) {
    int32_t delta = (i == 0) ? -1 : captureBuffer[i] - captureBuffer[i-1];

    if (delta < 0) {
      Serial.printf("[%2d] %8d\n", i, captureBuffer[i]);
    } else {
      sumOfDeltas += delta;
      n += 1;
      Serial.printf("[%2d] %8d (+%8d)\n", i, captureBuffer[i], delta);
    }
  }

  if (n > 0)
    Serial.printf("Average: %8d\n\n", sumOfDeltas / n);

  if (capturesPerBeat == 0)
    return;

  Serial.printf("captureNext = %d\n", captureNext);
}

void TCC0_Handler() {
  if (TCC0->INTFLAG.reg & TCC_INTFLAG_MC1) {
    capture(TCC0->CC[1].reg);
  }
  TCC0->INTFLAG.reg = TCC_INTFLAG_MC1;    // writing 1 clears the flag
}

void TCC1_Handler() {
  if (TCC1->INTFLAG.reg & TCC_INTFLAG_OVF) {
    tcc1callback();
    TCC1->INTFLAG.reg = TCC_INTFLAG_OVF;  // writing 1 clears the flag
  }
}

#endif // __SAMD21__ and used TC and TCC units
