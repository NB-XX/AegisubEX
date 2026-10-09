// Aegisub layer-style expansion helpers.
#pragma once

class AssFile;

/// Expand Aegisub-only style effects into ordinary ASS style/event layers.
/// If markers is true, standard Comment events are added so this Aegisub build
/// can restore each expanded group to its original single dialogue on reload.
void ExpandStyleEffects(AssFile& file, bool markers);

/// Restore expanded Aegisub groups from their standard Comment markers.
void CollapseStyleEffects(AssFile& file);
