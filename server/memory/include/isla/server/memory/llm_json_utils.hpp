#pragma once

#include <string>
#include <string_view>

namespace isla::server::memory {

// Attempts to strip markdown code fences from an LLM response so the inner
// content can be parsed as JSON.  If the text is not wrapped in fences it is
// returned unchanged (the original string, including any surrounding whitespace).
// Surrounding whitespace is only consumed when detecting and stripping fences.
//
// Recognised patterns (the language tag, if any, is discarded):
//   ```json\n ... \n```
//   ```\n ... \n```
//   ``` ... ```          (single-line)
[[nodiscard]] std::string StripMarkdownCodeFences(std::string_view text);

} // namespace isla::server::memory
