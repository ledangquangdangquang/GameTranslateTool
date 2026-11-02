// TextMeshPro static SDF font assets (Unity, Addressables bundles): the atlas is baked once at build
// time and the engine never renders a glyph it doesn't already have (unlike Godot's Dynamic mode, which
// re-renders at runtime - see ttf.cpp). Adding Vietnamese means baking new single-channel SDF glyph
// bitmaps ourselves (GDI render -> exact 2D Euclidean distance transform -> the font's own encoding,
// read from its Material's `_GradientScale`) and appending them as new rows at the bottom of the
// existing atlas texture - every existing glyph's rect is left untouched, byte for byte.
#pragma once
#include <functional>

#include "bundle.h"
#include "serialized.h"

// Scans every MonoBehaviour object in `sf` for a static (m_AtlasPopulationMode == 0) TMP_FontAsset
// missing Vietnamese letters (kVietnamese, ttf.h) and bakes them in. The atlas texture's pixel bytes
// live in a sibling raw node of `bundle` (Unity's streamed "resS" convention); that node is grown by
// appending a full fresh copy of the (bigger) texture at its end and repointing the Texture2D object's
// m_StreamData at it, so nothing already in the node moves. Returns the number of font assets patched.
int tmp_patch_vietnamese(SerializedFile& sf, Bundle& bundle, const std::function<void(const std::string&)>& log);
