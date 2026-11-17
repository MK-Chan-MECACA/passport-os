// Xiaozhi MCP tools for Hacker News tech news (self.news.*).
#pragma once

#include <cstddef>
#include <string>

struct cJSON;

namespace technews {
size_t tool_count();
const char *tool_schema(size_t index);
// Returns an MCP result object, or empty for a tool owned by another module.
std::string call(const char *name, const cJSON *arguments);
}  // namespace technews
