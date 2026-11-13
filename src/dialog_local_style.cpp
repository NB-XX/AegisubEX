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

#include "dialog_local_style.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_override.h"
#include "ass_style.h"
#include "colour_button.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "text_selection_controller.h"

#include <libaegisub/format.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/fontenum.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

LocalStyleTagFamily GetTagFamily(const std::string &name) {
	if (name == "\\c" || name == "\\1c") return LocalStyleTagFamily::PRIMARY_COLOR;
	if (name == "\\2c") return LocalStyleTagFamily::SECONDARY_COLOR;
	if (name == "\\3c") return LocalStyleTagFamily::OUTLINE_COLOR;
	if (name == "\\4c") return LocalStyleTagFamily::SHADOW_COLOR;
	if (name == "\\fs" || name == "\\fs+" || name == "\\fs-") return LocalStyleTagFamily::FONT_SIZE;
	if (name == "\\fn") return LocalStyleTagFamily::FONT_NAME;
	if (name == "\\b")  return LocalStyleTagFamily::BOLD;
	if (name == "\\i")  return LocalStyleTagFamily::ITALIC;
	if (name == "\\u")  return LocalStyleTagFamily::UNDERLINE;
	if (name == "\\s")  return LocalStyleTagFamily::STRIKEOUT;
	return LocalStyleTagFamily::UNKNOWN;
}

static void UpdateStateFromOverrideBlock(LocalStyleState &state, const AssDialogueBlockOverride *ovr, const AssStyle *style) {
	for (const auto& tag : ovr->Tags) {
		if (!tag.IsValid()) continue;
		bool omitted = tag.Params.empty() || tag.Params[0].omitted;

		if (tag.Name == "\\fs" || tag.Name == "\\fs+" || tag.Name == "\\fs-") {
			if (omitted) {
				state.has_fs = false;
				state.fs = (int)style->fontsize;
			} else {
				state.has_fs = true;
				state.fs = tag.Params[0].Get<int>((int)style->fontsize);
			}
		}
		else if (tag.Name == "\\fn") {
			if (omitted) {
				state.has_fn = false;
				state.fn = style->font;
			} else {
				state.has_fn = true;
				state.fn = tag.Params[0].Get<std::string>(style->font);
			}
		}
		else if (tag.Name == "\\c" || tag.Name == "\\1c") {
			if (omitted) {
				state.has_c1 = false;
				state.c1 = style->primary;
			} else {
				state.has_c1 = true;
				state.c1 = tag.Params[0].Get<agi::Color>(style->primary);
			}
		}
		else if (tag.Name == "\\2c") {
			if (omitted) {
				state.has_c2 = false;
				state.c2 = style->secondary;
			} else {
				state.has_c2 = true;
				state.c2 = tag.Params[0].Get<agi::Color>(style->secondary);
			}
		}
		else if (tag.Name == "\\3c") {
			if (omitted) {
				state.has_c3 = false;
				state.c3 = style->outline;
			} else {
				state.has_c3 = true;
				state.c3 = tag.Params[0].Get<agi::Color>(style->outline);
			}
		}
		else if (tag.Name == "\\4c") {
			if (omitted) {
				state.has_c4 = false;
				state.c4 = style->shadow;
			} else {
				state.has_c4 = true;
				state.c4 = tag.Params[0].Get<agi::Color>(style->shadow);
			}
		}
		else if (tag.Name == "\\b") {
			if (omitted) {
				state.has_b = false;
				state.b = style->bold ? 1 : 0;
			} else {
				state.has_b = true;
				state.b = tag.Params[0].Get<int>(style->bold ? 1 : 0);
			}
		}
		else if (tag.Name == "\\i") {
			if (omitted) {
				state.has_i = false;
				state.i = style->italic ? 1 : 0;
			} else {
				state.has_i = true;
				state.i = tag.Params[0].Get<int>(style->italic ? 1 : 0);
			}
		}
		else if (tag.Name == "\\u") {
			if (omitted) {
				state.has_u = false;
				state.u = style->underline ? 1 : 0;
			} else {
				state.has_u = true;
				state.u = tag.Params[0].Get<int>(style->underline ? 1 : 0);
			}
		}
		else if (tag.Name == "\\s") {
			if (omitted) {
				state.has_s = false;
				state.s = style->strikeout ? 1 : 0;
			} else {
				state.has_s = true;
				state.s = tag.Params[0].Get<int>(style->strikeout ? 1 : 0);
			}
		}
	}
}

