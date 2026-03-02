#!/usr/bin/env python3
"""
EPUB TOC Splitter - Split EPUB files based on existing TOC anchors.

This script:
1. Reads the existing TOC (toc.ncx) from an EPUB
2. Finds anchor positions in HTML files
3. Splits HTML files at those anchor points
4. Creates one file per TOC entry
5. Regenerates a clean EPUB with proper file structure

Usage:
    python epub_toc_builder.py input.epub              # Process EPUB using its TOC
    python epub_toc_builder.py input.epub -o out.epub  # Specify output filename
    python epub_toc_builder.py input.epub --dry-run    # Preview without creating files
"""

import argparse
import html
import re
import shutil
import uuid
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional
from urllib.parse import unquote
from xml.etree import ElementTree as ET

try:
    from bs4 import BeautifulSoup
    HAS_BS4 = True
except ImportError:
    HAS_BS4 = False


@dataclass
class TocEntry:
    """Represents a TOC entry from the NCX file."""
    title: str
    href: str  # filename without anchor
    anchor: str  # anchor/fragment (without #)
    play_order: int
    level: int = 1


@dataclass
class Chapter:
    """Represents a chapter in the output EPUB."""
    title: str
    filename: str
    content: str
    order: int


@dataclass
class BookMetadata:
    """Book metadata for EPUB generation."""
    title: str = "Untitled Book"
    author: str = "Unknown Author"
    language: str = "en"
    identifier: str = field(default_factory=lambda: str(uuid.uuid4()))


def extract_epub(epub_path: Path, output_dir: Path) -> Path:
    """Extract EPUB to a directory."""
    output_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(epub_path, "r") as zf:
        zf.extractall(output_dir)
    return output_dir


def find_content_opf(extracted_dir: Path) -> Optional[Path]:
    """Find the content.opf file in an extracted EPUB."""
    container_path = extracted_dir / "META-INF" / "container.xml"
    if container_path.exists():
        tree = ET.parse(container_path)
        root = tree.getroot()
        ns = {"c": "urn:oasis:names:tc:opendocument:xmlns:container"}
        rootfile = root.find(".//c:rootfile", ns)
        if rootfile is not None:
            opf_path = rootfile.get("full-path")
            if opf_path:
                return extracted_dir / opf_path
    # Fallback: search for content.opf
    for opf in extracted_dir.rglob("*.opf"):
        return opf
    return None


def find_toc_ncx(opf_path: Path) -> Optional[Path]:
    """Find the toc.ncx file referenced in content.opf."""
    tree = ET.parse(opf_path)
    root = tree.getroot()
    
    ns = {"opf": "http://www.idpf.org/2007/opf"}
    
    # Look for NCX in manifest
    manifest = root.find(".//opf:manifest", ns) or root.find(".//{http://www.idpf.org/2007/opf}manifest")
    if manifest is not None:
        for item in manifest:
            media_type = item.get("media-type", "")
            if "ncx" in media_type or item.get("id") == "ncx":
                href = item.get("href")
                if href:
                    return opf_path.parent / href
    
    # Fallback: look for toc.ncx in same directory
    ncx_path = opf_path.parent / "toc.ncx"
    if ncx_path.exists():
        return ncx_path
    
    # Search for any .ncx file
    for ncx in opf_path.parent.rglob("*.ncx"):
        return ncx
    
    return None


def parse_toc_ncx(ncx_path: Path, base_path: Path) -> list[TocEntry]:
    """Parse toc.ncx to extract TOC entries with anchors."""
    tree = ET.parse(ncx_path)
    root = tree.getroot()
    
    ns = {"ncx": "http://www.daisy.org/z3986/2005/ncx/"}
    
    entries = []
    
    def parse_nav_point(nav_point, level=1):
        # Get label text
        label = nav_point.find("ncx:navLabel/ncx:text", ns)
        if label is None:
            label = nav_point.find(".//{http://www.daisy.org/z3986/2005/ncx/}navLabel/{http://www.daisy.org/z3986/2005/ncx/}text")
        
        title = label.text.strip() if label is not None and label.text else "Untitled"
        
        # Get content src
        content = nav_point.find("ncx:content", ns)
        if content is None:
            content = nav_point.find(".//{http://www.daisy.org/z3986/2005/ncx/}content")
        
        if content is not None:
            src = content.get("src", "")
            src = unquote(src)  # URL decode
            
            # Split href and anchor
            if "#" in src:
                href, anchor = src.split("#", 1)
            else:
                href, anchor = src, ""
            
            play_order = int(nav_point.get("playOrder", len(entries) + 1))
            
            entries.append(TocEntry(
                title=title,
                href=href,
                anchor=anchor,
                play_order=play_order,
                level=level,
            ))
        
        # Process nested navPoints
        for child in nav_point.findall("ncx:navPoint", ns):
            parse_nav_point(child, level + 1)
        for child in nav_point.findall(".//{http://www.daisy.org/z3986/2005/ncx/}navPoint"):
            if child.getparent() == nav_point:  # Only direct children
                parse_nav_point(child, level + 1)
    
    # Find all top-level navPoints
    nav_map = root.find("ncx:navMap", ns) or root.find(".//{http://www.daisy.org/z3986/2005/ncx/}navMap")
    if nav_map is not None:
        for nav_point in nav_map.findall("ncx:navPoint", ns):
            parse_nav_point(nav_point)
        if not entries:  # Try without namespace
            for nav_point in nav_map.findall(".//{http://www.daisy.org/z3986/2005/ncx/}navPoint"):
                parse_nav_point(nav_point)
    
    return sorted(entries, key=lambda e: e.play_order)


