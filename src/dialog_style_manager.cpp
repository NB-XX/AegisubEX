// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "ass_style_storage.h"
#include "charset_detect.h"
#include "compat.h"
#include "dialog_manager.h"
#include "dialog_style_editor.h"
#include "dialogs.h"
#include "format.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "persist_location.h"
#include "selection_controller.h"
#include "subtitle_format.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/signal.h>
#include <libaegisub/split.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <vector>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/combobox.h>
#include <wx/filename.h>
#include <wx/fontenum.h>
#include <wx/intl.h>
#include <wx/listbox.h>
#ifdef __WXMSW__
#include <wx/msw/dc.h>
#endif
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/choicdlg.h> // Keep this last so wxUSE_CHOICEDLG is set.

namespace {
class StyleListBox final : public wxListBox {
	bool preview_mode = false;
	std::function<const AssStyle*(std::string const&)> style_getter;

public:
	StyleListBox(wxWindow *parent, wxWindowID id, const wxPoint& pos, const wxSize& size,
	             long style, std::function<const AssStyle*(std::string const&)> getter)
	: wxListBox(parent, id, pos, size, 0, nullptr, style | wxLB_OWNERDRAW)
	, style_getter(std::move(getter))
	{
	}

	void SetPreviewMode(bool enable) {
		if (preview_mode == enable) return;
		preview_mode = enable;
		EnsureItemHeights();
		Refresh();
		Update();
	}

	bool GetPreviewMode() const { return preview_mode; }

	void EnsureItemHeights() {
#ifdef __WXMSW__
		if (GetHWND()) {
			int item_h = preview_mode ? FromDIP(48) : FromDIP(20);
			::SendMessage((HWND)GetHWND(), LB_SETITEMHEIGHT, 0, (LPARAM)item_h);
			::SetWindowPos((HWND)GetHWND(), nullptr, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		}
#endif
	}

	const AssStyle *GetStyleFor(size_t idx) const {
		if (idx >= (size_t)GetCount() || !style_getter) return nullptr;
		return style_getter(from_wx(GetString(idx)));
	}

#ifdef __WXMSW__
	bool MSWOnMeasure(WXMEASUREITEMSTRUCT *item) override {
		MEASUREITEMSTRUCT *pStruct = (MEASUREITEMSTRUCT *)item;
		pStruct->itemHeight = preview_mode ? FromDIP(48) : FromDIP(20);
		pStruct->itemWidth = FromDIP(240);
		return true;
	}

	bool MSWOnDraw(WXDRAWITEMSTRUCT *item) override;
#endif

protected:
	int DoInsertItems(const wxArrayStringsAdapter& items,
	                  unsigned int pos,
	                  void **clientData, wxClientDataType type) override {
		int ret = wxListBox::DoInsertItems(items, pos, clientData, type);
		EnsureItemHeights();
		return ret;
	}
};

#ifdef __WXMSW__
bool StyleListBox::MSWOnDraw(WXDRAWITEMSTRUCT *item) {
	DRAWITEMSTRUCT *pStruct = (DRAWITEMSTRUCT *)item;
	if (pStruct->itemID == (UINT)-1)
		return false;

	size_t idx = pStruct->itemID;
	if (idx >= (size_t)GetCount())
		return false;

	RECT rcItem = pStruct->rcItem;
	HDC hdc = pStruct->hDC;
	bool is_selected = (pStruct->itemState & ODS_SELECTED) != 0;
	bool has_focus = (pStruct->itemState & ODS_FOCUS) != 0;

	wxDCTemp dc((WXHDC)hdc);
	wxRect rect(rcItem.left, rcItem.top, rcItem.right - rcItem.left, rcItem.bottom - rcItem.top);
	if (rect.width <= 0 || rect.height <= 0)
		return true;

	if (!preview_mode) {
		wxColour bg_col = is_selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
		                              : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
		wxColour fg_col = is_selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
		                              : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);

		dc.SetBrush(wxBrush(bg_col));
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.DrawRectangle(rect);

		dc.SetFont(GetFont());
		dc.SetTextForeground(fg_col);
		dc.SetBackgroundMode(wxBRUSHSTYLE_TRANSPARENT);

		wxString text = GetString(idx);
		dc.DrawText(text, rect.x + FromDIP(4), rect.y + (rect.height - dc.GetCharHeight()) / 2);

		if (has_focus && (pStruct->itemState & ODS_NOFOCUSRECT) == 0) {
			::DrawFocusRect(hdc, &rcItem);
		}
		return true;
	}

	// Preview Mode
	const AssStyle *s = GetStyleFor(idx);
	wxString style_name = GetString(idx);

	// 1. Draw row background
	wxColour row_bg = is_selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
	                              : ((idx % 2 == 0) ? wxColour(255, 255, 255) : wxColour(248, 249, 251));
	wxColour text_name = is_selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
	                                 : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	wxColour text_sub = is_selected ? wxColour(230, 235, 255) : wxColour(110, 115, 125);

	dc.SetBrush(wxBrush(row_bg));
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.DrawRectangle(rect);

	if (!is_selected) {
		dc.SetPen(wxPen(wxColour(232, 234, 238)));
		dc.DrawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width, rect.y + rect.height - 1);
	}

	// 2. Left side: Style Name + Font info
	int left_padding = FromDIP(6);
	int info_width = FromDIP(90);

	wxFont name_font = GetFont();
	name_font.SetWeight(wxFONTWEIGHT_BOLD);
	dc.SetFont(name_font);
	dc.SetTextForeground(text_name);
	dc.SetBackgroundMode(wxBRUSHSTYLE_TRANSPARENT);

	wxString disp_name = style_name;
	int max_name_w = info_width - FromDIP(4);
	if (dc.GetTextExtent(disp_name).GetWidth() > max_name_w) {
		while (!disp_name.empty() && dc.GetTextExtent(disp_name + "...").GetWidth() > max_name_w)
			disp_name.RemoveLast();
		disp_name += "...";
	}
	dc.DrawText(disp_name, rect.x + left_padding, rect.y + FromDIP(5));

