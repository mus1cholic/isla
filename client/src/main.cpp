#include "absl/log/initialize.h"
#include "absl/log/log.h"

#include "client_app.hpp"
#include "isla/engine/bootstrap.hpp"

int main() {
    absl::InitializeLog();
    LOG(INFO) << "isla client boot: " << isla::engine::bootstrap_message();
    isla::client::ClientApp app;
    return app.run();
}