def parse_content_opf(opf_path: Path) -> BookMetadata:
    """Parse content.opf to get metadata."""
    tree = ET.parse(opf_path)
    root = tree.getroot()
    
    ns = {
        "opf": "http://www.idpf.org/2007/opf",
        "dc": "http://purl.org/dc/elements/1.1/",
    }
    
    metadata = BookMetadata()
    
    title_el = root.find(".//dc:title", ns)
    if title_el is not None and title_el.text:
        metadata.title = title_el.text
    
    author_el = root.find(".//dc:creator", ns)
    if author_el is not None and author_el.text:
        metadata.author = author_el.text
    
    lang_el = root.find(".//dc:language", ns)
    if lang_el is not None and lang_el.text:
        metadata.language = lang_el.text
    
    id_el = root.find(".//dc:identifier", ns)
    if id_el is not None and id_el.text:
        metadata.identifier = id_el.text
    
    return metadata


def find_anchor_position(html_content: str, anchor: str) -> int:
    """Find the position of an anchor in HTML content."""
    if not anchor:
        return 0
    
    # Look for id="anchor" or name="anchor"
    patterns = [
        rf'id\s*=\s*["\']?{re.escape(anchor)}["\']?',
        rf'name\s*=\s*["\']?{re.escape(anchor)}["\']?',
        rf'<a[^>]*name\s*=\s*["\']?{re.escape(anchor)}["\']?',
    ]
    
    for pattern in patterns:
        match = re.search(pattern, html_content, re.IGNORECASE)
        if match:
            # Find the start of the containing tag
            tag_start = html_content.rfind("<", 0, match.start())
            if tag_start != -1:
                return tag_start
            return match.start()
    
    return -1


def extract_head_content(html_content: str) -> str:
    """Extract the <head> section from HTML."""
    match = re.search(r"<head[^>]*>.*?</head>", html_content, re.DOTALL | re.IGNORECASE)
    if match:
        return match.group(0)
    return '<head><meta charset="utf-8"/></head>'


def extract_body_content(html_content: str) -> str:
    """Extract content inside <body> tags."""
    match = re.search(r"<body[^>]*>(.*?)</body>", html_content, re.DOTALL | re.IGNORECASE)
    if match:
        return match.group(1)
    return html_content


def clean_html_fragment(content: str) -> str:
    """
    Clean up an HTML fragment to fix unclosed/mismatched tags.
    Uses BeautifulSoup if available, otherwise does basic regex cleanup.
    """
    if HAS_BS4:
        # Parse and re-serialize to fix tag issues
        soup = BeautifulSoup(content, 'html.parser')
        return str(soup)
    else:
        # Basic cleanup: remove orphan closing tags at the start
        # and unclosed opening tags at the end
        content = re.sub(r'^\s*</[^>]+>', '', content)  # Remove leading closing tags
        
        # Try to balance common tags
        for tag in ['h1', 'h2', 'h3', 'h4', 'p', 'div', 'span']:
            opens = len(re.findall(rf'<{tag}[\s>]', content, re.IGNORECASE))
            closes = len(re.findall(rf'</{tag}>', content, re.IGNORECASE))
            
            # Add missing closing tags at end
            if opens > closes:
                content += f'</{tag}>' * (opens - closes)
            # Remove extra closing tags from start
            elif closes > opens:
                for _ in range(closes - opens):
                    content = re.sub(rf'^(\s*)</{tag}>', r'\1', content, count=1, flags=re.IGNORECASE)
        
        return content