LocalStyleState AnalyzeLocalStylePreState(agi::Context *c, int sel_start, const AssStyle *&out_style) {
	LocalStyleState state;
	AssDialogue *active_line = c->selectionController->GetActiveLine();
	if (!active_line) return state;

	const AssStyle *style = c->ass->GetStyle(active_line->Style);
	static const AssStyle default_style;
	if (!style) style = &default_style;
	out_style = style;

	state.fs = (int)style->fontsize;
	state.fn = style->font;
	state.c1 = style->primary;
	state.c2 = style->secondary;
	state.c3 = style->outline;
	state.c4 = style->shadow;
	state.b = style->bold ? 1 : 0;
	state.i = style->italic ? 1 : 0;
	state.u = style->underline ? 1 : 0;
	state.s = style->strikeout ? 1 : 0;

	std::string full = active_line->Text.get();
	if (sel_start > (int)full.size()) sel_start = (int)full.size();
	if (sel_start < 0) sel_start = 0;

	std::string pre_text = full.substr(0, sel_start);
	AssDialogue temp_diag;
	temp_diag.Text = pre_text;
	auto blocks = temp_diag.ParseTags();

	for (const auto& block : blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			UpdateStateFromOverrideBlock(state, static_cast<AssDialogueBlockOverride*>(block.get()), style);
		}
	}

	return state;
}

LocalStyleState AnalyzeLocalStyleSelectionState(agi::Context *c, int sel_start, int sel_end, const AssStyle *&out_style, LocalStyleState *out_pre_state) {
	LocalStyleState pre_state = AnalyzeLocalStylePreState(c, sel_start, out_style);
	if (out_pre_state) *out_pre_state = pre_state;

	LocalStyleState sel_state = pre_state;
	AssDialogue *active_line = c->selectionController->GetActiveLine();
	if (!active_line) return sel_state;

	std::string full = active_line->Text.get();
	if (sel_start < 0) sel_start = 0;
	if (sel_end > (int)full.size()) sel_end = (int)full.size();
	if (sel_start > sel_end) std::swap(sel_start, sel_end);

	std::string sel = full.substr(sel_start, sel_end - sel_start);
	AssDialogue temp_diag;
	temp_diag.Text = sel;
	auto blocks = temp_diag.ParseTags();

	// Update state with leading override blocks in sel (before any plain text or drawing)
	for (const auto& block : blocks) {
		if (block->GetType() == AssBlockType::PLAIN || block->GetType() == AssBlockType::DRAWING) {
			break;
		}
		if (block->GetType() == AssBlockType::OVERRIDE) {
			UpdateStateFromOverrideBlock(sel_state, static_cast<AssDialogueBlockOverride*>(block.get()), out_style);
		}
	}

	return sel_state;
}

bool TryExpandAdjacentStyleOverrides(const std::string &text, int &sel_start, int &sel_end, LocalStyleTagFamily specific_family) {
	if (sel_start < 0 || sel_end > (int)text.size() || sel_start >= sel_end) return false;

	std::string sel = text.substr(sel_start, sel_end - sel_start);
	// If the selection already contains an override block, do not expand
	if (sel.find('{') != std::string::npos && sel.find('}') != std::string::npos) {
		return false;
	}

	// Look backward from sel_start across immediately adjacent override blocks
	int exp_start = sel_start;
	while (exp_start > 0 && text[exp_start - 1] == '}') {
		size_t open_brace = text.rfind('{', exp_start - 1);
		if (open_brace == std::string::npos) break;
		std::string tag_content = text.substr(open_brace + 1, exp_start - 1 - open_brace);
		if (tag_content.find('\\') == std::string::npos) break; // comment block
		exp_start = (int)open_brace;
	}

	// Look forward from sel_end across immediately adjacent override blocks
	int exp_end = sel_end;
	while (exp_end < (int)text.size() && text[exp_end] == '{') {
		size_t close_brace = text.find('}', exp_end + 1);
		if (close_brace == std::string::npos) break;
		std::string tag_content = text.substr(exp_end + 1, close_brace - (exp_end + 1));
		if (tag_content.find('\\') == std::string::npos) break;
		exp_end = (int)close_brace + 1;
	}

	if (exp_start == sel_start && exp_end == sel_end) return false;

	// Check if the adjacent override blocks contain local style tags matching the target
	AssDialogue temp_adj;
	temp_adj.Text = text.substr(exp_start, exp_end - exp_start);
	auto adj_blocks = temp_adj.ParseTags();

	bool matches = false;
	for (const auto& block : adj_blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			for (const auto& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags) {
				LocalStyleTagFamily fam = GetTagFamily(tag.Name);
				if (fam != LocalStyleTagFamily::UNKNOWN) {
					if (specific_family == LocalStyleTagFamily::UNKNOWN || specific_family == fam) {
						matches = true;
						break;
					}
				}
			}
		}
		if (matches) break;
	}

	if (matches) {
		sel_start = exp_start;
		sel_end = exp_end;
		return true;
	}
	return false;
}

