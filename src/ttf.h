// TrueType (glyf) fonts that lack Vietnamese letters: the missing letters are added as composite glyphs
// built from the font's own base letters and marks, placed with its GPOS mark-to-base anchors (a tone
// mark over a circumflex/breve is stacked above it instead of sharing its anchor). Marks the font
// doesn't have (hook above, horn) are traced from a Windows font through GDI; a missing or broken dot
// below is the font's own period, scaled down. Existing glyphs keep their indices.
#pragma once
#include <functional>
#include <string>

#include "bytes.h"

// Every Vietnamese letter that needs more than plain ASCII (134: lower/upper, base + all tone marks),
// as a flat, already-precomposed list - every one of these exists as a single Unicode code point that a
// Vietnamese-capable Windows font (Cambria, Calibri, Segoe UI, Tahoma, Times New Roman...) can render
// directly via GDI, no glyph composition needed. Shared by the GameMaker bitmap-font patcher and the
// Unity TMP SDF atlas patcher.
extern const char32_t kVietnamese[];

// Number of Vietnamese letters (of the 134 that need more than ASCII) the font lacks; -1 = not a
// TrueType font with Latin letters (CFF outlines, symbol / CJK / Thai fonts).
int ttf_missing_vietnamese(const Bytes& ttf);

// Adds the missing letters. Returns false (ttf untouched) when nothing is missing or the font can't be
// patched; `log` gets one line per font.
bool ttf_add_vietnamese(Bytes& ttf, const std::function<void(const std::string&)>& log);