def wrap_in_xhtml(content: str, head: str, title: str) -> str:
    """Wrap content in proper XHTML structure."""
    # Clean up the content to fix tag mismatches
    content = clean_html_fragment(content)
    
    # Update title in head if present
    if "<title>" in head:
        head = re.sub(r"<title>.*?</title>", f"<title>{html.escape(title)}</title>", head, flags=re.IGNORECASE)
    
    # Add CSS for bold headings if not already present
    bold_style = """<style type="text/css">
    h1, h2, h3, h4 { font-weight: bold; }
  </style>"""
    
    # Insert style before </head> if head exists
    if "</head>" in head.lower():
        head = re.sub(r"</head>", f"{bold_style}\n</head>", head, flags=re.IGNORECASE)
    else:
        head = f'<head><meta charset="utf-8"/><title>{html.escape(title)}</title>{bold_style}</head>'
    
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
{head}
<body>
{content}
</body>
</html>"""


def split_html_by_toc(html_content: str, toc_entries: list[TocEntry], html_filename: str) -> list[tuple[TocEntry, str]]:
    """
    Split HTML content based on TOC anchor positions.
    Returns list of (TocEntry, content) tuples.
    """
    # Filter TOC entries for this file
    file_entries = [e for e in toc_entries if e.href == html_filename]
    
    if not file_entries:
        return []
    
    # Find positions of each anchor
    positions = []
    for entry in file_entries:
        pos = find_anchor_position(html_content, entry.anchor)
        if pos == -1:
            pos = 0 if not positions else positions[-1][0]  # Default to start or last position
        positions.append((pos, entry))
    
    # Sort by position
    positions.sort(key=lambda x: x[0])
    
    # Extract content between positions
    body_content = extract_body_content(html_content)
    head_content = extract_head_content(html_content)
    
    results = []
    for i, (pos, entry) in enumerate(positions):
        # Adjust position relative to body content
        body_start = html_content.find(body_content)
        if body_start == -1:
            body_start = 0
        
        rel_pos = max(0, pos - body_start)
        
        if i + 1 < len(positions):
            next_pos = positions[i + 1][0] - body_start
            content = body_content[rel_pos:next_pos]
        else:
            content = body_content[rel_pos:]
        
        # Clean up content
        content = content.strip()
        
        if content:
            full_html = wrap_in_xhtml(content, head_content, entry.title)
            results.append((entry, full_html))
    
    return results


def generate_toc_ncx(chapters: list[Chapter], metadata: BookMetadata) -> str:
    """Generate toc.ncx content."""
    nav_points = ""
    for i, ch in enumerate(chapters):
        safe_title = html.escape(ch.title)
        nav_points += f"""
    <navPoint id="navpoint-{i + 1}" playOrder="{i + 1}">
      <navLabel>
        <text>{safe_title}</text>
      </navLabel>
      <content src="{ch.filename}"/>
    </navPoint>"""
    
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="{html.escape(metadata.identifier)}"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle>
    <text>{html.escape(metadata.title)}</text>
  </docTitle>
  <navMap>{nav_points}
  </navMap>
</ncx>"""


