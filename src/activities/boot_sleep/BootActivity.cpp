#include "BootActivity.h"

#include <GfxRenderer.h>

#include "config.h"

void BootActivity::onEnter() {
  const auto pageHeight = GfxRenderer::getScreenHeight();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_FONT_ID, pageHeight / 2, "Rebooting...", true, BOLD);
  renderer.displayBuffer();
}
