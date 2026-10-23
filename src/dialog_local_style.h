// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <wx/dialog.h>
#include <libaegisub/color.h>
#include <string>

class wxCheckBox;
class wxComboBox;
class wxSpinCtrl;
class wxChoice;
class wxTextCtrl;
class ColourButton;
class AssStyle;

namespace agi {
	struct Context;
}

struct LocalStyleState {
	bool has_fs = false;
	int fs = 0;

	bool has_fn = false;
	std::string fn;

	bool has_c1 = false;
	agi::Color c1;

	bool has_c2 = false;
	agi::Color c2;

	bool has_c3 = false;
	agi::Color c3;

	bool has_c4 = false;
	agi::Color c4;

	bool has_b = false;
	int b = 0;

	bool has_i = false;
	int i = 0;

	bool has_u = false;
	int u = 0;

	bool has_s = false;
	int s = 0;
};

LocalStyleState AnalyzeLocalStylePreState(agi::Context *c, int sel_start, const AssStyle *&out_style);
void ApplyLocalStyleTags(agi::Context *c, int sel_start, int sel_end, const std::string &opening_tag, const std::string &closing_tag);

/// Dialog for adjusting style tags for a selected range of text without affecting surrounding text
class DialogLocalStyle final : public wxDialog {
	agi::Context *context;
	int sel_start;
	int sel_end;
	std::string selected_text;
	std::string full_text;
	LocalStyleState pre_state;

	// Controls
	wxCheckBox *chk_c1 = nullptr;
	ColourButton *btn_c1 = nullptr;

	wxCheckBox *chk_c3 = nullptr;
	ColourButton *btn_c3 = nullptr;

	wxCheckBox *chk_c4 = nullptr;
	ColourButton *btn_c4 = nullptr;

	wxCheckBox *chk_c2 = nullptr;
	ColourButton *btn_c2 = nullptr;

	wxCheckBox *chk_fn = nullptr;
	wxComboBox *cmb_fn = nullptr;

	wxCheckBox *chk_fs = nullptr;
	wxSpinCtrl *spn_fs = nullptr;

	wxCheckBox *chk_b = nullptr;
	wxChoice *cho_b = nullptr;

	wxCheckBox *chk_i = nullptr;
	wxChoice *cho_i = nullptr;

	wxCheckBox *chk_u = nullptr;
	wxChoice *cho_u = nullptr;

	wxCheckBox *chk_s = nullptr;
	wxChoice *cho_s = nullptr;

	wxTextCtrl *preview_ctrl = nullptr;

	void UpdatePreview();

public:
	DialogLocalStyle(wxWindow *parent, agi::Context *c, int sel_start, int sel_end);
	~DialogLocalStyle() override;

	std::string GetOpeningTag() const;
	std::string GetClosingTag() const;
	void Apply();
};
