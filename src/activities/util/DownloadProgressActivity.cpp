#include "DownloadProgressActivity.h"

#include <GfxRenderer.h>

#include "config.h"

void DownloadProgressActivity::onEnter() {
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;

  const int screenWidth = GfxRenderer::getScreenWidth();
  const int screenHeight = GfxRenderer::getScreenHeight();

  renderer.clearScreen();

  // Draw message text near the top third of the screen.
  const int lineHeight = renderer.getLineHeight(UI_FONT_ID);
  const int textY = screenHeight / 3 - lineHeight / 2;
  renderer.drawCenteredText(UI_FONT_ID, textY, text.c_str(), true, REGULAR);

  // Draw a simple horizontal progress bar in the lower third.
  const int barWidth = screenWidth - 80;   // horizontal margins
  const int barHeight = 24;
  const int barX = (screenWidth - barWidth) / 2;
  const int barY = (2 * screenHeight) / 3 - barHeight / 2;

  // Outline
  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill according to progress (leave a 2px padding inside the outline).
  const int innerX = barX + 2;
  const int innerY = barY + 2;
  const int innerWidth = barWidth - 4;
  const int innerHeight = barHeight - 4;
  const int filledWidth = static_cast<int>(innerWidth * progress + 0.5f);

  if (filledWidth > 0) {
    renderer.fillRect(innerX, innerY, filledWidth, innerHeight);
  }

  renderer.displayBuffer(EInkDisplay::FAST_REFRESH);
}