	// Sub info (font family + size)
	wxFont sub_font = GetFont();
	sub_font.SetPointSize(std::max(7, sub_font.GetPointSize() - 2));
	dc.SetFont(sub_font);
	dc.SetTextForeground(text_sub);

	wxString sub_info = s ? wxString::Format("%s %gpt", to_wx(s->font), s->fontsize) : wxString();
	if (dc.GetTextExtent(sub_info).GetWidth() > max_name_w) {
		while (!sub_info.empty() && dc.GetTextExtent(sub_info + "...").GetWidth() > max_name_w)
			sub_info.RemoveLast();
		sub_info += "...";
	}
	dc.DrawText(sub_info, rect.x + left_padding, rect.y + FromDIP(26));

	// 3. Right side: Actual Subtitle Visual Preview Swatch
	int swatch_x = rect.x + left_padding + info_width + FromDIP(4);
	int swatch_y = rect.y + FromDIP(4);
	int swatch_w = rect.width - (swatch_x - rect.x) - FromDIP(6);
	int swatch_h = rect.height - FromDIP(8);

	if (swatch_w > FromDIP(40) && swatch_h > FromDIP(10)) {
		wxRect swatch_rect(swatch_x, swatch_y, swatch_w, swatch_h);

		wxColour preview_bg(125, 153, 176);
		try {
			preview_bg = to_wx(OPT_GET("Colour/Style Editor/Background/Preview")->GetColor());
		} catch (...) {}

		dc.SetBrush(wxBrush(preview_bg));
		dc.SetPen(wxPen(is_selected ? wxColour(255, 255, 255) : wxColour(85, 110, 130)));
		dc.DrawRoundedRectangle(swatch_rect, FromDIP(3));

		if (s) {
			int font_pt = std::clamp((int)(s->fontsize * 0.30), FromDIP(10), FromDIP(18));
			wxFont preview_font(wxFontInfo(font_pt)
				.FaceName(to_wx(s->font))
				.Bold(s->bold)
				.Italic(s->italic)
				.Underlined(s->underline)
				.Strikethrough(s->strikeout));

			if (!preview_font.IsOk())
				preview_font = wxFont(wxFontInfo(font_pt).Bold(s->bold).Italic(s->italic));

			dc.SetFont(preview_font);
			dc.SetClippingRegion(swatch_rect.Deflate(FromDIP(2)));

			wxString sample = style_name;
			if (sample.size() <= 6 && sample.IsAscii())
				sample += to_wx(" 示例");

			wxSize text_sz = dc.GetTextExtent(sample);
			if (text_sz.GetWidth() > swatch_w - FromDIP(8)) {
				sample = style_name;
				text_sz = dc.GetTextExtent(sample);
			}
			if (text_sz.GetWidth() > swatch_w - FromDIP(8)) {
				while (!sample.empty() && dc.GetTextExtent(sample + "...").GetWidth() > swatch_w - FromDIP(8))
					sample.RemoveLast();
				sample += "...";
				text_sz = dc.GetTextExtent(sample);
			}

			int tx = swatch_x + (swatch_w - text_sz.GetWidth()) / 2;
			int ty = swatch_y + (swatch_h - text_sz.GetHeight()) / 2;
			if (tx < swatch_x + FromDIP(4))
				tx = swatch_x + FromDIP(4);

			wxColour col_pri = to_wx(s->primary);
			wxColour col_out = to_wx(s->outline);
			wxColour col_shd = to_wx(s->shadow);

			// 3a. Opaque box
			if (s->borderstyle == 3) {
				wxRect box_rect(tx - FromDIP(3), ty - FromDIP(1), text_sz.GetWidth() + FromDIP(6), text_sz.GetHeight() + FromDIP(2));
				dc.SetBrush(wxBrush(col_out));
				dc.SetPen(*wxTRANSPARENT_PEN);
				dc.DrawRectangle(box_rect);
			}

			// 3b. Shadow
			if (s->shadow_w > 0.05) {
				int so = (s->shadow_w >= 2.5) ? 2 : 1;
				dc.SetTextForeground(col_shd);
				dc.DrawText(sample, tx + so, ty + so);
			}

			// 3c. Outline
			if (s->borderstyle != 3 && s->outline_w > 0.05) {
				int r = (s->outline_w >= 2.5) ? 2 : 1;
				dc.SetTextForeground(col_out);
				for (int dx = -r; dx <= r; ++dx) {
					for (int dy = -r; dy <= r; ++dy) {
						if (dx != 0 || dy != 0) {
							dc.DrawText(sample, tx + dx, ty + dy);
						}
					}
				}
			}

			// 3d. Primary Fill
			dc.SetTextForeground(col_pri);
			dc.DrawText(sample, tx, ty);

			dc.DestroyClippingRegion();
		}
	}

	if (has_focus && (pStruct->itemState & ODS_NOFOCUSRECT) == 0) {
		::DrawFocusRect(hdc, &rcItem);
	}
	return true;
}
#endif

class DialogStyleManager final : public wxDialog {
	agi::Context *c; ///< Project context
	std::unique_ptr<PersistLocation> persist;

	agi::signal::Connection commit_connection;
	agi::signal::Connection active_line_connection;

	std::shared_future<wxArrayString> font_list;

	/// Styles in the current subtitle file
	std::vector<AssStyle*> styleMap;

	/// Style storage manager
	AssStyleStorage Store;

	wxComboBox *CatalogList;
	StyleListBox *StorageList;
	StyleListBox *CurrentList;

	wxButton *CatalogDelete;
	wxButton *TogglePreview;

	wxButton *MoveToLocal;
	wxButton *MoveToStorage;

