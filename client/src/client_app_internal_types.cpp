#include "client_app_internal_types.hpp"

#include <memory>

#include "animated_mesh_skinning.hpp"

namespace isla::client::internal {

AnimatedMeshBinding::AnimatedMeshBinding()
    : skinning_workspace(
          std::make_unique<animated_mesh_skinning::PrimitiveSkinningWorkspace>()) {}

AnimatedMeshBinding::~AnimatedMeshBinding() = default;
AnimatedMeshBinding::AnimatedMeshBinding(AnimatedMeshBinding&&) noexcept = default;
AnimatedMeshBinding& AnimatedMeshBinding::operator=(AnimatedMeshBinding&&) noexcept = default;

} // namespace isla::client::internal
