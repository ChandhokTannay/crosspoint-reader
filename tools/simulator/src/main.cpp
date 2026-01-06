#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <builtinFonts/bookerly_2b.h>
#include <builtinFonts/bookerly_bold_2b.h>
#include <builtinFonts/bookerly_bold_italic_2b.h>
#include <builtinFonts/bookerly_italic_2b.h>
#include <builtinFonts/pixelarial14.h>
#include <builtinFonts/ubuntu_10.h>
#include <builtinFonts/ubuntu_bold_10.h>

#include <Epub/parsers/ChapterHtmlSlimParser.h>

#include <config.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr float kLineCompression = 0.95f;
constexpr int kMarginTop = 8;
constexpr int kMarginRight = 10;
constexpr int kMarginBottom = 22;
constexpr int kMarginLeft = 10;

struct Options {
  std::string epubPath;
  int spineIndex = 0;
  int pageIndex = 0;
  bool landscape = false;
  bool extraParagraphSpacing = true;
  bool listSpine = false;
  bool listToc = false;
  std::string outPath = "out.png";
};

void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s <book.epub> [--spine N] [--page N] [--landscape] [--out out.png] [--list-spine] [--list-toc]\n"
               "  --spine N              Spine item index (default: 0)\n"
               "  --page N               Page index within the spine item after pagination (default: 0)\n"
               "  --landscape            Render in horizontal/landscape mode\n"
               "  --extra-paragraph-spacing 0|1 (default: 1)\n"
               "  --list-spine            Print spine items and exit\n"
               "  --list-toc              Print table of contents and exit\n"
               "  --out PATH              Output PNG path (default: out.png)\n",
               argv0);
}

bool parseArgs(int argc, char** argv, Options* opts) {
  if (argc < 2) {
    return false;
  }

  opts->epubPath = argv[1];

  for (int i = 2; i < argc; i++) {
    const std::string arg = argv[i];

    if (arg == "--spine" && i + 1 < argc) {
      opts->spineIndex = std::atoi(argv[++i]);
    } else if (arg == "--page" && i + 1 < argc) {
      opts->pageIndex = std::atoi(argv[++i]);
    } else if (arg == "--landscape") {
      opts->landscape = true;
    } else if (arg == "--out" && i + 1 < argc) {
      opts->outPath = argv[++i];
    } else if (arg == "--list-spine") {
      opts->listSpine = true;
    } else if (arg == "--list-toc") {
      opts->listToc = true;
    } else if (arg == "--extra-paragraph-spacing" && i + 1 < argc) {
      opts->extraParagraphSpacing = std::atoi(argv[++i]) != 0;
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
      return false;
    }
  }

  return true;
}

bool getNativePixel(const uint8_t* buffer, const int x, const int y) {
  if (x < 0 || x >= EInkDisplay::DISPLAY_WIDTH || y < 0 || y >= EInkDisplay::DISPLAY_HEIGHT) {
    return false;
  }

  const int byteIndex = y * EInkDisplay::DISPLAY_WIDTH_BYTES + (x / 8);
  const uint8_t bitPosition = 7 - (x % 8);
  const bool isWhite = ((buffer[byteIndex] >> bitPosition) & 0x1) != 0;
  return !isWhite;  // black?
}

