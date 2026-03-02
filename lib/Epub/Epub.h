#pragma once
#include <Print.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/EpubTocEntry.h"

class ZipFile;

class Epub {
  // the title read from the EPUB meta data
  std::string title;
  // the cover image (original cover image item from manifest)
  std::string coverImageItem;
  // Optional 2bpp thumbnail stored inside the EPUB for Crosspoint navigation.
  std::string thumbnail2bppItem;
  // the ncx file
  std::string tocNcxItem;
  // where is the EPUBfile?
  std::string filepath;
  // the spine of the EPUB file
  std::vector<std::pair<std::string, std::string>> spine;
  // the file size of the spine items (proxy to book progress)
  std::vector<size_t> cumulativeSpineItemSize;
  // the toc of the EPUB file
  std::vector<EpubTocEntry> toc;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(const std::string& contentOpfFilePath);
  bool parseTocNcxFile();
  void initializeSpineItemSizes();

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load();
  // Lightweight version of load() that only parses container.xml and content.opf
  // (title, cover, thumbnail, manifest, spine refs) and skips TOC and spine size
  // initialization. Useful for metadata-only operations like thumbnails.
  bool loadMetadataOnly();
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getCoverImageItem() const;
  const std::string& getThumbnail2bppItem() const { return thumbnail2bppItem; }
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  std::string& getSpineItem(int spineIndex);
  int getSpineItemsCount() const;
  size_t getCumulativeSpineItemSize(const int spineIndex) const;
  EpubTocEntry& getTocItem(int tocIndex);
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  // Generate a readable chapter name from the spine item's filename when no TOC entry exists
  std::string getFallbackChapterName(int spineIndex) const;

  size_t getBookSize() const;
  uint8_t calculateProgress(const int currentSpineIndex, const float currentSpineRead);
};
