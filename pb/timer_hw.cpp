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

  Note on synchonization:

  TC units: No sync is required by the MCU as the bus and CPU will just stall
    until things are ready. This is fine. The only sync done here is to ensure
    that something is complete before proceeding.

  TCC units: Sync is required because access while not sync'd will cause a HW
    interrupt. Post write sync isn't required, except to ensure completion
    before proceeding.
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

  inline void sync(Tc* tc) {
    while(tc->COUNT16.STATUS.bit.SYNCBUSY);
  }

  inline void disableAndReset(Tc* tc) {
    tc->COUNT16.CTRLA.reg &= ~TC_CTRLA_ENABLE;
    sync(tc);

    tc->COUNT16.CTRLA.reg = TC_CTRLA_SWRST;
    sync(tc);
  }

  inline void enable(Tc* tc) {
    sync(tc);   // make sure all changes are sync'd before enabling
    tc->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
    sync(tc);
  }

  inline void sync(Tcc* tcc, uint32_t mask) {
    while(tcc->SYNCBUSY.reg & mask);
  }

  void initializeTcc(Tcc* tcc) {
    sync(tcc, TCC_SYNCBUSY_ENABLE);
    tcc->CTRLA.bit.ENABLE = 0;
    sync(tcc, TCC_SYNCBUSY_ENABLE);

    sync(tcc, TCC_SYNCBUSY_SWRST);
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

    tcc->DRVCTRL.reg
      = TCC_DRVCTRL_INVEN0
      | TCC_DRVCTRL_INVEN1
      | TCC_DRVCTRL_INVEN2
      | TCC_DRVCTRL_INVEN3
      | TCC_DRVCTRL_INVEN4
      | TCC_DRVCTRL_INVEN5
      | TCC_DRVCTRL_INVEN6
      | TCC_DRVCTRL_INVEN7;
      // inverted, because output buffer swaps
      // some times

    sync(tcc, TCC_SYNCBUSY_WAVE);
    tcc->WAVE.reg
      = TCC_WAVE_WAVEGEN_NPWM;
    sync(tcc, TCC_SYNCBUSY_WAVE);

    sync(tcc, TCC_SYNCBUSY_ENABLE);
    tcc->CTRLA.bit.ENABLE = 1;
    sync(tcc, TCC_SYNCBUSY_ENABLE);
  }

  void readCounts(Timing& counts) {
    // request read sync, with command written to ctrlb
    sync(measureTcc, TCC_SYNCBUSY_CTRLB);
    sync(sequenceTcc, TCC_SYNCBUSY_CTRLB);
    sync(tupletTcc, TCC_SYNCBUSY_CTRLB);
    measureTcc->CTRLBSET.reg = TCC_CTRLBSET_CMD_READSYNC;
    sequenceTcc->CTRLBSET.reg = TCC_CTRLBSET_CMD_READSYNC;
    tupletTcc->CTRLBSET.reg = TCC_CTRLBSET_CMD_READSYNC;

    // wait for command to complete
    while (measureTcc->CTRLBSET.bit.CMD);
    while (sequenceTcc->CTRLBSET.bit.CMD);
    while (tupletTcc->CTRLBSET.bit.CMD);

    // sync for count (probably redudant with the above?)
    sync(measureTcc, TCC_SYNCBUSY_COUNT);
    sync(sequenceTcc, TCC_SYNCBUSY_COUNT);
    sync(tupletTcc, TCC_SYNCBUSY_COUNT);
    counts.measure = measureTcc->COUNT.reg;
    counts.sequence = sequenceTcc->COUNT.reg;
    counts.beat = qcast(beatTc->COUNT16.COUNT.reg);
    counts.tuplet = tupletTcc->COUNT.reg;
  }

  void writeCounts(Timing& counts) {
    // sync as a group - though quantum is stopped, so shouldn't matter
    sync(measureTcc, TCC_SYNCBUSY_COUNT);
    sync(sequenceTcc, TCC_SYNCBUSY_COUNT);
    sync(tupletTcc, TCC_SYNCBUSY_COUNT);

    measureTcc->COUNT.reg = counts.measure;
    sequenceTcc->COUNT.reg = counts.sequence;
    beatTc->COUNT16.COUNT.reg = static_cast<uint16_t>(counts.beat);
    tupletTcc->COUNT.reg = counts.tuplet;
  }

  Timing activePeriods;

  void writePeriods(Timing& periods) {
    // sync as a group - though quantum is stopped, so shouldn't matter
    sync(measureTcc, TCC_SYNCBUSY_PER | TCC_SYNCBUSY_CC0);
    sync(sequenceTcc, TCC_SYNCBUSY_PER | TCC_SYNCBUSY_CC0);
    sync(tupletTcc, TCC_SYNCBUSY_PER | TCC_SYNCBUSY_CC0);

    measureTcc->PER.reg = periods.measure - 1;
    sequenceTcc->PER.reg = periods.sequence - 1;
    beatTc->COUNT16.CC[0].reg = static_cast<uint16_t>(periods.beat - 1);
    tupletTcc->PER.reg = periods.tuplet - 1;

    measureTcc->CC[0].reg = min(Q_PER_B, periods.measure) / 4 - 1;
    sequenceTcc->CC[0].reg = min(Q_PER_B, periods.sequence) / 4 - 1;
    beatTc->COUNT16.CC[1].reg = static_cast<uint16_t>(periods.beat / 4 - 1);
    tupletTcc->CC[0].reg = min(Q_PER_B, periods.tuplet) / 4 - 1;

    activePeriods = periods;
  }


  // TC4 has a 16 bit counter
  typedef uint16_t divisor_t;
  const float cpuFactor = 60.0f * F_CPU / Q_PER_B;


  float divisorToBpm(divisor_t divisor) {
    return roundf(cpuFactor / (float)divisor);
  }

  divisor_t divisorFromBpm(float bpm) {
    return roundf(cpuFactor / (float)bpm);
  }

  constexpr divisor_t divisorMin = 952;
  constexpr divisor_t divisorMax = 9524;

  // the divisor currently set on the timer, may jitter to correct phase
  divisor_t activeDivisor = 0;  // so it will be set the first time

  // the divisor that matches the sync'd BPM
  divisor_t targetDivisor = 0;

  void setDivisors(divisor_t target, divisor_t active) {
    if (activeDivisor != active) {
      // FIXME: would be better to adjust count to match remaining fration used
      // would be more efficient to keep previous divisor so no need to sync
      // read it back from the timer again
      quantumTc->COUNT16.CC[0].bit.CC = active - 1;
      activeDivisor = active;
    }

    targetDivisor = target;
  }

  template< typename T >
  inline T roundingDivide(T x, T q) {
    return (x + q / 2) / q;
  }

  // These are used to communicate changes in the clocking to the interrupt
  // service routine.
  volatile int externalClocksPerBeat = 0;
  volatile bool pendingExternalClocksPerBeatChange = false;

  // These variables "belong" to the capture() interrupt service routine.
  // They should not be modified, or even read, outside of it. They are
  // defined here only so debugging code can dump them.
  int       capturesPerBeat = 0;
  q_t       captureClkQ = 0;
  q_t       captureClkQHalf = 0;
  q_t       captureSequencePeriod = 0;
  const int captureBufferBeats = 2;
  const int captureBufferSize = captureBufferBeats * 48;
  divisor_t captureBuffer[captureBufferSize];
  divisor_t captureHistory[captureBufferSize];
  int       captureBufferSpan;
  int       captureNext = 0;
  uint32_t  captureSum = 0;
  int       captureCount = 0;

  q_t captureLastSample = 0;

  void capture(q_t sample) {
    if (pendingExternalClocksPerBeatChange) {
      int perBeat = constrain(externalClocksPerBeat, 0, captureBufferSize);
      if (capturesPerBeat != perBeat) {
        capturesPerBeat = perBeat;
        captureClkQ = Q_PER_B / perBeat;
        captureClkQHalf = captureClkQ / 2;
        captureBufferSpan = captureBufferBeats * perBeat;
        captureNext = 0;
        captureSum = 0;
        captureCount = 0;
        captureLastSample = 0;
      }
      pendingExternalClocksPerBeatChange = false;
    }

    if (capturesPerBeat == 0)     // not sync'd
      return;

    if (captureSequencePeriod != activePeriods.sequence) {
      captureSequencePeriod = activePeriods.sequence;
      captureLastSample = 0;    // can't rely on last sanple if period changd
    }

    if (captureLastSample > 0) {

      // compute current ext clk rate, as a divisor
      q_t qdiff =
        (sample + captureSequencePeriod - captureLastSample)
        % captureSequencePeriod;

      uint32_t dNext =
        roundingDivide((uint32_t)activeDivisor * qdiff, captureClkQ);
      dNext = constrain(dNext, divisorMin, divisorMax);

      // record this in the capture buffer, and average it with up to
      // captureBufferBeats' worth of measurements
      captureSum += dNext;
      if (captureCount >= captureBufferSpan) {
        captureSum -= captureBuffer[captureNext];
      } else {
        captureCount += 1;
      }
      captureBuffer[captureNext] = dNext;

      // compute the average ext clk rate over the last captureBufferBeats
      uint32_t dFilt = roundingDivide(captureSum, (uint32_t)captureCount);
      captureHistory[captureNext] = dFilt;

      captureNext = (captureNext + 1) % captureBufferSpan;

      // phase error (in Q)
      q_t phase = sample % captureClkQ;
      if (phase >= captureClkQHalf)
        phase -= captureClkQ;

      // adjust filterd divisor to fix the phase error over one beat
      uint32_t dAdj = roundingDivide(dFilt * Q_PER_B, Q_PER_B - phase);
      dAdj = constrain(dAdj, divisorMin, divisorMax);

      setDivisors(dFilt, dAdj);
    }
    captureLastSample = sample;
  }

  void resetCapture(int perBeat) {
    // noInterrupts();  // shouldn't be needed given the access pattern
    externalClocksPerBeat = perBeat;
    pendingExternalClocksPerBeatChange = true;
    // interrupts();
  }
}