std::string MergeLocalStyleTags(const std::string &sel, const std::string &opening_tag, const std::string &closing_tag) {
	if (opening_tag.empty() && closing_tag.empty()) return sel;

	AssDialogue temp_open;
	temp_open.Text = opening_tag;
	auto open_blocks = temp_open.ParseTags();

	std::vector<std::string> open_tag_strs;
	for (auto &block : open_blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			auto ovr = static_cast<AssDialogueBlockOverride*>(block.get());
			for (auto const& t : ovr->Tags) {
				if (t.IsValid()) open_tag_strs.push_back((std::string)t);
			}
		}
	}

	AssDialogue temp_close;
	temp_close.Text = closing_tag;
	auto close_blocks = temp_close.ParseTags();

	std::vector<std::string> close_tag_strs;
	for (auto &block : close_blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			auto ovr = static_cast<AssDialogueBlockOverride*>(block.get());
			for (auto const& t : ovr->Tags) {
				if (t.IsValid()) close_tag_strs.push_back((std::string)t);
			}
		}
	}

	if (open_tag_strs.empty() && close_tag_strs.empty()) return sel;

	AssDialogue temp_sel;
	temp_sel.Text = sel;
	auto blocks = temp_sel.ParseTags();

	if (blocks.empty()) {
		return opening_tag + closing_tag;
	}

	int first_plain = -1;
	int last_plain = -1;
	for (size_t i = 0; i < blocks.size(); ++i) {
		if (blocks[i]->GetType() == AssBlockType::PLAIN || blocks[i]->GetType() == AssBlockType::DRAWING) {
			if (first_plain == -1) first_plain = (int)i;
			last_plain = (int)i;
		}
	}

	for (const auto &open_str : open_tag_strs) {
		AssOverrideTag parsed_open(open_str);
		LocalStyleTagFamily fam = GetTagFamily(parsed_open.Name);
		if (fam == LocalStyleTagFamily::UNKNOWN) continue;

		std::string matching_close_str;
		for (const auto &c_str : close_tag_strs) {
			AssOverrideTag parsed_close(c_str);
			if (GetTagFamily(parsed_close.Name) == fam) {
				matching_close_str = c_str;
				break;
			}
		}

		if (first_plain == -1) {
			bool replaced = false;
			for (auto &block : blocks) {
				if (block->GetType() == AssBlockType::OVERRIDE) {
					auto ovr = static_cast<AssDialogueBlockOverride*>(block.get());
					for (size_t t = 0; t < ovr->Tags.size(); ++t) {
						if (GetTagFamily(ovr->Tags[t].Name) == fam) {
							if (!replaced) {
								ovr->Tags[t].SetText(open_str);
								replaced = true;
							} else {
								ovr->Tags.erase(ovr->Tags.begin() + t);
								--t;
							}
						}
					}
				}
			}
			if (!replaced) {
				AssDialogueBlockOverride *target_ovr = nullptr;
				for (auto &block : blocks) {
					if (block->GetType() == AssBlockType::OVERRIDE) {
						target_ovr = static_cast<AssDialogueBlockOverride*>(block.get());
						break;
					}
				}
				if (target_ovr) {
					target_ovr->AddTag(open_str);
				} else {
					auto new_ovr = std::make_unique<AssDialogueBlockOverride>();
					new_ovr->AddTag(open_str);
					blocks.insert(blocks.begin(), std::move(new_ovr));
				}
			}
			continue;
		}

		// Selection has plain text:
		// 1. In leading override blocks (0 .. first_plain - 1):
		bool replaced_leading = false;
		for (int b = 0; b < first_plain; ++b) {
			if (blocks[b]->GetType() == AssBlockType::OVERRIDE) {
				auto ovr = static_cast<AssDialogueBlockOverride*>(blocks[b].get());
				for (size_t t = 0; t < ovr->Tags.size(); ++t) {
					if (GetTagFamily(ovr->Tags[t].Name) == fam) {
						if (!replaced_leading) {
							ovr->Tags[t].SetText(open_str);
							replaced_leading = true;
						} else {
							ovr->Tags.erase(ovr->Tags.begin() + t);
							--t;
						}
					}
				}
			}
		}

		if (!replaced_leading) {
			AssDialogueBlockOverride *target_ovr = nullptr;
			for (int b = first_plain - 1; b >= 0; --b) {
				if (blocks[b]->GetType() == AssBlockType::OVERRIDE) {
					target_ovr = static_cast<AssDialogueBlockOverride*>(blocks[b].get());
					break;
				}
			}
			if (target_ovr) {
				target_ovr->AddTag(open_str);
			} else {
				auto new_ovr = std::make_unique<AssDialogueBlockOverride>();
				new_ovr->AddTag(open_str);
				blocks.insert(blocks.begin(), std::move(new_ovr));
				first_plain++;
				last_plain++;
			}
		}

		// 2. In body blocks (between first_plain and last_plain):
		for (int b = first_plain + 1; b < last_plain; ++b) {
			if (blocks[b]->GetType() == AssBlockType::OVERRIDE) {
				auto ovr = static_cast<AssDialogueBlockOverride*>(blocks[b].get());
				for (size_t t = 0; t < ovr->Tags.size(); ++t) {
					if (GetTagFamily(ovr->Tags[t].Name) == fam) {
						ovr->Tags.erase(ovr->Tags.begin() + t);
						--t;
					}
				}
			}
		}

		// 3. In trailing override blocks (last_plain + 1 .. end):
		if (!matching_close_str.empty()) {
			bool replaced_trailing = false;
			for (size_t b = last_plain + 1; b < blocks.size(); ++b) {
				if (blocks[b]->GetType() == AssBlockType::OVERRIDE) {
					auto ovr = static_cast<AssDialogueBlockOverride*>(blocks[b].get());
					for (size_t t = 0; t < ovr->Tags.size(); ++t) {
						if (GetTagFamily(ovr->Tags[t].Name) == fam) {
							if (!replaced_trailing) {
								ovr->Tags[t].SetText(matching_close_str);
								replaced_trailing = true;
							} else {
								ovr->Tags.erase(ovr->Tags.begin() + t);
								--t;
							}
						}
					}
				}
			}

			if (!replaced_trailing) {
				AssDialogueBlockOverride *target_ovr = nullptr;
				for (size_t b = last_plain + 1; b < blocks.size(); ++b) {
					if (blocks[b]->GetType() == AssBlockType::OVERRIDE) {
						target_ovr = static_cast<AssDialogueBlockOverride*>(blocks[b].get());
						break;
					}
				}
				if (target_ovr) {
					target_ovr->AddTag(matching_close_str);
				} else {
					auto new_ovr = std::make_unique<AssDialogueBlockOverride>();
					new_ovr->AddTag(matching_close_str);
					blocks.push_back(std::move(new_ovr));
				}
			}
		}
	}

	std::string result;
	for (auto &block : blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			auto ovr = static_cast<AssDialogueBlockOverride*>(block.get());
			if (ovr->Tags.empty()) continue;
			result += ovr->GetText();
		} else {
			result += block->GetText();
		}
	}

	return result;
}

