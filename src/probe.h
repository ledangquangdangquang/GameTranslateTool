// Diagnostic for when `scan` reports 0 sources and the real text location is unknown: walks every
// file in a folder (not just the usual candidate list) looking for runs of English-like text.
#pragma once
#include <functional>

#include "bytes.h"

void probe_dir(const fs::path& dir, int topN, const std::function<void(const std::string&)>& log);