bool writePngFromFrameBuffer(const uint8_t* nativeFrameBuffer, const GfxRenderer& renderer, const std::string& outPath) {
  const bool portrait = renderer.getOrientation() == GfxRenderer::PORTRAIT;

  const int outW = portrait ? renderer.getScreenWidth() : EInkDisplay::DISPLAY_WIDTH;
  const int outH = portrait ? renderer.getScreenHeight() : EInkDisplay::DISPLAY_HEIGHT;

  std::vector<uint8_t> rgb(outW * outH * 3);

  for (int y = 0; y < outH; y++) {
    for (int x = 0; x < outW; x++) {
      int nx = x;
      int ny = y;

      if (portrait) {
        // In portrait mode, GfxRenderer rotates portrait coords to native coords:
        // nativeX = y
        // nativeY = DISPLAY_HEIGHT - 1 - x
        nx = y;
        ny = EInkDisplay::DISPLAY_HEIGHT - 1 - x;
      }

      const bool black = getNativePixel(nativeFrameBuffer, nx, ny);
      const uint8_t v = black ? 0 : 255;

      const size_t idx = (static_cast<size_t>(y) * outW + x) * 3;
      rgb[idx + 0] = v;
      rgb[idx + 1] = v;
      rgb[idx + 2] = v;
    }
  }

  if (!stbi_write_png(outPath.c_str(), outW, outH, 3, rgb.data(), outW * 3)) {
    std::fprintf(stderr, "Failed to write PNG: %s\n", outPath.c_str());
    return false;
  }

  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parseArgs(argc, argv, &opts)) {
    usage(argv[0]);
    return 2;
  }

  // Setup rendering stack
  EInkDisplay display;
  GfxRenderer renderer(display);
  renderer.setOrientation(opts.landscape ? GfxRenderer::LANDSCAPE : GfxRenderer::PORTRAIT);

  // Fonts (match firmware)
  EpdFont bookerlyFont(&bookerly_2b);
  EpdFont bookerlyBoldFont(&bookerly_bold_2b);
  EpdFont bookerlyItalicFont(&bookerly_italic_2b);
  EpdFont bookerlyBoldItalicFont(&bookerly_bold_italic_2b);
  EpdFontFamily bookerlyFontFamily(&bookerlyFont, &bookerlyBoldFont, &bookerlyItalicFont, &bookerlyBoldItalicFont);

  EpdFont smallFont(&pixelarial14);
  EpdFontFamily smallFontFamily(&smallFont);

  EpdFont ubuntu10Font(&ubuntu_10);
  EpdFont ubuntuBold10Font(&ubuntu_bold_10);
  EpdFontFamily ubuntuFontFamily(&ubuntu10Font, &ubuntuBold10Font);

  renderer.insertFont(READER_FONT_ID, bookerlyFontFamily);
  renderer.insertFont(UI_FONT_ID, ubuntuFontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Load EPUB (ZipFile expects a filesystem path)
  Epub epub(opts.epubPath, ".crosspoint_host_cache");
  if (!epub.load()) {
    std::fprintf(stderr, "Failed to load EPUB: %s\n", opts.epubPath.c_str());
    return 1;
  }

  if (opts.listSpine) {
    const int count = epub.getSpineItemsCount();
    for (int i = 0; i < count; i++) {
      std::fprintf(stdout, "%d\t%s\n", i, epub.getSpineItem(i).c_str());
    }
    return 0;
  }

  if (opts.listToc) {
    const int count = epub.getTocItemsCount();
    for (int i = 0; i < count; i++) {
      const auto& t = epub.getTocItem(i);
      std::fprintf(stdout, "%d\t(level %d)\t%s\t%s\n", i, t.level, t.title.c_str(), t.href.c_str());
    }
    return 0;
  }

  if (opts.spineIndex < 0 || opts.spineIndex >= epub.getSpineItemsCount()) {
    std::fprintf(stderr, "Invalid spine index %d (max %d)\n", opts.spineIndex, epub.getSpineItemsCount() - 1);
    return 1;
  }

  const std::string spineItem = epub.getSpineItem(opts.spineIndex);

  size_t htmlSize = 0;
  uint8_t* htmlBytes = epub.readItemContentsToBytes(spineItem, &htmlSize, true);
  if (!htmlBytes) {
    std::fprintf(stderr, "Failed to read spine item: %s\n", spineItem.c_str());
    return 1;
  }

  // Write out to a temp file (ChapterHtmlSlimParser reads from FILE*)
  const auto tmpPath = (std::filesystem::temp_directory_path() / "crosspoint_preview.xhtml").string();
  {
    std::ofstream out(tmpPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(htmlBytes), static_cast<std::streamsize>(htmlSize));
  }
  free(htmlBytes);

  std::vector<std::unique_ptr<Page>> pages;

  ChapterHtmlSlimParser parser(tmpPath.c_str(), renderer, READER_FONT_ID, kLineCompression, kMarginTop, kMarginRight,
                              kMarginBottom, kMarginLeft, opts.extraParagraphSpacing,
                              [&pages](std::unique_ptr<Page> page) {
                                if (page) {
                                  pages.push_back(std::move(page));
                                }
                              });

  if (!parser.parseAndBuildPages()) {
    std::fprintf(stderr, "Failed to parse and paginate chapter HTML\n");
    return 1;
  }

  if (pages.empty()) {
    std::fprintf(stderr, "No pages produced (empty chapter?)\n");
    return 1;
  }

  if (opts.pageIndex < 0 || opts.pageIndex >= static_cast<int>(pages.size())) {
    std::fprintf(stderr, "Invalid page index %d (max %zu)\n", opts.pageIndex, pages.size() - 1);
    return 1;
  }

  renderer.clearScreen();
  pages[opts.pageIndex]->render(renderer, READER_FONT_ID);

  if (!writePngFromFrameBuffer(display.getFrameBuffer(), renderer, opts.outPath)) {
    return 1;
  }

  std::fprintf(stdout, "Wrote %s (%s, spine=%d, page=%d, pages=%zu)\n", opts.outPath.c_str(),
               opts.landscape ? "landscape" : "portrait", opts.spineIndex, opts.pageIndex, pages.size());

  return 0;
}
