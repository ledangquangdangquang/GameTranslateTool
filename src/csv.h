// Delimiter-agnostic RFC4180-ish table parser/writer shared by GameMaker's comma CSV
// (gamemaker.cpp) and Unity TextAsset tab-separated tables (formats.cpp "bigtsv" kind).
// A field with the delimiter, a quote or a line break is "quoted", "" inside it is a literal ".
// Parsing keeps each field's raw byte span in the source text, so a rewrite only replaces the
// cells it translates and everything else (quoting style included) stays byte-identical.
#pragma once
#include <string>
#include <vector>

#include "bytes.h"

struct CsvField { std::string value; size_t start, end; };  // [start,end): raw span in the source text

std::vector<std::vector<CsvField>> csv_parse(const std::string& t, char delim = ',');
size_t csv_bom(const std::string& t);
// Case-insensitive header lookup, searching every column (including column 0).
int csv_find_column(const std::vector<std::vector<CsvField>>& rows, const std::string& name);
std::string csv_encode(const std::string& v, char delim = ',');
