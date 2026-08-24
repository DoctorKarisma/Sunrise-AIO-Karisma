#pragma once

#include "../hooks/spawn/spawn_runtime.h"

namespace sunrise::client::spawn {

void initialize_population(void* module) noexcept;
void shutdown_population() noexcept;
bool publish_population(const hooks::spawn::PopulationSettings& settings) noexcept;

} // namespace sunrise::client::spawn