	wxButton *StorageNew;
	wxButton *StorageEdit;
	wxButton *StorageCopy;
	wxButton *StorageDelete;
	wxButton *StorageMoveUp;
	wxButton *StorageMoveDown;
	wxButton *StorageMoveTop;
	wxButton *StorageMoveBottom;
	wxButton *StorageSort;

	wxButton *CurrentNew;
	wxButton *CurrentEdit;
	wxButton *CurrentCopy;
	wxButton *CurrentDelete;
	wxButton *CurrentPurge;
	wxButton *CurrentMoveUp;
	wxButton *CurrentMoveDown;
	wxButton *CurrentMoveTop;
	wxButton *CurrentMoveBottom;
	wxButton *CurrentSort;

	/// Load the list of available storages
	void LoadCatalog();
	/// Load the style list from the subtitles file
	void LoadCurrentStyles(int commit_type);
	/// Enable/disable all of the buttons as appropriate
	void UpdateButtons();
	/// Move styles up or down
	/// @param storage Storage or current file styles
	/// @param type 0: up; 1: top; 2: down; 3: bottom; 4: sort
	void MoveStyles(bool storage, int type);

	/// Open the style editor for the given style on the script
	/// @param style Style to edit, or nullptr for new
	/// @param new_name Default new name for copies
	void ShowCurrentEditor(AssStyle *style, std::string const& new_name = "");

	/// Open the style editor for the given style in the storage
	/// @param style Style to edit, or nullptr for new
	/// @param new_name Default new name for copies
	void ShowStorageEditor(AssStyle *style, std::string const& new_name = "");

	/// Save the storage and update the view after a change
	void UpdateStorage();

	void OnChangeCatalog();
	void OnCatalogNew();
	void OnCatalogDelete();

	void OnCopyToCurrent();
	void OnCopyToStorage();

	void OnCurrentCopy();
	void OnCurrentDelete();
	void OnCurrentPurge();
	void OnCurrentEdit();
	void OnCurrentImport();
	void OnCurrentNew();

	void OnStorageCopy();
	void OnStorageDelete();
	void OnStorageEdit();
	void OnStorageNew();

	void OnKeyDown(wxKeyEvent &event);
	void PasteToCurrent();
	void PasteToStorage();

	template<class T>
	void CopyToClipboard(wxListBox *list, T const& v);

	void OnActiveLineChanged(AssDialogue *new_line);

public:
	DialogStyleManager(agi::Context *context);
};

wxBitmapButton *add_bitmap_button(wxWindow *parent, wxSizer *sizer, wxBitmapBundle const& img, wxString const& tooltip) {
	wxBitmapButton *btn = new wxBitmapButton(parent, -1, img);
	btn->SetToolTip(tooltip);
	sizer->Add(btn, wxSizerFlags().Expand());
	return btn;
}

wxSizer *make_move_buttons(wxWindow *parent, wxButton **up, wxButton **down, wxButton **top, wxButton **bottom, wxButton **sort) {
	wxSizer *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->AddStretchSpacer(1);

	*up     = add_bitmap_button(parent, sizer, GETBUNDLE(arrow_up, 24), _("Move style up"));
	*down   = add_bitmap_button(parent, sizer, GETBUNDLE(arrow_down, 24), _("Move style down"));
	*top    = add_bitmap_button(parent, sizer, GETBUNDLE(arrow_up_stop, 24), _("Move style to top"));
	*bottom = add_bitmap_button(parent, sizer, GETBUNDLE(arrow_down_stop, 24), _("Move style to bottom"));
	*sort   = add_bitmap_button(parent, sizer, GETBUNDLE(arrow_sort, 24), _("Sort styles alphabetically"));

	sizer->AddStretchSpacer(1);
	return sizer;
}

wxSizer *make_edit_buttons(wxWindow *parent, wxString move_label, wxButton **move, wxButton **nw, wxButton **edit, wxButton **copy, wxButton **del) {
	wxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);

	*move = new wxButton(parent, -1, move_label);
	*nw = new wxButton(parent, -1, _("&New"));
	*edit = new wxButton(parent, -1, _("&Edit"));
	*copy = new wxButton(parent, -1, _("&Copy"));
	*del = new wxButton(parent, -1, _("&Delete"));

	sizer->Add(*nw, wxSizerFlags(1).Expand().Border(wxRIGHT));
	sizer->Add(*edit, wxSizerFlags(1).Expand().Border(wxRIGHT));
	sizer->Add(*copy, wxSizerFlags(1).Expand().Border(wxRIGHT));
	sizer->Add(*del, wxSizerFlags(1).Expand());

	return sizer;
}

template<class Func>
std::string unique_name(Func name_checker, std::string const& source_name) {
	if (name_checker(source_name)) {
		std::string name = agi::format(_("%s - Copy"), source_name);
		for (int i = 2; name_checker(name); ++i)
			name = agi::format(_("%s - Copy (%d)"), source_name, i);
		return name;
	}
	return source_name;
}

template<class Func1, class Func2>
void add_styles(Func1 name_checker, Func2 style_adder) {
	auto cb = GetClipboard();
	int failed_to_parse = 0;
	for (auto tok : agi::Split(cb, '\n')) {
		tok = agi::Trim(tok);
		if (tok.empty()) continue;
		try {
			AssStyle *s = new AssStyle(tok);
			s->name = unique_name(name_checker, s->name);
			style_adder(s);
		}
		catch (...) {
			++failed_to_parse;
		}
	}
	if (failed_to_parse)
		wxMessageBox(_("Could not parse style"), _("Could not parse style"), wxOK | wxICON_EXCLAMATION);
}

int confirm_delete(int n, wxWindow *parent, wxString const& title) {
	return wxMessageBox(
		fmt_plural(n, "Are you sure you want to delete this style?", "Are you sure you want to delete these %d styles?", n),
		title, wxYES_NO | wxICON_EXCLAMATION, parent);
}

int get_single_sel(wxListBox *lb) {
	wxArrayInt selections;
	int n = lb->GetSelections(selections);
	return n == 1 ? selections[0] : -1;
}

