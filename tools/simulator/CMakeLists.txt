cmake_minimum_required(VERSION 3.16)
project(crosspoint_simulator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(crosspoint-sim
  src/main.cpp

  # Core libs
  ../../lib/Utf8/Utf8.cpp
  ../../lib/EpdFont/EpdFont.cpp
  ../../lib/EpdFont/EpdFontFamily.cpp
  ../../lib/GfxRenderer/GfxRenderer.cpp

  # EPUB + parsing
  ../../lib/ZipFile/ZipFile.cpp
  ../../lib/Epub/Epub.cpp
  ../../lib/Epub/Epub/FsHelpers.cpp
  ../../lib/Epub/Epub/Page.cpp
  ../../lib/Epub/Epub/ParsedText.cpp
  ../../lib/Epub/Epub/htmlEntities.cpp
  ../../lib/Epub/Epub/blocks/TextBlock.cpp
  ../../lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ../../lib/Epub/Epub/parsers/ContainerParser.cpp
  ../../lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ../../lib/Epub/Epub/parsers/TocNcxParser.cpp

  # Third party
  ../../lib/miniz/miniz.c
  ../../lib/expat/xmlparse.c
  ../../lib/expat/xmlrole.c
  ../../lib/expat/xmltok.c
  ../../lib/expat/xmltok_ns.c
  ../../lib/expat/xmltok_impl.c
)

target_compile_definitions(crosspoint-sim PRIVATE
  CROSSPOINT_HOST=1
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_include_directories(crosspoint-sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party

  ../../lib/Utf8
  ../../lib/EpdFont
  ../../lib/GfxRenderer
  ../../lib/ZipFile
  ../../lib/Serialization
  ../../lib/Epub
  ../../lib/miniz
  ../../lib/expat
  ../../src
)

if (APPLE)
  target_compile_options(crosspoint-sim PRIVATE -Wno-deprecated-declarations)
endif()