void ApplyLocalStyleTags(agi::Context *c, int sel_start, int sel_end, const std::string &opening_tag, const std::string &closing_tag) {
	if (opening_tag.empty() && closing_tag.empty()) return;

	AssDialogue *active_line = c->selectionController->GetActiveLine();
	if (!active_line) return;

	std::string text = active_line->Text.get();
	if (sel_start < 0) sel_start = 0;
	if (sel_end > (int)text.size()) sel_end = (int)text.size();
	if (sel_start > sel_end) std::swap(sel_start, sel_end);

	TryExpandAdjacentStyleOverrides(text, sel_start, sel_end);

	std::string prefix = text.substr(0, sel_start);
	std::string sel = text.substr(sel_start, sel_end - sel_start);
	std::string suffix = text.substr(sel_end);

	std::string replacement = MergeLocalStyleTags(sel, opening_tag, closing_tag);
	active_line->Text = prefix + replacement + suffix;

	c->ass->Commit(to_wx("局部样式变更"), AssFile::COMMIT_DIAG_TEXT, -1, active_line);
	c->textSelectionController->SetSelection(sel_start, sel_start + (int)replacement.size());
}

DialogLocalStyle::DialogLocalStyle(wxWindow *parent, agi::Context *c, int sel_start, int sel_end)
: wxDialog(parent, -1, to_wx("局部样式变更"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, context(c)
, sel_start(sel_start)
, sel_end(sel_end)
{
	AssDialogue *active_line = context->selectionController->GetActiveLine();
	if (active_line) {
		full_text = active_line->Text.get();
		if (this->sel_start < 0) this->sel_start = 0;
		if (this->sel_end > (int)full_text.size()) this->sel_end = (int)full_text.size();
		if (this->sel_start > this->sel_end) std::swap(this->sel_start, this->sel_end);
		TryExpandAdjacentStyleOverrides(full_text, this->sel_start, this->sel_end);
		selected_text = full_text.substr(this->sel_start, this->sel_end - this->sel_start);
	}

	const AssStyle *style = nullptr;
	sel_state = AnalyzeLocalStyleSelectionState(context, this->sel_start, this->sel_end, style, &pre_state);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);

	// 1. Selected text display
	AssDialogue temp_diag;
	temp_diag.Text = selected_text;
	std::string display_text = temp_diag.GetStrippedText();
	if (display_text.empty()) display_text = selected_text;

	auto text_box = new wxStaticBoxSizer(wxVERTICAL, this, to_wx("选中文本"));
	auto text_ctrl = new wxTextCtrl(this, -1, to_wx(display_text), wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
	text_box->Add(text_ctrl, wxSizerFlags(1).Expand().Border(wxALL, 3));
	main_sizer->Add(text_box, wxSizerFlags().Expand().Border(wxALL, 6));

	// 2. Color adjustments
	auto color_box = new wxStaticBoxSizer(wxVERTICAL, this, to_wx("颜色调节"));
	auto color_grid = new wxFlexGridSizer(2, 4, 6, 12);

	chk_c1 = new wxCheckBox(this, -1, to_wx("主体颜色 (\\c)"));
	btn_c1 = new ColourButton(this, FromDIP(wxSize(38, 20)), false, sel_state.c1);
	auto c1_sizer = new wxBoxSizer(wxHORIZONTAL);
	c1_sizer->Add(chk_c1, wxSizerFlags().Center().Border(wxRIGHT, 5));
	c1_sizer->Add(btn_c1, wxSizerFlags().Center());
	color_grid->Add(c1_sizer, wxSizerFlags().CenterVertical());

	chk_c3 = new wxCheckBox(this, -1, to_wx("边框颜色 (\\3c)"));
	btn_c3 = new ColourButton(this, FromDIP(wxSize(38, 20)), false, sel_state.c3);
	auto c3_sizer = new wxBoxSizer(wxHORIZONTAL);
	c3_sizer->Add(chk_c3, wxSizerFlags().Center().Border(wxRIGHT, 5));
	c3_sizer->Add(btn_c3, wxSizerFlags().Center());
	color_grid->Add(c3_sizer, wxSizerFlags().CenterVertical());

	chk_c4 = new wxCheckBox(this, -1, to_wx("阴影颜色 (\\4c)"));
	btn_c4 = new ColourButton(this, FromDIP(wxSize(38, 20)), false, sel_state.c4);
	auto c4_sizer = new wxBoxSizer(wxHORIZONTAL);
	c4_sizer->Add(chk_c4, wxSizerFlags().Center().Border(wxRIGHT, 5));
	c4_sizer->Add(btn_c4, wxSizerFlags().Center());
	color_grid->Add(c4_sizer, wxSizerFlags().CenterVertical());

	chk_c2 = new wxCheckBox(this, -1, to_wx("次要颜色 (\\2c)"));
	btn_c2 = new ColourButton(this, FromDIP(wxSize(38, 20)), false, sel_state.c2);
	auto c2_sizer = new wxBoxSizer(wxHORIZONTAL);
	c2_sizer->Add(chk_c2, wxSizerFlags().Center().Border(wxRIGHT, 5));
	c2_sizer->Add(btn_c2, wxSizerFlags().Center());
	color_grid->Add(c2_sizer, wxSizerFlags().CenterVertical());

	color_box->Add(color_grid, wxSizerFlags(1).Expand().Border(wxALL, 5));
	main_sizer->Add(color_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));

	// 3. Font and Size
	auto font_box = new wxStaticBoxSizer(wxVERTICAL, this, to_wx("字体与字号"));
	auto font_row = new wxBoxSizer(wxHORIZONTAL);

	chk_fn = new wxCheckBox(this, -1, to_wx("字体 (\\fn):"));
	wxArrayString font_list = wxFontEnumerator::GetFacenames();
	font_list.Sort();
	cmb_fn = new wxComboBox(this, -1, to_wx(sel_state.fn), wxDefaultPosition, wxDefaultSize, font_list, wxCB_DROPDOWN);
	cmb_fn->Enable(false);

	font_row->Add(chk_fn, wxSizerFlags().Center().Border(wxRIGHT, 5));
	font_row->Add(cmb_fn, wxSizerFlags(1).Center().Border(wxRIGHT, 15));

	chk_fs = new wxCheckBox(this, -1, to_wx("字号 (\\fs):"));
	spn_fs = new wxSpinCtrl(this, -1, std::to_string(sel_state.fs), wxDefaultPosition, FromDIP(wxSize(75, -1)), wxSP_ARROW_KEYS, 1, 1000, sel_state.fs);
	spn_fs->Enable(false);

	font_row->Add(chk_fs, wxSizerFlags().Center().Border(wxRIGHT, 5));
	font_row->Add(spn_fs, wxSizerFlags().Center());

	font_box->Add(font_row, wxSizerFlags().Expand().Border(wxALL, 5));
	main_sizer->Add(font_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));

	// 4. Style attributes (Bold, Italic, Underline, Strikeout)
	auto style_box = new wxStaticBoxSizer(wxVERTICAL, this, to_wx("样式效果"));
	auto style_grid = new wxFlexGridSizer(2, 4, 6, 12);

	wxArrayString bool_choices;
	bool_choices.Add(to_wx("开启 (1)"));
	bool_choices.Add(to_wx("关闭 (0)"));

	chk_b = new wxCheckBox(this, -1, to_wx("粗体 (\\b):"));
	cho_b = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, bool_choices);
	cho_b->SetSelection(sel_state.b == 0 ? 0 : 1);
	cho_b->Enable(false);
	auto b_sizer = new wxBoxSizer(wxHORIZONTAL);
	b_sizer->Add(chk_b, wxSizerFlags().Center().Border(wxRIGHT, 5));
	b_sizer->Add(cho_b, wxSizerFlags().Center());
	style_grid->Add(b_sizer, wxSizerFlags().CenterVertical());

	chk_i = new wxCheckBox(this, -1, to_wx("斜体 (\\i):"));
	cho_i = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, bool_choices);
	cho_i->SetSelection(sel_state.i == 0 ? 0 : 1);
	cho_i->Enable(false);
	auto i_sizer = new wxBoxSizer(wxHORIZONTAL);
	i_sizer->Add(chk_i, wxSizerFlags().Center().Border(wxRIGHT, 5));
	i_sizer->Add(cho_i, wxSizerFlags().Center());
	style_grid->Add(i_sizer, wxSizerFlags().CenterVertical());

	chk_u = new wxCheckBox(this, -1, to_wx("下划线 (\\u):"));
	cho_u = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, bool_choices);
	cho_u->SetSelection(sel_state.u == 0 ? 0 : 1);
	cho_u->Enable(false);
	auto u_sizer = new wxBoxSizer(wxHORIZONTAL);
	u_sizer->Add(chk_u, wxSizerFlags().Center().Border(wxRIGHT, 5));
	u_sizer->Add(cho_u, wxSizerFlags().Center());
	style_grid->Add(u_sizer, wxSizerFlags().CenterVertical());

	chk_s = new wxCheckBox(this, -1, to_wx("删除线 (\\s):"));
	cho_s = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, bool_choices);
	cho_s->SetSelection(sel_state.s == 0 ? 0 : 1);
	cho_s->Enable(false);
	auto s_sizer = new wxBoxSizer(wxHORIZONTAL);
	s_sizer->Add(chk_s, wxSizerFlags().Center().Border(wxRIGHT, 5));
	s_sizer->Add(cho_s, wxSizerFlags().Center());
	style_grid->Add(s_sizer, wxSizerFlags().CenterVertical());

	style_box->Add(style_grid, wxSizerFlags(1).Expand().Border(wxALL, 5));
	main_sizer->Add(style_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));

	// 5. Preview
	auto prev_box = new wxStaticBoxSizer(wxVERTICAL, this, to_wx("效果预览"));
	preview_ctrl = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
	prev_box->Add(preview_ctrl, wxSizerFlags(1).Expand().Border(wxALL, 3));
	main_sizer->Add(prev_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));

	// 6. Buttons
	auto btn_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	main_sizer->Add(btn_sizer, wxSizerFlags().Right().Border(wxALL, 8));

	SetSizerAndFit(main_sizer);
	CenterOnParent();

	// Event bindings
	auto on_chk_toggle = [this](wxCheckBox *chk, wxWindow *ctrl) {
		if (ctrl) ctrl->Enable(chk->IsChecked());
		UpdatePreview();
	};

	chk_c1->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_c1, nullptr); });
	chk_c3->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_c3, nullptr); });
	chk_c4->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_c4, nullptr); });
	chk_c2->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_c2, nullptr); });

	btn_c1->Bind(EVT_COLOR, [this](ValueEvent<agi::Color>&) { chk_c1->SetValue(true); UpdatePreview(); });
	btn_c3->Bind(EVT_COLOR, [this](ValueEvent<agi::Color>&) { chk_c3->SetValue(true); UpdatePreview(); });
	btn_c4->Bind(EVT_COLOR, [this](ValueEvent<agi::Color>&) { chk_c4->SetValue(true); UpdatePreview(); });
	btn_c2->Bind(EVT_COLOR, [this](ValueEvent<agi::Color>&) { chk_c2->SetValue(true); UpdatePreview(); });

	chk_fn->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_fn, cmb_fn); });
	cmb_fn->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { chk_fn->SetValue(true); cmb_fn->Enable(true); UpdatePreview(); });
	cmb_fn->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { chk_fn->SetValue(true); cmb_fn->Enable(true); UpdatePreview(); });

	chk_fs->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_fs, spn_fs); });
	spn_fs->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { chk_fs->SetValue(true); spn_fs->Enable(true); UpdatePreview(); });
	spn_fs->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { chk_fs->SetValue(true); spn_fs->Enable(true); UpdatePreview(); });

	chk_b->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_b, cho_b); });
	cho_b->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { chk_b->SetValue(true); cho_b->Enable(true); UpdatePreview(); });

	chk_i->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_i, cho_i); });
	cho_i->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { chk_i->SetValue(true); cho_i->Enable(true); UpdatePreview(); });

	chk_u->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_u, cho_u); });
	cho_u->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { chk_u->SetValue(true); cho_u->Enable(true); UpdatePreview(); });

	chk_s->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { on_chk_toggle(chk_s, cho_s); });
	cho_s->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { chk_s->SetValue(true); cho_s->Enable(true); UpdatePreview(); });

	UpdatePreview();
}