DialogStyleManager::DialogStyleManager(agi::Context *context)
: wxDialog(context->parent, -1, _("Styles Manager"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(context)
, commit_connection(c->ass->AddCommitListener(&DialogStyleManager::LoadCurrentStyles, this))
, active_line_connection(c->selectionController->AddActiveLineListener(&DialogStyleManager::OnActiveLineChanged, this))
, font_list(std::async(std::launch::async, []() -> wxArrayString {
	wxArrayString fontList = wxFontEnumerator::GetFacenames();
	fontList.Sort();
	return fontList;
}))
{
	using std::bind;
	SetIcons(GETICONS(style_toolbutton));

	// Static box sizers
	wxStaticBoxSizer *CatalogSizer = new wxStaticBoxSizer(wxHORIZONTAL,this,_("Catalog of available storages"));
	wxStaticBoxSizer *StorageSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Storage"));
	wxStaticBoxSizer *CurrentSizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Current script"));

	wxWindow *CatalogSizerBox = CatalogSizer->GetStaticBox();
	wxWindow *StorageSizerBox = StorageSizer->GetStaticBox();
	wxWindow *CurrentSizerBox = CurrentSizer->GetStaticBox();

	// Catalog
	CatalogList = new wxComboBox(CatalogSizerBox,-1, "", wxDefaultPosition, wxSize(-1,-1), 0, nullptr, wxCB_READONLY);
	wxButton *CatalogNew = new wxButton(CatalogSizerBox, -1, _("New"));
	CatalogDelete = new wxButton(CatalogSizerBox, -1, _("Delete"));
	TogglePreview = new wxButton(CatalogSizerBox, -1, to_wx("效果预览"));
	TogglePreview->SetToolTip(to_wx("切换样式列表显示模式：在纯文本列表与实际字幕效果预览之间切换"));

	CatalogSizer->Add(CatalogList,1,wxEXPAND | wxRIGHT,5);
	CatalogSizer->Add(CatalogNew,0,wxRIGHT,5);
	CatalogSizer->Add(CatalogDelete,0,wxRIGHT,15);
	CatalogSizer->Add(TogglePreview,0,wxALIGN_CENTER_VERTICAL,0);

	// Storage styles list
	wxSizer *StorageButtons = make_edit_buttons(StorageSizerBox, _("Copy to &current script ->"), &MoveToLocal, &StorageNew, &StorageEdit, &StorageCopy, &StorageDelete);

	wxSizer *StorageListSizer = new wxBoxSizer(wxHORIZONTAL);
	StorageList = new StyleListBox(StorageSizerBox, -1, wxDefaultPosition, FromDIP(wxSize(280,260)), wxLB_EXTENDED,
		[this](std::string const& name) -> const AssStyle* { return Store.GetStyle(name); });
	StorageListSizer->Add(StorageList,1,wxEXPAND,0);
	StorageListSizer->Add(make_move_buttons(StorageSizerBox, &StorageMoveUp, &StorageMoveDown, &StorageMoveTop, &StorageMoveBottom, &StorageSort), wxSizerFlags().Expand());

	StorageSizer->Add(StorageListSizer,1,wxEXPAND | wxBOTTOM,5);
	StorageSizer->Add(MoveToLocal,0,wxEXPAND | wxBOTTOM,5);
	StorageSizer->Add(StorageButtons,0,wxEXPAND,0);

	// Local styles list
	wxButton *CurrentImport = new wxButton(CurrentSizerBox, -1, _("&Import from script..."));
	wxSizer *CurrentButtons = make_edit_buttons(CurrentSizerBox, _("<- Copy to &storage"), &MoveToStorage, &CurrentNew, &CurrentEdit, &CurrentCopy, &CurrentDelete);
	CurrentPurge = new wxButton(CurrentSizerBox, -1, to_wx("删除未使用的样式"));
	CurrentPurge->SetToolTip(to_wx("删除所有没有被任何字幕行（含注释行）引用、也没有被 {\\r样式名} 标签引用的样式"));

	wxSizer *MoveImportSizer = new wxBoxSizer(wxHORIZONTAL);
	MoveImportSizer->Add(MoveToStorage,1,wxEXPAND | wxRIGHT,5);
	MoveImportSizer->Add(CurrentImport,1,wxEXPAND,0);

	wxSizer *CurrentListSizer = new wxBoxSizer(wxHORIZONTAL);
	CurrentList = new StyleListBox(CurrentSizerBox, -1, wxDefaultPosition, FromDIP(wxSize(280,260)), wxLB_EXTENDED,
		[this](std::string const& name) -> const AssStyle* { return c->ass->GetStyle(name); });
	CurrentListSizer->Add(CurrentList,1,wxEXPAND,0);
	CurrentListSizer->Add(make_move_buttons(CurrentSizerBox, &CurrentMoveUp, &CurrentMoveDown, &CurrentMoveTop, &CurrentMoveBottom, &CurrentSort), wxSizerFlags().Expand());

	CurrentSizer->Add(CurrentListSizer,1,wxEXPAND | wxBOTTOM,5);
	CurrentSizer->Add(MoveImportSizer,0,wxEXPAND | wxBOTTOM,5);
	CurrentSizer->Add(CurrentPurge,0,wxEXPAND | wxBOTTOM,5);
	CurrentSizer->Add(CurrentButtons,0,wxEXPAND,0);

	// Buttons
	wxStdDialogButtonSizer *buttonSizer = CreateStdDialogButtonSizer(wxCANCEL | wxHELP);
	buttonSizer->GetCancelButton()->SetLabel(_("Close"));
	Bind(wxEVT_BUTTON, bind(&HelpButton::OpenPage, "Styles Manager"), wxID_HELP);

	// General layout
	wxSizer *StylesSizer = new wxBoxSizer(wxHORIZONTAL);
	StylesSizer->Add(StorageSizer,1,wxRIGHT | wxEXPAND,5);
	StylesSizer->Add(CurrentSizer,1,wxEXPAND,0);
	wxSizer *MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(CatalogSizer,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP,5);
	MainSizer->Add(StylesSizer,1,wxEXPAND | wxALL,5);
	MainSizer->Add(buttonSizer,0,wxBOTTOM | wxEXPAND,5);

	SetSizerAndFit(MainSizer);

	// Position window
	persist = std::make_unique<PersistLocation>(this, "Tool/Style Manager");

	// Populate lists
	LoadCatalog();
	LoadCurrentStyles(AssFile::COMMIT_STYLES | AssFile::COMMIT_DIAG_META);

	//Set key handlers for lists
	CatalogList->Bind(wxEVT_KEY_DOWN, &DialogStyleManager::OnKeyDown, this);
	StorageList->Bind(wxEVT_KEY_DOWN, &DialogStyleManager::OnKeyDown, this);
	CurrentList->Bind(wxEVT_KEY_DOWN, &DialogStyleManager::OnKeyDown, this);

	StorageMoveUp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(true, 0); });
	StorageMoveTop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(true, 1); });
	StorageMoveDown->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(true, 2); });
	StorageMoveBottom->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(true, 3); });
	StorageSort->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(true, 4); });

	CurrentMoveUp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(false, 0); });
	CurrentMoveTop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(false, 1); });
	CurrentMoveDown->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(false, 2); });
	CurrentMoveBottom->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(false, 3); });
	CurrentSort->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { MoveStyles(false, 4); });

	CatalogNew->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCatalogNew(); });
	CatalogDelete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCatalogDelete(); });

	StorageNew->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnStorageNew(); });
	StorageEdit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnStorageEdit(); });
	StorageCopy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnStorageCopy(); });
	StorageDelete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnStorageDelete(); });

	CurrentNew->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCurrentNew(); });
	CurrentEdit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCurrentEdit(); });
	CurrentCopy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCurrentCopy(); });
	CurrentDelete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCurrentDelete(); });
	CurrentPurge->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCurrentPurge(); });

	CurrentImport->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCurrentImport(); });

	MoveToLocal->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCopyToCurrent(); });
	MoveToStorage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCopyToStorage(); });

	CatalogList->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { OnChangeCatalog(); });

	StorageList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { UpdateButtons(); });
	StorageList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { OnStorageEdit(); });

	CurrentList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { UpdateButtons(); });
	CurrentList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { OnCurrentEdit(); });

	auto update_preview_label = [this]() {
		bool p = StorageList->GetPreviewMode();
		TogglePreview->SetLabel(p ? to_wx("纯文本列表") : to_wx("效果预览"));
	};

	TogglePreview->Bind(wxEVT_BUTTON, [this, update_preview_label](wxCommandEvent&) {
		bool next = !StorageList->GetPreviewMode();
		StorageList->SetPreviewMode(next);
		CurrentList->SetPreviewMode(next);
		update_preview_label();
		try {
			OPT_SET("Tool/Style Manager/Preview")->SetBool(next);
		} catch (...) {}
	});

	bool init_prev = false;
	try {
		init_prev = OPT_GET("Tool/Style Manager/Preview")->GetBool();
	} catch (...) {}
	if (init_prev) {
		StorageList->SetPreviewMode(true);
		CurrentList->SetPreviewMode(true);
		update_preview_label();
	}
}

