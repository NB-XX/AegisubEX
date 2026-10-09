#pragma once

#include <string>

class wxWindow;
namespace agi::fs { class path; }

/// Show source-line diagnostics for a failed ASS/SRT load. Returns true if a
/// format-specific report was shown, false when the caller should show its
/// regular load error instead.
bool ShowSubtitleFormatAudit(wxWindow *parent, agi::fs::path const& filename,
	char const *encoding, std::string const& parser_error);