DialogLocalStyle::~DialogLocalStyle() = default;

std::string DialogLocalStyle::GetOpeningTag() const {
	std::string tags;

	if (chk_c1 && chk_c1->IsChecked() && btn_c1) {
		tags += "\\c" + btn_c1->GetColor().GetAssOverrideFormatted();
	}
	if (chk_c2 && chk_c2->IsChecked() && btn_c2) {
		tags += "\\2c" + btn_c2->GetColor().GetAssOverrideFormatted();
	}
	if (chk_c3 && chk_c3->IsChecked() && btn_c3) {
		tags += "\\3c" + btn_c3->GetColor().GetAssOverrideFormatted();
	}
	if (chk_c4 && chk_c4->IsChecked() && btn_c4) {
		tags += "\\4c" + btn_c4->GetColor().GetAssOverrideFormatted();
	}
	if (chk_fn && chk_fn->IsChecked() && cmb_fn) {
		tags += "\\fn" + from_wx(cmb_fn->GetValue());
	}
	if (chk_fs && chk_fs->IsChecked() && spn_fs) {
		tags += "\\fs" + std::to_string(spn_fs->GetValue());
	}
	if (chk_b && chk_b->IsChecked() && cho_b) {
		tags += "\\b" + std::to_string(cho_b->GetSelection() == 0 ? 1 : 0);
	}
	if (chk_i && chk_i->IsChecked() && cho_i) {
		tags += "\\i" + std::to_string(cho_i->GetSelection() == 0 ? 1 : 0);
	}
	if (chk_u && chk_u->IsChecked() && cho_u) {
		tags += "\\u" + std::to_string(cho_u->GetSelection() == 0 ? 1 : 0);
	}
	if (chk_s && chk_s->IsChecked() && cho_s) {
		tags += "\\s" + std::to_string(cho_s->GetSelection() == 0 ? 1 : 0);
	}

	if (tags.empty()) return "";
	return "{" + tags + "}";
}