void DialogStyleManager::LoadCurrentStyles(int commit_type) {
	if (commit_type & AssFile::COMMIT_STYLES || commit_type == AssFile::COMMIT_NEW) {
		CurrentList->Clear();
		styleMap.clear();

		for (auto& style : c->ass->Styles) {
			CurrentList->Append(to_wx(style.name));
			styleMap.push_back(&style);
		}
	}

	if (commit_type & AssFile::COMMIT_DIAG_META) {
		AssDialogue *dia = c->selectionController->GetActiveLine();
		CurrentList->DeselectAll();
		if (dia && commit_type != AssFile::COMMIT_NEW)
			CurrentList->SetStringSelection(to_wx(dia->Style));
		else
			CurrentList->SetSelection(0);
	}

	UpdateButtons();
}

void DialogStyleManager::OnActiveLineChanged(AssDialogue *new_line) {
	if (new_line) {
		CurrentList->DeselectAll();
		CurrentList->SetStringSelection(to_wx(new_line->Style));
		UpdateButtons();
	}
}

void DialogStyleManager::UpdateStorage() {
	Store.Save();

	StorageList->Clear();
	StorageList->Append(to_wx(Store.GetNames()));

	UpdateButtons();
}

void DialogStyleManager::OnChangeCatalog() {
	std::string catalog(from_wx(CatalogList->GetStringSelection()));
	c->ass->Properties.style_storage = catalog;
	Store.LoadCatalog(catalog);
	UpdateStorage();
}

void DialogStyleManager::LoadCatalog() {
	CatalogList->Clear();

	// Get saved style catalogs
	auto catalogs = AssStyleStorage::GetCatalogs();
	for (auto const& c : catalogs)
		CatalogList->Append(to_wx(c));

	// Create a default storage if there are none
	if (CatalogList->IsListEmpty()) {
		Store.LoadCatalog("Default");
		Store.push_back(std::make_unique<AssStyle>());
		Store.Save();
		CatalogList->Append("Default");
	}

	// Set to default if available
	std::string pickStyle = c->ass->Properties.style_storage;
	if (pickStyle.empty())
		pickStyle = "Default";

	int opt = CatalogList->FindString(to_wx(pickStyle), false);
	CatalogList->SetSelection(opt == wxNOT_FOUND ? 0 : opt);

	OnChangeCatalog();
}

