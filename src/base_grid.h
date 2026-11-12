// Copyright (c) 2006, Rodrigo Braz Monteiro
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

#pragma once

#include <libaegisub/signal.h>

#include <memory>
#include <string>
#include <vector>
#include <wx/brush.h>
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include <wx/window.h>

namespace agi {
	struct Context;
	class OptionValue;
}
class AssDialogue;
class GridColumn;
class WidthHelper;

class BaseGrid final : public wxWindow {
	std::vector<agi::signal::Connection> connections;
	int lineHeight = 1;     ///< Height of a line in pixels in the current font
	bool holding = false;   ///< Is a drag selection in process?
	int scrollWheelProgress  = 0;	///< How close we are to reaching a full mouse wheel step
	wxFont font;            ///< Current grid font
	wxScrollBar *scrollBar; ///< The grid's scrollbar
	bool byFrame = false;   ///< Should times be displayed as frame numbers

	/// Row from which the selection shrinks/grows from when selecting via the
	/// keyboard, shift-clicking or dragging
	int extendRow = -1;

	/// First row that is visible at the current scroll position
	int yPos = 0;

	int active_row = -1;
	int playback_row = -1;
	bool review_mode = false;
	bool overlap_highlight = false;

	bool filter_active = false;
	int filter_field = 0; // 0: 全部, 1: 文本, 2: 说话人, 3: 样式
	wxString filter_query;

	std::unique_ptr<WidthHelper> width_helper;

	/// Rows which are visible on the current video frame
	std::vector<int> visible_rows;
	std::vector<bool> overlap_rows;

	agi::Context *context; ///< Associated project context

	std::vector<std::unique_ptr<GridColumn>> columns;
	std::vector<bool> columns_visible;

	std::vector<wxRect> text_refresh_rects;

	/// Cached brushes used for row backgrounds
	struct {
		wxBrush Default;
		wxBrush Header;
		wxBrush Selection;
		wxBrush Comment;
		wxBrush Visible;
		wxBrush SelectedComment;
		wxBrush Playback;
		wxBrush Overlap;
		wxBrush OverlapSelected;
		wxBrush LeftCol;
	} row_colors;

	std::vector<AssDialogue*> index_line_map;  ///< Row number -> dialogue line

	/// Connection for video seek event. Stored explicitly so that it can be
	/// blocked if the relevant option is disabled
	agi::signal::Connection seek_listener;

	/// Cached grid body context menu
	std::unique_ptr<wxMenu> context_menu;

	void OnDPIChanged(wxDPIChangedEvent &e);
	void OnContextMenu(wxContextMenuEvent &evt);
	void OnHighlightVisibleChange(agi::OptionValue const& opt);
	void OnKeyDown(wxKeyEvent &event);
	void OnCharHook(wxKeyEvent &event);
	void OnMouseEvent(wxMouseEvent &event);
	void OnPaint(wxPaintEvent &event);
	void OnScroll(wxScrollEvent &event);
	void OnShowColMenu(wxCommandEvent &event);
	void OnSize(wxSizeEvent &event);
	void OnSubtitlesCommit(int type);
	void OnActiveLineChanged(AssDialogue *);
	void OnSeek();
	void OnReviewModeChanged(bool active);

	void AdjustScrollbar();
	void SetColumnWidths();

	bool IsDisplayed(const AssDialogue *line) const;

	void UpdateMaps();
	void UpdateOverlapRows();
	void UpdateStyle();

	void SelectRow(int row, bool addToSelected = false, bool select=true);

	void MakeRowVisible(int row);

	/// @brief Get dialogue by index
	/// @param n Index to look up
	/// @return Subtitle dialogue line for index, or 0 if invalid index
	AssDialogue *GetDialogue(int n) const;

public:
	BaseGrid(wxWindow* parent, agi::Context *context);
	~BaseGrid();

	int GetRows() const { return static_cast<int>(index_line_map.size()); }
	void SetByFrame(bool state);
	void ScrollTo(int y);
	void SetOverlapHighlight(bool enabled);
	bool IsOverlapHighlightEnabled() const { return overlap_highlight; }

	void SetFilter(int field, const wxString& query);
	void ClearFilter();
	bool HasActiveFilter() const { return review_mode && filter_active && !filter_query.empty(); }
	int GetTotalEventsCount() const;

	agi::signal::Signal<int, int> FilterCountsChanged;

	DECLARE_EVENT_TABLE()
};

class wxChoice;
class wxTextCtrl;
class wxStaticText;
class wxButton;

class ReviewFilterBar final : public wxPanel {
	agi::Context *context;
	BaseGrid *grid;
	wxStaticText *label = nullptr;
	wxChoice *field_choice = nullptr;
	wxTextCtrl *query_ctrl = nullptr;
	wxStaticText *count_label = nullptr;
	wxButton *clear_btn = nullptr;
	agi::signal::Connection filter_conn;
	agi::signal::Connection review_conn;

	void UpdateCountLabel(int displayed, int total);
	void OnQueryUpdated();

public:
	ReviewFilterBar(wxWindow *parent, agi::Context *context, BaseGrid *grid);
	~ReviewFilterBar() override;

	void OnReviewModeChanged(bool active);
	void Clear();
};
