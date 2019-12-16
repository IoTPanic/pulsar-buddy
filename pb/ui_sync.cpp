#include "ui_sync.h"

#include "timer_hw.h"


bool SyncField::isOutOfDate() {
  if (modeAsDrawn != mode)
    return true;

  switch (mode) {
    case displayBPM:
      if (bpmAsDrawn != currentBpm())
        return true;
      break;

    case displaySync:
      if (syncAsDrawn != pendingSync)
        return true;
      break;
  }

  return Field::isOutOfDate();
}

void SyncField::enter(bool alternate) {
  mode = alternate ? displaySync : displayBPM;
  pendingSync = state.syncMode;
}

void SyncField::exit() {
  if (mode == displaySync) {
    state.syncMode = pendingSync;
    setSync(state.syncMode);
    mode = displayBPM;
  }
}

namespace SyncImages {
  const unsigned char broken[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x21, 0x00,
    0x21, 0x00, 0x21, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x40, 0x02, 0x80, 0x01, 0x00,
    0x02, 0x80, 0x04, 0x40, 0x00, 0x00, 0x01, 0x00, 0x01, 0x08, 0x01, 0x08, 0x01, 0x08, 0x01, 0xf8,
    0x01, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char paused[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x15, 0x00, 0x20, 0x00,
    0x01, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xc0, 0x06, 0xc0, 0x06, 0xc0,
    0x06, 0xc0, 0x06, 0xc0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x08, 0x01, 0x50,
    0x00, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char din24[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80, 0x0f, 0xe0, 0x1f, 0xf0,
    0x1f, 0xf0, 0x3f, 0xf8, 0x37, 0xd8, 0x3f, 0xf8, 0x1b, 0xb0, 0x1e, 0xf0, 0x0f, 0xe0, 0x03, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x1c, 0x18, 0x3e, 0x38, 0x46, 0x38, 0x06, 0x58, 0x0c, 0x98, 0x18, 0xfc,
    0x30, 0xfc, 0x7e, 0x18, 0x7e, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char din48[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80, 0x0f, 0xe0, 0x1f, 0xf0,
    0x1f, 0xf0, 0x3f, 0xf8, 0x37, 0xd8, 0x3f, 0xf8, 0x1b, 0xb0, 0x1e, 0xf0, 0x0f, 0xe0, 0x03, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x0c, 0x70, 0x1c, 0xd8, 0x1c, 0xd8, 0x2c, 0x70, 0x4c, 0xb8, 0x7e, 0xd8,
    0x7e, 0xd8, 0x0c, 0xd8, 0x0c, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char midi[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80, 0x0f, 0xe0, 0x1f, 0xf0,
    0x1f, 0xf0, 0x3f, 0xf8, 0x37, 0xd8, 0x3f, 0xf8, 0x1b, 0xb0, 0x1e, 0xf0, 0x0f, 0xe0, 0x03, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x30, 0x1c, 0x70, 0x1e, 0xf0, 0x1f, 0xf0, 0x1b, 0xb0, 0x19, 0x30,
    0x18, 0x30, 0x18, 0x30, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char clk32[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x21, 0x00,
    0x21, 0x00, 0x21, 0x08, 0x01, 0x08, 0x01, 0x08, 0x09, 0xf8, 0x19, 0xf8, 0x38, 0x00, 0x18, 0x00,
    0x18, 0x00, 0x19, 0x00, 0x1a, 0x00, 0x04, 0x00, 0x08, 0x00, 0x10, 0x00, 0x2e, 0x38, 0x13, 0x4c,
    0x03, 0x0c, 0x06, 0x18, 0x03, 0x30, 0x13, 0x64, 0x0e, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char clk16[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x21, 0x00,
    0x21, 0x00, 0x21, 0x08, 0x01, 0x08, 0x01, 0x08, 0x09, 0xf8, 0x19, 0xf8, 0x38, 0x00, 0x18, 0x00,
    0x18, 0x00, 0x19, 0x00, 0x1a, 0x00, 0x04, 0x00, 0x08, 0x00, 0x12, 0x08, 0x26, 0x30, 0x0e, 0x60,
    0x06, 0xf0, 0x06, 0xd8, 0x06, 0xd8, 0x06, 0xd8, 0x06, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char clk8[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x21, 0x00,
    0x21, 0x00, 0x21, 0x08, 0x01, 0x08, 0x01, 0x08, 0x09, 0xf8, 0x19, 0xf8, 0x38, 0x00, 0x18, 0x00,
    0x18, 0x00, 0x19, 0x00, 0x1a, 0x00, 0x04, 0x00, 0x08, 0x00, 0x13, 0xc0, 0x26, 0x60, 0x07, 0x60,
    0x07, 0xc0, 0x03, 0xe0, 0x06, 0xe0, 0x06, 0x60, 0x03, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char clk4[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x3f, 0x00, 0x21, 0x00,
    0x21, 0x00, 0x21, 0x08, 0x01, 0x08, 0x01, 0x08, 0x09, 0xf8, 0x19, 0xf8, 0x38, 0x00, 0x18, 0x00,
    0x18, 0x00, 0x19, 0x00, 0x1a, 0x00, 0x04, 0x00, 0x08, 0x40, 0x10, 0xc0, 0x21, 0xc0, 0x02, 0xc0,
    0x04, 0xc0, 0x07, 0xe0, 0x07, 0xe0, 0x00, 0xc0, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned char fixed[] = { // 15 x 32
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x80, 0x02, 0x84, 0x02, 0x98, 0x05, 0x58, 0x04, 0x60,
    0x05, 0x40, 0x08, 0xa0, 0x09, 0x20, 0x08, 0x20, 0x1d, 0x70, 0x1f, 0xf0, 0x1f, 0xf0, 0x1f, 0xf0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
}

namespace {
  struct SyncInfo {
    SyncMode              mode;
    const unsigned char * image;
  };

  SyncInfo syncOptions[] = {
    { syncFixed,  SyncImages::fixed },
    { sync1ppqn,  SyncImages::clk4 },
    { sync2ppqn,  SyncImages::clk8 },
    { sync4ppqn,  SyncImages::clk16 },
    { sync8ppqn,  SyncImages::clk32 },
    { sync24ppqn, SyncImages::din24 },
    { sync48ppqn, SyncImages::din48 },
    // { syncMidi,   SyncImages::midi },
  };

  const int numSyncOptions = sizeof(syncOptions) / sizeof(syncOptions[0]);

  int findSyncModeIndex(SyncMode sync) {
    for (int i = 0; i < numSyncOptions; ++i) {
      if (sync == syncOptions[i].mode)
        return i;
    }
    return -1;
  }
}

void SyncField::update(Encoder::Update update) {
  switch (mode) {
    case displayBPM:
      setBpm(currentBpm() + update.accel(5));
      break;

    case displaySync:
      int i = findSyncModeIndex(pendingSync);
      int j = constrain(i + update.dir(), 0, numSyncOptions - 1);
      pendingSync = syncOptions[j].mode;
      break;
  }
}

void SyncField::redraw() {

  display.setTextColor(foreColor());

  switch (mode) {
    case displayBPM: {
      bpmAsDrawn = currentBpm();

      // rotated coordinates of the field
      const int16_t xr = display.height() - (y + h);
      const int16_t yr = x;
      const int16_t wr = h;
      const int16_t hr = w;

      display.setRotation(3);
      centerNumber(bpmAsDrawn, xr, yr, wr, hr);
      display.setRotation(0);

      break;
    }

    case displaySync:
      int i = findSyncModeIndex(pendingSync);
      if (i >= 0)
        display.drawBitmap(x, y, syncOptions[i].image, 15, 32, foreColor());
      else
        centerText("?", x, y, w, h);
      syncAsDrawn = pendingSync;
      break;
  }

  modeAsDrawn = mode;
}