bpm_t currentBpm() {
  static auto updateAt = millis();
  static float reportedBpmf = 0;
  static bpm_t reportedBpm = 0;

  if (updateAt < millis()) {
    updateAt += 100;    // recompute only 10x a second

    auto targetBpmf = divisorToBpm(targetDivisor);
    if (fabsf(targetBpmf - reportedBpmf) < 1.0f) {
      // moving a little bit, so just slew slowly
      reportedBpmf = (10.0f * reportedBpmf + targetBpmf) / 11.0f;
    } else {
      // moved a lot
      reportedBpmf = targetBpmf;
    }

    reportedBpm = roundf(reportedBpmf);
  }

  return reportedBpm;
}

void setBpm(bpm_t bpm) {
  bpm = constrain(bpm, bpmMin, bpmMax);
  divisor_t d = divisorFromBpm((float)bpm);
  setDivisors(d, d);
}

void setSync(SyncMode sync) {
  int clocksPerBeat = 0;
  switch (sync) {
    case syncFixed: clocksPerBeat = 0; break;
    case syncTap: clocksPerBeat = 1; break;
    default:
      if (sync & syncExternalFlag) {
        clocksPerBeat = sync & syncPPQNMask;
      } else {
        clocksPerBeat = 0;
      }
  }

  resetCapture(clocksPerBeat);
}