def generate_content_opf(chapters: list[Chapter], metadata: BookMetadata) -> str:
    """Generate content.opf content."""
    manifest_items = '    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>\n'
    spine_items = ""
    
    for i, ch in enumerate(chapters):
        item_id = f"chapter{i + 1}"
        manifest_items += f'    <item id="{item_id}" href="{ch.filename}" media-type="application/xhtml+xml"/>\n'
        spine_items += f'    <itemref idref="{item_id}"/>\n'
    
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:identifier id="BookId">{html.escape(metadata.identifier)}</dc:identifier>
    <dc:title>{html.escape(metadata.title)}</dc:title>
    <dc:creator>{html.escape(metadata.author)}</dc:creator>
    <dc:language>{metadata.language}</dc:language>
  </metadata>
  <manifest>
{manifest_items}  </manifest>
  <spine toc="ncx">
{spine_items}  </spine>
</package>"""


def generate_container_xml() -> str:
    """Generate META-INF/container.xml content."""
    return """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>"""


def create_epub(chapters: list[Chapter], metadata: BookMetadata, output_path: Path):
    """Create an EPUB file from chapters."""
    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # mimetype must be first and uncompressed
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        
        # META-INF/container.xml
        zf.writestr("META-INF/container.xml", generate_container_xml())
        
        # OEBPS/content.opf
        zf.writestr("OEBPS/content.opf", generate_content_opf(chapters, metadata))
        
        # OEBPS/toc.ncx
        zf.writestr("OEBPS/toc.ncx", generate_toc_ncx(chapters, metadata))
        
        # Chapter files
        for ch in chapters:
            zf.writestr(f"OEBPS/{ch.filename}", ch.content)
    
    print(f"\nCreated: {output_path}")


def make_safe_filename(title: str, order: int) -> str:
    """Generate a safe filename from a chapter title."""
    # Remove unsafe characters
    safe = re.sub(r"[^\w\s-]", "", title).strip()
    safe = re.sub(r"\s+", "_", safe).lower()
    
    if not safe:
        safe = f"chapter_{order:03d}"
    else:
        # Prefix with order to maintain sequence
        safe = f"{order:03d}_{safe}"
    
    # Truncate if too long
    if len(safe) > 50:
        safe = safe[:50]
    
    return f"{safe}.xhtml"


def process_epub(epub_path: Path, output_path: Path, dry_run: bool = False):
    """Process an EPUB, split by TOC, and regenerate with proper file structure."""
    print(f"Processing: {epub_path}")
    
    # Create temp directory for extraction
    temp_dir = Path(f"/tmp/epub_toc_splitter_{uuid.uuid4().hex[:8]}")
    
    try:
        extract_epub(epub_path, temp_dir)
        
        opf_path = find_content_opf(temp_dir)
        if not opf_path:
            print("Error: Could not find content.opf")
            return
        
        ncx_path = find_toc_ncx(opf_path)
        if not ncx_path:
            print("Error: Could not find toc.ncx - this EPUB has no TOC to use")
            return
        
        metadata = parse_content_opf(opf_path)
        print(f"Book: {metadata.title} by {metadata.author}")
        
        toc_entries = parse_toc_ncx(ncx_path, opf_path.parent)
        print(f"Found {len(toc_entries)} TOC entries")
        
        if not toc_entries:
            print("Error: No TOC entries found")
            return
        
        # Group TOC entries by file
        files_to_process = {}
        for entry in toc_entries:
            if entry.href not in files_to_process:
                files_to_process[entry.href] = []
            files_to_process[entry.href].append(entry)
        
        print(f"\nTOC spans {len(files_to_process)} HTML file(s):")
        for href, entries in files_to_process.items():
            print(f"  {href}: {len(entries)} chapter(s)")
            for e in entries:
                anchor_info = f" #{e.anchor}" if e.anchor else ""
                print(f"    - {e.title}{anchor_info}")
        
        if dry_run:
            print("\n[Dry run - no files created]")
            return
        
        # Process each file and split by TOC
        all_chapters: list[Chapter] = []
        chapter_order = 0
        
        for href, entries in files_to_process.items():
            html_path = opf_path.parent / href
            if not html_path.exists():
                print(f"\nWarning: {href} not found, skipping")
                continue
            
            with open(html_path, "r", encoding="utf-8", errors="ignore") as f:
                html_content = f.read()
            
            if len(entries) == 1 and not entries[0].anchor:
                # Single entry without anchor - use whole file
                entry = entries[0]
                filename = make_safe_filename(entry.title, chapter_order + 1)
                
                head = extract_head_content(html_content)
                body = extract_body_content(html_content)
                content = wrap_in_xhtml(body, head, entry.title)
                
                all_chapters.append(Chapter(
                    title=entry.title,
                    filename=filename,
                    content=content,
                    order=chapter_order,
                ))
                chapter_order += 1
            else:
                # Multiple entries or anchored - split the file
                splits = split_html_by_toc(html_content, entries, href)
                
                for entry, content in splits:
                    filename = make_safe_filename(entry.title, chapter_order + 1)
                    
                    all_chapters.append(Chapter(
                        title=entry.title,
                        filename=filename,
                        content=content,
                        order=chapter_order,
                    ))
                    chapter_order += 1
        
        print(f"\nCreating {len(all_chapters)} chapter files...")
        for ch in all_chapters:
            print(f"  {ch.filename}: {ch.title}")
        
        create_epub(all_chapters, metadata, output_path)
        
    finally:
        if temp_dir.exists():
            shutil.rmtree(temp_dir)


def main():
    parser = argparse.ArgumentParser(
        description="Split EPUB files based on existing TOC anchors.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
This script reads the existing table of contents from an EPUB and splits
HTML files at anchor points to create one file per chapter.

Examples:
  %(prog)s input.epub                  Process EPUB using its TOC
  %(prog)s input.epub -o output.epub   Specify output filename  
  %(prog)s input.epub --dry-run        Preview TOC structure without creating files
        """,
    )
    
    parser.add_argument("input", help="Input EPUB file")
    parser.add_argument("-o", "--output", type=Path, help="Output EPUB path")
    parser.add_argument("--dry-run", "-n", action="store_true", 
                        help="Preview TOC structure without creating files")
    
    args = parser.parse_args()
    
    input_path = Path(args.input)
    if not input_path.exists():
        print(f"Error: {input_path} not found")
        return
    
    output_path = args.output or input_path.with_stem(f"{input_path.stem}_split")
    process_epub(input_path, output_path, args.dry_run)


if __name__ == "__main__":
    main()
