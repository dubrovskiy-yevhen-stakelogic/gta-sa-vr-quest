#pragma once

// Object script-brain start-chain diagnostics (basketball investigation).
// Logs [braindiag] lines for CScriptsForBrains::CheckIfNewEntityNeedsScript /
// StartOrRequestNewStreamedScriptBrain / StartNewStreamedScriptBrain so a
// device capture shows exactly where the court brain chain stops.
namespace savr::braindiag {

// Install the logging hooks. Call once with the libGame.so NOLOAD handle.
void Install(void* handle);

}  // namespace savr::braindiag
