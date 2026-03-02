#include "isla/engine/bootstrap.hpp"
#include "isla/shared/build_info.hpp"

namespace isla::engine {

const char* bootstrap_message() {
    return isla::shared::build_info();
}

} // namespace isla::engine