std::string DialogLocalStyle::GetClosingTag() const {
	std::string tags;

	if (chk_s && chk_s->IsChecked()) {
		if (pre_state.has_s)
			tags += "\\s" + std::to_string(pre_state.s);
		else
			tags += "\\s";
	}
	if (chk_u && chk_u->IsChecked()) {
		if (pre_state.has_u)
			tags += "\\u" + std::to_string(pre_state.u);
		else
			tags += "\\u";
	}
	if (chk_i && chk_i->IsChecked()) {
		if (pre_state.has_i)
			tags += "\\i" + std::to_string(pre_state.i);
		else
			tags += "\\i";
	}
	if (chk_b && chk_b->IsChecked()) {
		if (pre_state.has_b)
			tags += "\\b" + std::to_string(pre_state.b);
		else
			tags += "\\b";
	}
	if (chk_fs && chk_fs->IsChecked()) {
		if (pre_state.has_fs)
			tags += "\\fs" + std::to_string(pre_state.fs);
		else
			tags += "\\fs";
	}
	if (chk_fn && chk_fn->IsChecked()) {
		if (pre_state.has_fn)
			tags += "\\fn" + pre_state.fn;
		else
			tags += "\\fn";
	}
	if (chk_c4 && chk_c4->IsChecked()) {
		if (pre_state.has_c4)
			tags += "\\4c" + pre_state.c4.GetAssOverrideFormatted();
		else
			tags += "\\4c";
	}
	if (chk_c3 && chk_c3->IsChecked()) {
		if (pre_state.has_c3)
			tags += "\\3c" + pre_state.c3.GetAssOverrideFormatted();
		else
			tags += "\\3c";
	}
	if (chk_c2 && chk_c2->IsChecked()) {
		if (pre_state.has_c2)
			tags += "\\2c" + pre_state.c2.GetAssOverrideFormatted();
		else
			tags += "\\2c";
	}
	if (chk_c1 && chk_c1->IsChecked()) {
		if (pre_state.has_c1)
			tags += "\\c" + pre_state.c1.GetAssOverrideFormatted();
		else
			tags += "\\c";
	}

	if (tags.empty()) return "";
	return "{" + tags + "}";
}

void DialogLocalStyle::UpdatePreview() {
	if (!preview_ctrl) return;
	std::string opening = GetOpeningTag();
	std::string closing = GetClosingTag();
	std::string preview = MergeLocalStyleTags(selected_text, opening, closing);
	preview_ctrl->SetValue(to_wx(preview));
}

void DialogLocalStyle::Apply() {
	std::string opening = GetOpeningTag();
	std::string closing = GetClosingTag();
	ApplyLocalStyleTags(context, sel_start, sel_end, opening, closing);
}
