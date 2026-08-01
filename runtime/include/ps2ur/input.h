// input: DualShock 2 via libpad (sio2man.irx + padman.irx) -- sticks, pressure
// buttons, rumble (plan sections 3.5, 7.1, 8: src/input).
// TODO(spec missing: section 9): input milestone tasks and mapping table.
#pragma once

namespace ps2ur {
namespace input {

bool init();
void shutdown();
bool initialized();

} // namespace input
} // namespace ps2ur
