#include "layout.h"

#include "display.h"
#include "state.h"
#include "ui_field.h"
#include "ui_memory.h"
#include "ui_music.h"


namespace {
  // NOTE: Be careful when adjusting the range of available
  // settings, because they can produce timing periods that
  // are too big for the timers.

  auto fieldNumberMeasures
    = ValueField<uint16_t>(18,  5, 12, 20,
        userSettings().numberMeasures,
        { 1, 2, 3, 4, 5, 6, 7, 8 }
        );

  auto fieldBeatsPerMeasure
    = ValueField<uint16_t>(39,  0, 24, 14,
        userSettings().beatsPerMeasure,
        { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }
        );

  auto fieldBeatUnit
    = ValueField<uint16_t>(39, 17, 24, 14,
        userSettings().beatUnit,
        { 2, 4, 8, 16 }
        );

  auto commonTimeSignatures
    = TimeSignatureField(39, 14, 24, 3,
        fieldBeatsPerMeasure, fieldBeatUnit
        );

  auto pendingLoopIndicator
    = PendingIndicator(18, 0, pendingLoop);


  auto fieldTupletCount
    = ValueField<uint16_t>(66,  5, 12, 20,
        userSettings().tupletCount,
        { 2, 3, 4, 5, 6, 7, 8, 9 }
        );
  auto fieldTupletTime
    = ValueField<uint16_t>(84,  5, 12, 20,
        userSettings().tupletTime,
        { 2, 3, 4, 6, 8 }
        );

  auto commonTuplets
    = TupletRatioField(78, 5, 6, 20,
        fieldTupletCount, fieldTupletTime
        );

  auto fieldTupletUnit
    = BeatField(96, 2, 12, 28,
        userSettings().tupletUnit
        );

  auto pendingTupletIndicator
    = PendingIndicator(66, 0, pendingTuplet);


  auto fieldMemory
    = MemoryField(111, 5, 17, 23,
        userState().memoryIndex
        );

  auto pendingMemoryIndicator
    = PendingIndicator(111, 0, pendingMemory);


  const std::initializer_list<Field*> selectableFields =
    { &fieldNumberMeasures,
      &fieldBeatsPerMeasure,
      &commonTimeSignatures,
      &fieldBeatUnit,
      &fieldTupletCount,
      &commonTuplets,
      &fieldTupletTime,
      &fieldTupletUnit,
      &fieldMemory
    };

  int selectedFieldIndex = 0;
  void updateSelectedField(int dir) {
    selectedFieldIndex =
      constrain(selectedFieldIndex + dir, 0, selectableFields.size() - 1);
  }

  Field* selectedField() {    // "class" needed due to auto-prototype gen.
    return selectableFields.begin()[selectedFieldIndex];
  }

  enum SelectMode { selectNone, selectField, selectValue };

  SelectMode selectMode = selectNone;

  bool fullRedrawPending = false;
}

void resetSelection() {
  selectMode = selectNone;
  selectedField()->deselect();
}

void updateSelection(int dir) {
  switch (selectMode) {

    case selectNone:
      selectMode = selectField;
      selectedField()->select();
      break;

    case selectField:
      selectedField()->deselect();
      updateSelectedField(dir);
      selectedField()->select();
      break;

    case selectValue:
      selectedField()->update(dir);
      break;
  }
}

void clickSelection(ButtonState s) {
  switch (selectMode) {

    case selectNone:
      if (s == buttonUp || s == buttonUpLong) {
        selectMode = selectField;
        selectedField()->select();
      }
      break;

    case selectField:
      if (s == buttonUp || s == buttonUpLong) {
        selectMode = selectValue;
      }
      break;

    case selectValue:
      if (selectedField()->click(s)) {
        break;
      }
      if (s == buttonUp || s == buttonUpLong) {
        selectMode = selectField;
      }
      break;
  }
}

namespace {

  void drawBPM() {
    display.setRotation(3);
    display.fillRect(0, 0, 32, 14, BLACK);
    display.setTextColor(WHITE);
    centerNumber(bpm, 0, 0, 32, 14);
    display.setRotation(0);
  }

  void drawSeparator(int16_t x) {
    for (int16_t y = 0; y < 32; y += 2) {
      display.drawPixel(x, y, WHITE);
    }
  }

  void drawRepeat(bool refresh) {
    fieldNumberMeasures.render(refresh);
    fieldBeatsPerMeasure.render(refresh);
    commonTimeSignatures.render(refresh);
    fieldBeatUnit.render(refresh);

    pendingLoopIndicator.render(refresh);
  }

  void drawTuplet(bool refresh) {
    fieldTupletCount.render(refresh);
    commonTuplets.render(refresh);
    fieldTupletTime.render(refresh);
    fieldTupletUnit.render(refresh);

    pendingTupletIndicator.render(refresh);
  }

  void drawMemory(bool refresh) {
    fieldMemory.render(refresh);

    pendingMemoryIndicator.render(refresh);
  }

  void drawFixed() {
    // BPM area

    drawSeparator(16);

    // Repeat area
    //    the x
    display.drawLine(31, 12, 37, 18, WHITE);
    display.drawLine(31, 18, 37, 12, WHITE);

    drawSeparator(64);

    // Tuplet area

    drawSeparator(109);

    // Memory area
  }

}

void displayOutOfDate() {
  fullRedrawPending = true;
}

void drawAll(bool refresh) {
  display.dim(false);

  unsigned long t0 = millis();

  refresh |= fullRedrawPending;
  if (refresh) {
    fullRedrawPending = false;
    display.clearDisplay();

    resetText();
    drawFixed();
  }

  drawBPM();
  drawRepeat(refresh);
  drawTuplet(refresh);
  drawMemory(refresh);

  unsigned long t1 = millis();

  display.display();

  unsigned long t2 = millis();
  // Serial.print("draw ms: ");
  // Serial.print(t1 - t0);
  // Serial.print(", disp ms: ");
  // Serial.println(t2 - t1);
}