void DialogStyleManager::OnCatalogNew() {
	wxString name = wxGetTextFromUser(_("New storage name:"), _("New catalog entry"), "", this);
	if (!name) return;

	// Remove bad characters from the name
	wxString badchars = wxFileName::GetForbiddenChars(wxPATH_DOS);
	int badchars_removed = 0;
	for (wxUniCharRef chr : name) {
		if (badchars.find(chr) != badchars.npos) {
			chr = '_';
			++badchars_removed;
		}
	}

	// Make sure that there is no storage with the same name (case insensitive search since Windows filenames are case insensitive)
	if (CatalogList->FindString(name, false) != wxNOT_FOUND) {
		wxMessageBox(_("A catalog with that name already exists."), _("Catalog name conflict"), wxOK | wxICON_ERROR | wxCENTER);
		return;
	}

	// Warn about bad characters
	if (badchars_removed) {
		wxMessageBox(
			fmt_tl("The specified catalog name contains one or more illegal characters. They have been replaced with underscores instead.\nThe catalog has been renamed to \"%s\".", name),
			_("Invalid characters"));
	}

	// Add to list of storages
	CatalogList->Append(name);
	CatalogList->SetStringSelection(name);
	OnChangeCatalog();
}

void DialogStyleManager::OnCatalogDelete() {
	if (CatalogList->GetCount() == 1) return;

	wxString name = CatalogList->GetStringSelection();
	wxString message = fmt_tl("Are you sure you want to delete the storage \"%s\" from the catalog?", name);
	int option = wxMessageBox(message, _("Confirm delete"), wxYES_NO | wxICON_EXCLAMATION , this);
	if (option == wxYES) {
		agi::fs::Remove(config::path->Decode("?user/catalog/" + from_wx(name) + ".sty"));
		CatalogList->Delete(CatalogList->GetSelection());
		CatalogList->SetSelection(0);
		OnChangeCatalog();
	}
}

void DialogStyleManager::OnCopyToStorage() {
	wxArrayInt selections;
	int n = CurrentList->GetSelections(selections);
	wxArrayString copied;
	copied.reserve(n);
	for (int i = 0; i < n; i++) {
		wxString styleName = CurrentList->GetString(selections[i]);

		if (AssStyle *style = Store.GetStyle(from_wx(styleName))) {
			if (wxYES == wxMessageBox(fmt_tl("There is already a style with the name \"%s\" in the current storage. Overwrite?", styleName), _("Style name collision"), wxYES_NO)) {
				*style = *styleMap.at(selections[i]);
				copied.push_back(styleName);
			}
		}
		else {
			Store.push_back(std::make_unique<AssStyle>(*styleMap.at(selections[i])));
			copied.push_back(styleName);
		}
	}

	UpdateStorage();
	for (auto const& style_name : copied)
		StorageList->SetStringSelection(style_name, true);

	UpdateButtons();
}

void DialogStyleManager::OnCopyToCurrent() {
	wxArrayInt selections;
	int n = StorageList->GetSelections(selections);
	wxArrayString copied;
	copied.reserve(n);
	for (int i = 0; i < n; i++) {
		wxString styleName = StorageList->GetString(selections[i]);

		if (AssStyle *style = c->ass->GetStyle(from_wx(styleName))) {
			if (wxYES == wxMessageBox(fmt_tl("There is already a style with the name \"%s\" in the current script. Overwrite?", styleName), _("Style name collision"), wxYES_NO)) {
				*style = *Store[selections[i]];
				copied.push_back(styleName);
			}
		}
		else {
			c->ass->Styles.push_back(*new AssStyle(*Store[selections[i]]));
			copied.push_back(styleName);
		}
	}

	c->ass->Commit(_("style copy"), AssFile::COMMIT_STYLES);

	CurrentList->DeselectAll();
	for (auto const& style_name : copied)
		CurrentList->SetStringSelection(style_name, true);
	UpdateButtons();
}

template<class T>
void DialogStyleManager::CopyToClipboard(wxListBox *list, T const& v) {
	wxArrayInt selections;
	list->GetSelections(selections);

	std::string data;
	for(size_t i = 0; i < selections.size(); ++i) {
		if (i) data += "\r\n";
		AssStyle *s = v[selections[i]];
		s->UpdateData();
		data += s->GetEntryData();
	}

	SetClipboard(data);
}

void DialogStyleManager::PasteToCurrent() {
	add_styles(
		[this](std::string const& str) { return c->ass->GetStyle(str); },
		[this](AssStyle *s) { c->ass->Styles.push_back(*s); });

	c->ass->Commit(_("style paste"), AssFile::COMMIT_STYLES);
}

void DialogStyleManager::PasteToStorage() {
	add_styles(
		[this](std::string const& str) { return Store.GetStyle(str); },
		[this](AssStyle *s) { Store.push_back(std::unique_ptr<AssStyle>(s)); });

	UpdateStorage();
	StorageList->SetStringSelection(to_wx(Store.back()->name));
	UpdateButtons();
}

void DialogStyleManager::ShowStorageEditor(AssStyle *style, std::string const& new_name) {
	DialogStyleEditor editor(this, style, c, &Store, new_name, font_list.get());
	if (editor.ShowModal()) {
		UpdateStorage();
		StorageList->SetStringSelection(to_wx(editor.GetStyleName()));
		UpdateButtons();
	}
}

void DialogStyleManager::OnStorageNew() {
	ShowStorageEditor(nullptr);
}

void DialogStyleManager::OnStorageEdit() {
	int sel = get_single_sel(StorageList);
	if (sel == -1) return;
	ShowStorageEditor(Store[sel]);
}

void DialogStyleManager::OnStorageCopy() {
	int sel = get_single_sel(StorageList);
	if (sel == -1) return;

	ShowStorageEditor(Store[sel], unique_name(
		[this](std::string const& str) { return Store.GetStyle(str); }, Store[sel]->name));
}

void DialogStyleManager::OnStorageDelete() {
	wxArrayInt selections;
	int n = StorageList->GetSelections(selections);

	if (confirm_delete(n, this, _("Confirm delete from storage")) == wxYES) {
		for (int i = 0; i < n; i++)
			Store.Delete(selections[i] - i);
		UpdateStorage();
	}
}