void initializeTimers(const State& state, void (*onMeasure)()) {

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
  EIC->CONFIG[1].bit.SENSE0 = EIC_CONFIG_SENSE0_FALL_Val;
    // input buffer circuit inverts the signal
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

  quantumTc->COUNT16.EVCTRL.reg
    = TC_EVCTRL_OVFEO
    ;

  setBpm(state.userBpm);
  setSync(state.syncMode);

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

  beatTc->COUNT16.CTRLC.reg
    = TC_CTRLC_INVEN0
    | TC_CTRLC_INVEN1;

  beatTc->COUNT16.EVCTRL.reg
    = TC_EVCTRL_TCEI
    | TC_EVCTRL_EVACT_COUNT
    ;

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

  resetTimers(state.settings);
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
  Timing counts;

  stopQuantumEvents();
  uint16_t quantumCount = quantumTc->COUNT16.COUNT.reg;
  readCounts(counts);
  startQuantumEvents();

  Serial.println("dumpTimer counts:");
  Serial.printf("  q = %d / %d\n", quantumCount, activeDivisor);
  dumpTiming(counts);
}

void dumpCapture() {
  Serial.printf("captures per beat: %d\n", capturesPerBeat);
  Serial.printf("capture count: %d\n", captureCount);
  if (capturesPerBeat == 0)
    return;

  int32_t sum = 0;

  for (int i = 0; i < captureCount; ++i) {
    sum += captureBuffer[i];
    Serial.printf("[%2d] %8d -- %8d\n", i, captureBuffer[i], captureHistory[i]);
  }

  if (captureCount > 0)
    Serial.printf("Sum: %8d, Average: %8d\n\n", sum, sum / captureCount);

  Serial.printf("captureSum = %d\n", captureSum);

  Serial.printf("active: %3d bpm - %5d divisor  |  "
                "target: %3d bpm - %5d divisor\n",
    divisorToBpm(activeDivisor), activeDivisor,
    divisorToBpm(targetDivisor), targetDivisor);
}

void TCC0_Handler() {
  if (TCC0->INTFLAG.reg & TCC_INTFLAG_MC1) {
    sync(TCC0, TCC_SYNCBUSY_CC1);
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
