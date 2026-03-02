#pragma once

#include <string>
#include <string_view>

namespace isla::shared::test {

[[nodiscard]] std::string runfile_path(std::string_view relative_workspace_path);

} // namespace isla::shared::test