void DialogStyleManager::ShowCurrentEditor(AssStyle *style, std::string const& new_name) {
	DialogStyleEditor editor(this, style, c, nullptr, new_name, font_list.get());
	if (editor.ShowModal()) {
		CurrentList->DeselectAll();
		CurrentList->SetStringSelection(to_wx(editor.GetStyleName()));
		UpdateButtons();
	}
}

void DialogStyleManager::OnCurrentNew() {
	ShowCurrentEditor(nullptr);
}

void DialogStyleManager::OnCurrentEdit() {
	int sel = get_single_sel(CurrentList);
	if (sel == -1) return;
	ShowCurrentEditor(styleMap[sel]);
}

void DialogStyleManager::OnCurrentCopy() {
	int sel = get_single_sel(CurrentList);
	if (sel == -1) return;

	ShowCurrentEditor(styleMap[sel], unique_name(
		[this](std::string const& str) { return c->ass->GetStyle(str); },
		styleMap[sel]->name));
}

void DialogStyleManager::OnCurrentDelete() {
	wxArrayInt selections;
	int n = CurrentList->GetSelections(selections);

	if (confirm_delete(n, this, _("Confirm delete from current")) == wxYES) {
		for (int i = 0; i < n; i++) {
			delete styleMap.at(selections[i]);
		}
		c->ass->Commit(_("style delete"), AssFile::COMMIT_STYLES);
	}
}

void DialogStyleManager::OnCurrentPurge() {
	// Collect the style names this file still references. Commented-out
	// lines count too, because uncommenting one later should find its style
	// intact. Styles reached only through inline override tags ({\r名称})
	// are referenced as well.
	std::set<std::string> used;
	for (auto& event : c->ass->Events) {
		used.insert(event.Style.get());
		std::string const& text = event.Text.get();
		for (size_t pos = text.find("\\r"); pos != std::string::npos; pos = text.find("\\r", pos + 2)) {
			size_t end = text.find_first_of("}\\", pos + 2);
			if (end != pos + 2)
				used.insert(text.substr(pos + 2, end == std::string::npos ? std::string::npos : end - pos - 2));
		}
	}

	std::vector<AssStyle*> doomed;
	std::vector<std::string> doomed_names;
	for (auto& style : c->ass->Styles) {
		// "Default" is the fallback every file needs and lines can name
		// without referencing it in text, so keep it unconditionally.
		if (style.name == "Default" || used.count(style.name)) continue;
		doomed.push_back(&style);
		doomed_names.push_back(style.name);
	}

	if (doomed.empty()) {
		wxMessageBox(to_wx("这个字幕文件里没有未使用的样式。"), to_wx("删除未使用样式"), wxOK | wxICON_INFORMATION, this);
		return;
	}

	// Never empty the style list: the file must retain at least one style.
	if (doomed.size() >= c->ass->Styles.size()) {
		wxMessageBox(to_wx("这些样式虽然都未被引用，但至少要为一个字幕文件保留一个样式，未执行删除。"), to_wx("删除未使用样式"), wxOK | wxICON_INFORMATION, this);
		return;
	}

	wxString names;
	for (size_t i = 0; i < doomed_names.size(); ++i) {
		names += (i ? "、" : "") + to_wx(doomed_names[i]);
		if (i >= 19) { names += "…"; break; }
	}
	if (wxMessageBox(fmt_wx("确定要删除 %d 个未被引用的样式吗？\n\n%s\n\n删除后仍可通过撤销恢复。", static_cast<int>(doomed.size()), names),
		to_wx("删除未使用样式"), wxYES_NO | wxICON_EXCLAMATION, this) != wxYES)
		return;

	for (AssStyle *style : doomed)
		delete style;
	c->ass->Commit(to_wx("删除未使用的样式"), AssFile::COMMIT_STYLES);
}

