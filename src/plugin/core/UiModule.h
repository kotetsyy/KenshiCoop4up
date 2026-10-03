// UiModule.h - core-side host for the optional KenshiCoopUI.dll provider.
//
// The F2 panel, connection banner and private diagnostic overlay live in
// KenshiCoopUI.dll beside KenshiCoop.dll. The provider is loaded on the first
// tick, initialized once and pinned until process exit (never unloaded or
// reloaded). A missing, ABI-incompatible or faulting provider is logged once and
// disables the UI only; networking and the game loop keep running.

#ifndef KENSHICOOP_UI_MODULE_H
#define KENSHICOOP_UI_MODULE_H

#include "../../ui/CoopUiApi.h"

namespace coop {
namespace uimodule {

// Game main thread only. Hands *state to the provider and fills *command with
// the one action it queued (kind COOP_UI_NONE when there is none, or when the UI
// is unavailable). Strings in command->settings are NUL-terminated on return.
void tick(const CoopUiSnapshot* state, CoopUiCommand* command);

} // namespace uimodule
} // namespace coop

#endif // KENSHICOOP_UI_MODULE_H