void DialogStyleManager::OnCurrentImport() {
	auto filename = OpenFileSelector(c, _("Open Subtitles File"), "Path/Last/Subtitles", "", "", SubtitleFormat::GetWildcards(0), this);
	if (filename.empty()) return;

	std::string charset;
	try {
		charset = CharSetDetect::GetEncoding(filename);
	}
	catch (agi::UserCancelException const&) {
		return;
	}

	AssFile temp;
	try {
		auto reader = SubtitleFormat::GetReader(filename, charset.c_str());
		if (!reader) {
			wxMessageBox(_("Unsupported subtitle format"), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
			return;
		}
		reader->ReadFile(&temp, filename, 0, charset.c_str());
	}
	catch (agi::Exception const& err) {
		wxMessageBox(to_wx(err.GetMessage()), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
	}
	catch (...) {
		wxMessageBox(_("Unknown error"), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
		return;
	}

	// Get styles
	auto styles = temp.GetStyles();
	if (styles.empty()) {
		wxMessageBox(_("The selected file has no available styles."), _("Error Importing Styles"));
		return;
	}

	// Get selection
	wxArrayInt selections;
	int res = GetSelectedChoices(this, selections, _("Choose styles to import:"), _("Import Styles"), to_wx(styles));
	if (res == -1 || selections.empty()) return;
	bool modified = false;

	// Loop through selection
	for (auto const& sel : selections) {
		// Check if there is already a style with that name
		if (AssStyle *existing = c->ass->GetStyle(styles[sel])) {
			int answer = wxMessageBox(
				fmt_tl("There is already a style with the name \"%s\" in the current script. Overwrite?", styles[sel]),
				_("Style name collision"),
				wxYES_NO);
			if (answer == wxYES) {
				modified = true;
				*existing = *temp.GetStyle(styles[sel]);
			}
			continue;
		}

		// Copy
		modified = true;
		c->ass->Styles.push_back(*new AssStyle(*temp.GetStyle(styles[sel])));
	}

	// Update
	if (modified)
		c->ass->Commit(_("style import"), AssFile::COMMIT_STYLES);
}

void DialogStyleManager::UpdateButtons() {
	CatalogDelete->Enable(CatalogList->GetCount() > 1);

	// Get storage selection
	wxArrayInt sels;
	int n = StorageList->GetSelections(sels);

	StorageEdit->Enable(n == 1);
	StorageCopy->Enable(n == 1);
	StorageDelete->Enable(n > 0);
	MoveToLocal->Enable(n > 0);

	int firstStor = -1;
	int lastStor = -1;
	if (n) {
		firstStor = sels[0];
		lastStor = sels[n-1];
	}

	// Check if selection is continuous
	bool contStor = true;
	for (int i = 1; i < n; ++i) {
		if (sels[i] != sels[i-1]+1) {
			contStor = false;
			break;
		}
	}

	int itemsStor = StorageList->GetCount();
	StorageMoveUp->Enable(contStor && firstStor > 0);
	StorageMoveTop->Enable(contStor && firstStor > 0);
	StorageMoveDown->Enable(contStor && lastStor != -1 && lastStor < itemsStor-1);
	StorageMoveBottom->Enable(contStor && lastStor != -1 && lastStor < itemsStor-1);
	StorageSort->Enable(itemsStor > 1);

	// Get current selection
	n = CurrentList->GetSelections(sels);

	CurrentEdit->Enable(n == 1);
	CurrentCopy->Enable(n == 1);
	CurrentDelete->Enable(n > 0);
	MoveToStorage->Enable(n > 0);

	int firstCurr = -1;
	int lastCurr = -1;
	if (n) {
		firstCurr = sels[0];
		lastCurr = sels[n-1];
	}

	// Check if selection is continuous
	bool contCurr = true;
	for (int i = 1; i < n; ++i) {
		if (sels[i] != sels[i-1]+1) {
			contCurr = false;
			break;
		}
	}

	int itemsCurr = CurrentList->GetCount();
	CurrentMoveUp->Enable(contCurr && firstCurr > 0);
	CurrentMoveTop->Enable(contCurr && firstCurr > 0);
	CurrentMoveDown->Enable(contCurr && lastCurr != -1 && lastCurr < itemsCurr-1);
	CurrentMoveBottom->Enable(contCurr && lastCurr != -1 && lastCurr < itemsCurr-1);
	CurrentSort->Enable(itemsCurr > 1);
}

struct cmp_name {
	template<typename T>
	bool operator()(T const& lft, T const& rgt) const { return lft->name < rgt->name; }
};

template<class Cont>
static void do_move(Cont& styls, int type, int& first, int& last, bool storage) {
	auto begin = styls.begin();

	// Move up
	if (type == 0) {
		if (first == 0) return;
		rotate(begin + first - 1, begin + first, begin + last + 1);
		first--;
		last--;
	}
	// Move to top
	else if (type == 1) {
		rotate(begin, begin + first, begin + last + 1);
		last = last - first;
		first = 0;
	}
	// Move down
	else if (type == 2) {
		if (last + 1 == (int)styls.size()) return;
		rotate(begin + first, begin + last + 1, begin + last + 2);
		first++;
		last++;
	}
	// Move to bottom
	else if (type == 3) {
		rotate(begin + first, begin + last + 1, styls.end());
		first = styls.size() - (last - first + 1);
		last = styls.size() - 1;
	}
	// Sort
	else if (type == 4) {
		// Get confirmation
		if (storage) {
			int res = wxMessageBox(_("Are you sure? This cannot be undone!"), _("Sort styles"), wxYES_NO | wxCENTER);
			if (res == wxNO) return;
		}

		sort(styls.begin(), styls.end(), cmp_name());

		first = 0;
		last = 0;
	}
}

void DialogStyleManager::MoveStyles(bool storage, int type) {
	wxListBox *list = storage ? StorageList : CurrentList;

	// Get selection
	wxArrayInt sels;
	int n = list->GetSelections(sels);
	if (n == 0 && type != 4) return;

	int first = 0, last = 0;
	if (n) {
		first = sels.front();
		last = sels.back();
	}

	if (storage) {
		do_move(Store, type, first, last, true);
		UpdateStorage();
	}
	else {
		do_move(styleMap, type, first, last, false);

		// Replace styles
		size_t curn = 0;
		for (auto it = c->ass->Styles.begin(); it != c->ass->Styles.end(); ++it) {
			auto new_style_at_pos = c->ass->Styles.iterator_to(*styleMap[curn]);
			EntryList<AssStyle>::node_algorithms::swap_nodes(it.pointed_node(), new_style_at_pos.pointed_node());
			if (++curn == styleMap.size()) break;
			it = new_style_at_pos;
		}

		c->ass->Commit(_("style move"), AssFile::COMMIT_STYLES);
	}

	for (int i = 0 ; i < (int)list->GetCount(); ++i) {
		if (i < first || i > last)
			list->Deselect(i);
		else
			list->Select(i);
	}

	UpdateButtons();
}

void DialogStyleManager::OnKeyDown(wxKeyEvent &event) {
	wxWindow *focus = wxWindow::FindFocus();

	switch(event.GetKeyCode()) {
		case WXK_DELETE :
			if (focus == StorageList)
				OnStorageDelete();
			else if (focus == CurrentList)
				OnCurrentDelete();
			break;

		case 'C' :
		case 'c' :
			if (event.CmdDown()) {
				if (focus == StorageList)
					CopyToClipboard(StorageList, Store);
				else if (focus == CurrentList)
					CopyToClipboard(CurrentList, styleMap);
			}
			break;

		case 'V' :
		case 'v' :
			if (event.CmdDown()) {
				if (focus == StorageList)
					PasteToStorage();
				else if (focus == CurrentList)
					PasteToCurrent();
			}
			break;
		default:
			event.Skip();
			break;
	}
}
}

void ShowStyleManagerDialog(agi::Context *c) {
	c->dialog->Show<DialogStyleManager>(c);
}
