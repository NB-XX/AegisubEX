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

#include "base_grid.h"

#include "include/aegisub/context.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_box.h"
#include "compat.h"
#include "grid_column.h"
#include "options.h"
#include "project.h"
#include "utils.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "video_controller.h"
#include "ai_assistant.h"

#include <libaegisub/util.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>

namespace {
	std::set<std::string> s_expanded_groups;
	std::map<const AssDialogue*, int> s_line_to_grid_row;
	wxBrush s_brush_motion_group(wxColour(235, 245, 255));
	wxBrush s_brush_motion_group_selected(wxColour(200, 225, 255));

	int GetRowFromDialogue(const AssDialogue *d) {
		if (!d) return -1;
		auto it = s_line_to_grid_row.find(d);
		if (it != s_line_to_grid_row.end()) return it->second;
		return -1;
	}
}

// Check menu.h for id range allocation before editing this enum
enum {
	GRID_SCROLLBAR = 1730,
	MENU_SHOW_COL = (wxID_HIGHEST + 1) + 2000 // Needs 15 IDs after this
};

BaseGrid::BaseGrid(wxWindow* parent, agi::Context *context)
: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxSUNKEN_BORDER)
, scrollBar(new wxScrollBar(this, GRID_SCROLLBAR, wxDefaultPosition, wxDefaultSize, wxSB_VERTICAL))
, context(context)
, columns(GetGridColumns())
, columns_visible(OPT_GET("Subtitle/Grid/Column")->GetListBool())
, seek_listener(context->videoController->AddSeekListener(&BaseGrid::OnSeek, this))
{
	scrollBar->SetScrollbar(0,10,100,10);

	auto scrollbarpositioner = new wxBoxSizer(wxHORIZONTAL);
	scrollbarpositioner->AddStretchSpacer();
	scrollbarpositioner->Add(scrollBar, 0, wxEXPAND, 0);

	SetSizerAndFit(scrollbarpositioner);

	SetBackgroundStyle(wxBG_STYLE_PAINT);

	for (size_t i : agi::util::range(std::min(columns_visible.size(), columns.size()))) {
		if (!columns_visible[i])
			columns[i]->SetVisible(false);
	}

	UpdateStyle();
	OnHighlightVisibleChange(*OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame"));

	connections = agi::signal::make_vector({
		context->ass->AddCommitListener(&BaseGrid::OnSubtitlesCommit, this),

		context->selectionController->AddActiveLineListener(&BaseGrid::OnActiveLineChanged, this),
		context->selectionController->AddSelectionListener([&]{ Refresh(false); }),
		context->videoController->AddReviewModeListener(&BaseGrid::OnReviewModeChanged, this),

		OPT_SUB("Subtitle/Grid/Font Face", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Subtitle/Grid/Font Size", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Active Border", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Background", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Comment", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Inframe", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Selected Comment", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Background/Selection", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Collision", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Header", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Left Column", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Lines", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Selection", &BaseGrid::UpdateStyle, this),
		OPT_SUB("Colour/Subtitle Grid/Standard", &BaseGrid::UpdateStyle, this),

		OPT_SUB("Subtitle/Grid/Highlight Subtitles in Frame", &BaseGrid::OnHighlightVisibleChange, this),
		OPT_SUB("Subtitle/Grid/Hide Overrides", [&](agi::OptionValue const&) { Refresh(false); }),
	});

	Bind(wxEVT_CONTEXT_MENU, &BaseGrid::OnContextMenu, this);
}

BaseGrid::~BaseGrid() { }

BEGIN_EVENT_TABLE(BaseGrid,wxWindow)
	EVT_PAINT(BaseGrid::OnPaint)
	EVT_SIZE(BaseGrid::OnSize)
	EVT_COMMAND_SCROLL(GRID_SCROLLBAR,BaseGrid::OnScroll)
	EVT_MOUSE_EVENTS(BaseGrid::OnMouseEvent)
	EVT_KEY_DOWN(BaseGrid::OnKeyDown)
	EVT_CHAR_HOOK(BaseGrid::OnCharHook)
	EVT_MENU_RANGE(MENU_SHOW_COL,MENU_SHOW_COL+15,BaseGrid::OnShowColMenu)
	EVT_DPI_CHANGED(BaseGrid::OnDPIChanged)
END_EVENT_TABLE()

void BaseGrid::OnDPIChanged(wxDPIChangedEvent &e) {
	UpdateStyle();
	e.Skip();
}

void BaseGrid::OnSubtitlesCommit(int type) {
	if (type == AssFile::COMMIT_NEW) {
		s_expanded_groups.clear();
	}
	if (type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_ORDER || type & AssFile::COMMIT_DIAG_ADDREM)
		UpdateMaps();
	else if (type & AssFile::COMMIT_DIAG_TIME)
		UpdateOverlapRows();

	if (type & AssFile::COMMIT_DIAG_META) {
		SetColumnWidths();
		Refresh(false);
		return;
	}
	if (type & AssFile::COMMIT_DIAG_TIME)
		Refresh(false);
	else if (type & AssFile::COMMIT_DIAG_TEXT) {
		for (auto const& rect : text_refresh_rects)
			RefreshRect(rect, false);
	}
}

void BaseGrid::OnShowColMenu(wxCommandEvent &event) {
	int item = event.GetId() - MENU_SHOW_COL;
	bool new_value = !columns_visible[item];

	columns_visible.resize(columns.size(), true);
	columns_visible[item] = new_value;
	OPT_SET("Subtitle/Grid/Column")->SetListBool(columns_visible);
	columns[item]->SetVisible(new_value);

	SetColumnWidths();

	Refresh(false);
}

void BaseGrid::OnHighlightVisibleChange(agi::OptionValue const& opt) {
	if (opt.GetBool())
		seek_listener.Unblock();
	else
		seek_listener.Block();
}

void BaseGrid::UpdateStyle() {
	font = *wxNORMAL_FONT;
	wxString fontname = FontFace("Subtitle/Grid");
	if (!fontname.empty()) font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Grid/Font Size")->GetInt());
	font.SetWeight(wxFONTWEIGHT_NORMAL);

	wxClientDC dc(this);
	dc.SetFont(font);

	// Set line height
	lineHeight = dc.GetCharHeight() + 4;

	// Set row brushes
	row_colors.Default.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor()));
	row_colors.Header.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Header")->GetColor()));
	row_colors.Selection.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
	row_colors.Comment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Comment")->GetColor()));
	row_colors.Visible.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Inframe")->GetColor()));
	row_colors.SelectedComment.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selected Comment")->GetColor()));
	row_colors.LeftCol.SetColour(to_wx(OPT_GET("Colour/Subtitle Grid/Left Column")->GetColor()));
	row_colors.Playback.SetColour(wxColour(255, 222, 150));
	row_colors.Overlap.SetColour(wxColour(255, 218, 222));
	row_colors.OverlapSelected.SetColour(wxColour(232, 133, 140));
	s_brush_motion_group.SetColour(wxColour(235, 245, 255));
	s_brush_motion_group_selected.SetColour(wxColour(200, 225, 255));

	if (width_helper)
		width_helper->ClearCache();

	SetColumnWidths();

	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::UpdateMaps() {
	index_line_map.clear();
	s_line_to_grid_row.clear();

	int current_group_row = -1;
	std::string current_folded_gid;

	for (auto& curdiag : context->ass->Events) {
		std::string const& eff = curdiag.Effect.get();
		if (eff.rfind("motion-group:", 0) == 0) {
			std::string gid = eff.substr(13);
			int row_idx = static_cast<int>(index_line_map.size());
			index_line_map.push_back(&curdiag);
			s_line_to_grid_row[&curdiag] = row_idx;

			bool is_expanded = (s_expanded_groups.find(gid) != s_expanded_groups.end());
			std::string text = curdiag.Text.get();
			if (!is_expanded && text.rfind("▼", 0) == 0) {
				curdiag.Text = "▶" + text.substr(3);
			} else if (is_expanded && text.rfind("▶", 0) == 0) {
				curdiag.Text = "▼" + text.substr(3);
			}

			if (is_expanded) {
				current_folded_gid.clear();
				current_group_row = -1;
			} else {
				current_folded_gid = gid;
				current_group_row = row_idx;
			}
			continue;
		}

		if (eff.rfind("motion-frame:", 0) == 0) {
			std::string gid = eff.substr(13);
			bool is_expanded = (s_expanded_groups.find(gid) != s_expanded_groups.end());
			if (!is_expanded) {
				int group_row = (gid == current_folded_gid && current_group_row >= 0) ? current_group_row : -1;
				s_line_to_grid_row[&curdiag] = group_row;
				continue;
			}
		}

		int row_idx = static_cast<int>(index_line_map.size());
		index_line_map.push_back(&curdiag);
		s_line_to_grid_row[&curdiag] = row_idx;
		current_folded_gid.clear();
		current_group_row = -1;
	}

	UpdateOverlapRows();
	SetColumnWidths();
	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::UpdateOverlapRows() {
	overlap_rows.assign(index_line_map.size(), false);
	std::vector<int> timed_rows;
	timed_rows.reserve(index_line_map.size());
	for (size_t i = 0; i < index_line_map.size(); ++i) {
		auto line = index_line_map[i];
		if (!line->Comment && line->End > line->Start)
			timed_rows.push_back(static_cast<int>(i));
	}
	std::sort(timed_rows.begin(), timed_rows.end(), [&](int a, int b) {
		if (index_line_map[a]->Start != index_line_map[b]->Start)
			return index_line_map[a]->Start < index_line_map[b]->Start;
		return index_line_map[a]->End < index_line_map[b]->End;
	});

	int max_end = std::numeric_limits<int>::min();
	for (size_t i = 0; i < timed_rows.size(); ++i) {
		int row = timed_rows[i];
		auto line = index_line_map[row];
		int line_start = line->Start;
		int line_end = line->End;
		bool overlaps_earlier = max_end > line_start;
		bool overlaps_later = i + 1 < timed_rows.size() && static_cast<int>(index_line_map[timed_rows[i + 1]]->Start) < line_end;
		overlap_rows[row] = overlaps_earlier || overlaps_later;
		max_end = std::max(max_end, line_end);
	}
}

void BaseGrid::OnActiveLineChanged(AssDialogue *new_active) {
	if (new_active) {
		int grid_row = GetRowFromDialogue(new_active);
		if (grid_row >= 0) {
			if (grid_row != active_row)
				MakeRowVisible(grid_row);
			extendRow = active_row = grid_row;
		}
		Refresh(false);
	}
	else
		active_row = -1;
}

void BaseGrid::MakeRowVisible(int row) {
	int h = GetClientSize().GetHeight();

	if (row < yPos + 1)
		ScrollTo(row - 1);
	else if (row > yPos + h/lineHeight - 3)
		ScrollTo(row - h/lineHeight + 3);
}

void BaseGrid::SelectRow(int row, bool addToSelected, bool select) {
	if (row < 0 || (size_t)row >= index_line_map.size()) return;

	AssDialogue *line = index_line_map[row];

	if (!addToSelected) {
		context->selectionController->SetSelectedSet(Selection{line});
		return;
	}

	bool selected = !!context->selectionController->GetSelectedSet().count(line);
	if (select != selected) {
		auto selection = context->selectionController->GetSelectedSet();
		if (select)
			selection.insert(line);
		else
			selection.erase(line);
		context->selectionController->SetSelectedSet(std::move(selection));
	}
}

void BaseGrid::OnSeek() {
	int new_playback_row = -1;
	if (review_mode && context->project->VideoProvider()) {
		int time = context->videoController->TimeAtFrame(context->videoController->GetFrameN(), agi::vfr::EXACT);
		for (size_t i = 0; i < index_line_map.size(); ++i) {
			auto line = index_line_map[i];
			bool is_motion_group = (line->Effect.get().rfind("motion-group:", 0) == 0);
			if ((!line->Comment || is_motion_group) && line->Start <= time && time < line->End) {
				new_playback_row = static_cast<int>(i);
				break;
			}
		}
	}
	bool playback_changed = new_playback_row != playback_row;
	playback_row = new_playback_row;
	int lines = GetClientSize().GetHeight() / lineHeight + 1;
	lines = mid(0, lines, GetRows() - yPos);

	auto it = begin(visible_rows);
	for (int i : boost::irange(yPos, yPos + lines)) {
		if (IsDisplayed(index_line_map[i])) {
			if (it == end(visible_rows) || *it != i) {
				Refresh(false);
				return;
			}
			++it;
		}
	}
	if (it != end(visible_rows) || playback_changed)
		Refresh(false);
}

void BaseGrid::OnReviewModeChanged(bool active) {
	review_mode = active;
	if (!active) overlap_highlight = false;
	if (!active) playback_row = -1;
	else OnSeek();
	Refresh(false);
}

void BaseGrid::SetOverlapHighlight(bool enabled) {
	if (!review_mode || overlap_highlight == enabled) return;
	overlap_highlight = enabled;
	Refresh(false);
}

void BaseGrid::OnPaint(wxPaintEvent &) {
	// Find which columns need to be repainted
	std::vector<char> paint_columns;
	paint_columns.resize(columns.size(), false);
	bool any = false;
	for (wxRegionIterator region(GetUpdateRegion()); region; ++region) {
		wxRect updrect = region.GetRect();
		int x = 0;
		for (size_t i : agi::util::range(columns.size())) {
			int width = columns[i]->Width();
			if (width && updrect.x < x + width && updrect.x + updrect.width > x) {
				paint_columns[i] = true;
				any = true;
			}
			x += width;
		}
	}

	if (!any) return;

	int w = 0;
	int h = 0;
	GetClientSize(&w,&h);
	w -= scrollBar->GetSize().GetWidth();

	wxAutoBufferedPaintDC dc(this);
	dc.SetFont(font);

	dc.SetBackground(row_colors.Default);
	dc.Clear();

	// Draw labels
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.SetBrush(row_colors.LeftCol);
	dc.DrawRectangle(0, lineHeight, columns[0]->Width(), h-lineHeight);

	// Row colors
	wxColour text_standard(to_wx(OPT_GET("Colour/Subtitle Grid/Standard")->GetColor()));
	wxColour text_selection(to_wx(OPT_GET("Colour/Subtitle Grid/Selection")->GetColor()));
	wxColour text_collision(to_wx(OPT_GET("Colour/Subtitle Grid/Collision")->GetColor()));

	// First grid row
	wxPen grid_pen(to_wx(OPT_GET("Colour/Subtitle Grid/Lines")->GetColor()));
	dc.SetPen(grid_pen);
	dc.DrawLine(0, 0, w, 0);
	dc.SetPen(*wxTRANSPARENT_PEN);

	auto paint_text = [&](wxString const& str, int x, int y, int col) {
		int left = x + 4;
		if (columns[col]->Centered()) {
			wxSize ext = dc.GetTextExtent(str);
			left += (columns[col]->Width() - 6 - ext.GetWidth()) / 2;
		}

		dc.DrawText(str, left, y + 2);
	};

	// Paint header
	{
		dc.SetTextForeground(text_standard);
		dc.SetBrush(row_colors.Header);
		dc.DrawRectangle(0, 0, w, lineHeight);

		int x = 0;
		for (size_t i : agi::util::range(columns.size())) {
			if (paint_columns[i])
				paint_text(columns[i]->Header(), x, 0, i);
			x += columns[i]->Width();
		}

		dc.SetPen(grid_pen);
		dc.DrawLine(0, lineHeight, w, lineHeight);
	}

	// Paint the rows
	const int drawPerScreen = h/lineHeight + 1;
	const int nDraw = mid(0, drawPerScreen, GetRows() - yPos);
	const int grid_x = columns[0]->Width();

	const auto active_line = context->selectionController->GetActiveLine();
	auto const& selection = context->selectionController->GetSelectedSet();
	int active_grid_row = GetRowFromDialogue(active_line);
	bool active_line_overlaps = false;
	if (overlap_highlight && active_line && !active_line->Comment && active_line->End > active_line->Start) {
		if (active_grid_row >= 0 && static_cast<size_t>(active_grid_row) < overlap_rows.size())
			active_line_overlaps = overlap_rows[active_grid_row];
	}
	visible_rows.clear();

	for (int i : agi::util::range(nDraw)) {
		wxBrush color = row_colors.Default;
		AssDialogue *curDiag = index_line_map[i + yPos];

		bool is_motion_group = (curDiag->Effect.get().rfind("motion-group:", 0) == 0);
		bool inSel = !!selection.count(curDiag);
		if (inSel && is_motion_group)
			color = s_brush_motion_group_selected;
		else if (is_motion_group)
			color = s_brush_motion_group;
		else if (inSel && curDiag->Comment)
			color = row_colors.SelectedComment;
		else if (inSel)
			color = row_colors.Selection;
		else if (review_mode && playback_row == i + yPos)
			color = row_colors.Playback;
		else if (curDiag->Comment)
			color = row_colors.Comment;

		bool collision_row = overlap_highlight && static_cast<size_t>(i + yPos) < overlap_rows.size() && overlap_rows[i + yPos];
		if (collision_row)
			color = row_colors.Overlap;
		if (active_line_overlaps && !curDiag->Comment && (curDiag == active_line || active_line->CollidesWith(curDiag)))
			color = row_colors.OverlapSelected;

		if (OPT_GET("Subtitle/Grid/Highlight Subtitles in Frame")->GetBool() && IsDisplayed(curDiag)) {
			if (color == row_colors.Default)
				color = row_colors.Visible;
			visible_rows.push_back(i + yPos);
		}
		dc.SetBrush(color);

		// Draw row background color
		if (color != row_colors.Default) {
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.DrawRectangle(grid_x, (i + 1) * lineHeight + 1, w, lineHeight);
		}

		if (active_line != curDiag && curDiag->CollidesWith(active_line))
			dc.SetTextForeground(text_collision);
		else if (inSel)
			dc.SetTextForeground(text_selection);
		else if (is_motion_group)
			dc.SetTextForeground(wxColour(0, 95, 185));
		else
			dc.SetTextForeground(text_standard);

		// Draw text
		int x = 0;
		int y = (i + 1) * lineHeight;
		for (size_t j : agi::util::range(columns.size())) {
			if (paint_columns[j])
				columns[j]->Paint(dc, x, y, curDiag, context);
			x += columns[j]->Width();
		}
		if (review_mode && playback_row == i + yPos) {
			dc.SetBrush(*wxTRANSPARENT_BRUSH);
			dc.SetPen(wxPen(wxColour(230, 135, 20), 2));
			dc.DrawRectangle(grid_x + 1, y + 1, std::max(0, w - grid_x - 2), lineHeight - 1);
		}

		// Draw grid
		dc.SetPen(grid_pen);
		dc.DrawLine(0, y + lineHeight, w , y + lineHeight);
		dc.SetPen(*wxTRANSPARENT_PEN);
	}

	// Draw grid columns
	{
		int maxH = (nDraw + 1) * lineHeight;
		int x = 0;
		dc.SetPen(grid_pen);
		for (auto const& column : columns) {
			x += column->Width();
			if (x < w)
				dc.DrawLine(x, 0, x, maxH);
		}
		dc.DrawLine(0, 0, 0, maxH);
		dc.DrawLine(w, 0, w, maxH);
	}

	if (active_grid_row >= yPos && active_grid_row < yPos + nDraw) {
		dc.SetPen(wxPen(to_wx(OPT_GET("Colour/Subtitle Grid/Active Border")->GetColor())));
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		dc.DrawRectangle(0, (active_grid_row - yPos + 1) * lineHeight, w, lineHeight + 1);
	}
}

void BaseGrid::OnSize(wxSizeEvent &) {
	AdjustScrollbar();
	Refresh(false);
}

void BaseGrid::OnScroll(wxScrollEvent &event) {
	int newPos = event.GetPosition();
	if (yPos != newPos) {
		context->ass->Properties.scroll_position = yPos = newPos;
		Refresh(false);
	}
}

void BaseGrid::OnMouseEvent(wxMouseEvent &event) {
	int h = GetClientSize().GetHeight();
	bool shift = event.ShiftDown();
	bool alt = event.AltDown();
	bool ctrl = event.CmdDown();

	// Row that mouse is over
	bool click = event.LeftDown();
	bool dclick = event.LeftDClick();
	int row = event.GetY() / lineHeight + yPos - 1;
	if (holding && !click)
		row = mid(0, row, GetRows()-1);
	AssDialogue *dlg = GetDialogue(row);
	if (!dlg) row = 0;

	bool is_motion_group = (dlg && dlg->Effect.get().rfind("motion-group:", 0) == 0);

	auto toggle_group_fold = [this](AssDialogue *d) {
		if (!d) return;
		std::string const& eff = d->Effect.get();
		if (eff.rfind("motion-group:", 0) != 0) return;
		std::string gid = eff.substr(13);

		bool was_expanded = (s_expanded_groups.find(gid) != s_expanded_groups.end());
		if (was_expanded) {
			s_expanded_groups.erase(gid);
		} else {
			s_expanded_groups.insert(gid);
		}

		// 翻转开合箭头图标
		std::string text = d->Text.get();
		if (!was_expanded) { // 展开
			if (text.rfind("▶", 0) == 0) {
				text = "▼" + text.substr(3);
			}
		} else { // 折叠
			if (text.rfind("▼", 0) == 0) {
				text = "▶" + text.substr(3);
			}
		}
		if (text != d->Text.get()) {
			d->Text = text;
		}

		UpdateMaps();

		int new_row = GetRowFromDialogue(d);
		if (new_row >= 0) {
			context->selectionController->SetActiveLine(d);
			SelectRow(new_row, false);
			MakeRowVisible(new_row);
		}
		if (context->videoController) {
			context->videoController->JumpToTime(d->Start);
		}
	};

	// Double-click on motion-group row toggles expand/collapse
	if (dclick && is_motion_group && !shift && !ctrl && !alt) {
		toggle_group_fold(dlg);
		return;
	}

	// Single-click on line number '#' column of motion-group row toggles expand/collapse
	if (click && is_motion_group && !shift && !ctrl && !alt && event.GetX() < columns[0]->Width()) {
		toggle_group_fold(dlg);
		return;
	}

	if (event.ButtonDown() && OPT_GET("Subtitle/Grid/Focus Allow")->GetBool())
		SetFocus();

	if (holding) {
		if (!event.LeftIsDown()) {
			if (dlg)
				MakeRowVisible(row);
			holding = false;
			ReleaseMouse();
		}
		else {
			// Only scroll if the mouse has moved to a different row to avoid
			// scrolling on sloppy clicks
			if (row != extendRow) {
				if (row <= yPos)
					ScrollTo(yPos - 3);
				// When dragging down we give a 3 row margin to make it easier
				// to see what's going on, but we don't want to scroll down if
				// the user clicks on the bottom row and drags up
				else if (row > yPos + h / lineHeight - (row > extendRow ? 3 : 1))
					ScrollTo(yPos + 3);
			}
		}
	}
	else if (click && dlg) {
		holding = true;
		CaptureMouse();
	}

	if ((click || holding || dclick) && dlg) {
		int old_extend = extendRow;

		// SetActiveLine will scroll the grid if the row is only half-visible,
		// but we don't want to scroll until the mouse moves or the button is
		// released, to avoid selecting multiple lines on a click
		int old_y_pos = yPos;
		context->selectionController->SetActiveLine(dlg);
		ScrollTo(old_y_pos);
		extendRow = row;

		auto const& selection = context->selectionController->GetSelectedSet();

		// Toggle selected
		if (click && ctrl && !shift && !alt) {
			bool isSel = !!selection.count(dlg);
			if (isSel && selection.size() == 1) return;
			SelectRow(row, true, !isSel);
			return;
		}

		// Normal click
		if ((click || dclick) && !shift && !ctrl && !alt) {
			if (dclick) {
				context->audioBox->ScrollToActiveLine();
				context->videoController->JumpToTime(dlg->Start);
			}
			SelectRow(row, false);
			return;
		}

		// Change active line only
		if (click && !shift && !ctrl && alt)
			return;

		// Block select
		if ((click && shift && !alt) || holding) {
			extendRow = old_extend;
			int i1 = row;
			int i2 = extendRow;

			if (i1 > i2)
				std::swap(i1, i2);

			// Toggle each
			Selection newsel;
			if (ctrl) newsel = selection;
			for (int i = i1; i <= i2; i++)
				newsel.insert(GetDialogue(i));
			context->selectionController->SetSelectedSet(std::move(newsel));
			return;
		}

		return;
	}

	// Mouse wheel
	if (event.GetWheelRotation() != 0) {
		if (ForwardMouseWheelEvent(this, event)) {
			int step = shift ? h / lineHeight - 2 : 3;
			scrollWheelProgress += event.GetWheelRotation();
			ScrollTo(yPos - step * (scrollWheelProgress / event.GetWheelDelta()));
			scrollWheelProgress %= event.GetWheelDelta();
		}
		return;
	}

	event.Skip();
}

void BaseGrid::OnContextMenu(wxContextMenuEvent &evt) {
	wxPoint pos = evt.GetPosition();
	if (pos == wxDefaultPosition || ScreenToClient(pos).y > lineHeight) {
		if (!context_menu) context_menu = menu::GetMenu("grid_context", (wxID_HIGHEST + 1) + 8000, context);
		menu::OpenPopupMenu(context_menu.get(), this);
	}
	else {
		wxMenu menu;
		for (size_t i : agi::util::range(columns.size())) {
			if (columns[i]->CanHide())
				menu.Append(MENU_SHOW_COL + i, columns[i]->Description(), "", wxITEM_CHECK)->Check(columns[i]->Visible());
		}
		PopupMenu(&menu);
	}
}

void BaseGrid::ScrollTo(int y) {
	int nextY = mid(0, y, GetRows() - 1);
	if (yPos != nextY) {
		context->ass->Properties.scroll_position = yPos = nextY;
		scrollBar->SetThumbPosition(yPos);
		Refresh(false);
	}
}

void BaseGrid::AdjustScrollbar() {
	wxSize clientSize = GetClientSize();
	wxSize scrollbarSize = scrollBar->GetSize();

	scrollBar->Freeze();
	scrollBar->SetSize(clientSize.GetWidth() - scrollbarSize.GetWidth(), 0, scrollbarSize.GetWidth(), clientSize.GetHeight());

	if (GetRows() <= 1) {
		yPos = 0;
		scrollBar->Enable(false);
		scrollBar->Thaw();
		return;
	}

	if (!scrollBar->IsEnabled())
		scrollBar->Enable(true);

	int drawPerScreen = clientSize.GetHeight() / lineHeight;
	int rows = GetRows();

	context->ass->Properties.scroll_position = yPos = mid(0, yPos, rows - 1);

	scrollBar->SetScrollbar(yPos, drawPerScreen, rows + drawPerScreen - 1, drawPerScreen - 2, true);
	scrollBar->Thaw();
}

void BaseGrid::SetColumnWidths() {
	int w, h;
	GetClientSize(&w, &h);

	// DC for text extents test
	wxClientDC dc(this);
	dc.SetFont(font);

	text_refresh_rects.clear();
	int x = 0;

	if (!width_helper)
		width_helper = std::make_unique<WidthHelper>();
	width_helper->SetDC(&dc);

	for (auto const& column : columns) {
		column->UpdateWidth(context, *width_helper);
		if (column->Width() && column->RefreshOnTextChange())
			text_refresh_rects.emplace_back(x, 0, column->Width(), h);
		x += column->Width();
	}
	width_helper->Age();
}

AssDialogue *BaseGrid::GetDialogue(int n) const {
	if (static_cast<size_t>(n) >= index_line_map.size()) return nullptr;
	return index_line_map[n];
}

bool BaseGrid::IsDisplayed(const AssDialogue *line) const {
	if (!context->project->VideoProvider()) return false;
	int frame = context->videoController->GetFrameN();
	return context->project->Timecodes().FrameAtTime(line->Start, agi::vfr::START) <= frame
		&& context->project->Timecodes().FrameAtTime(line->End, agi::vfr::END) >= frame;
}

void BaseGrid::OnCharHook(wxKeyEvent &event) {
	if (hotkey::check("Subtitle Grid", context, event))
		return;

	int key = event.GetKeyCode();

	if (key == WXK_UP || key == WXK_DOWN ||
		key == WXK_PAGEUP || key == WXK_PAGEDOWN ||
		key == WXK_HOME || key == WXK_END)
	{
		event.Skip();
		return;
	}

	hotkey::check("Audio", context, event);
}

void BaseGrid::OnKeyDown(wxKeyEvent &event) {
	int w,h;
	GetClientSize(&w, &h);

	int key = event.GetKeyCode();
	bool ctrl = event.CmdDown();
	bool alt = event.AltDown();
	bool shift = event.ShiftDown();

	int dir = 0;
	int step = 1;
	if (key == WXK_UP) dir = -1;
	else if (key == WXK_DOWN) dir = 1;
	else if (key == WXK_PAGEUP) {
		dir = -1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_PAGEDOWN) {
		dir = 1;
		step = h / lineHeight - 2;
	}
	else if (key == WXK_HOME) {
		dir = -1;
		step = GetRows();
	}
	else if (key == WXK_END) {
		dir = 1;
		step = GetRows();
	}

	if (!dir) {
		bool quick_style = OPT_GET("Tool/Playback/Review Quick Style")->GetBool();
		if (quick_style && !ctrl && !alt && context->aiPanel) {
			int num_index = -1;
			if (key >= '1' && key <= '9') num_index = key - '1';
			else if (key == '0') num_index = 9;
			else if (key >= WXK_NUMPAD1 && key <= WXK_NUMPAD9) num_index = key - WXK_NUMPAD1;
			else if (key == WXK_NUMPAD0) num_index = 9;

			if (num_index >= 0) {
				context->aiPanel->ApplyStyle(num_index);
				return;
			}
		}
		event.Skip();
		return;
	}

	auto active_line = context->selectionController->GetActiveLine();
	int old_extend = extendRow;
	int current_grid_row = active_line ? GetRowFromDialogue(active_line) : 0;
	if (current_grid_row < 0) current_grid_row = 0;
	int next = mid(0, current_grid_row + dir * step, GetRows() - 1);
	context->selectionController->SetActiveLine(GetDialogue(next));

	// Move selection
	if (!ctrl && !shift && !alt) {
		SelectRow(next);
		return;
	}

	// Move active only
	if (alt && !shift && !ctrl)
		return;

	// Shift-selection
	if (shift && !ctrl && !alt) {
		extendRow = old_extend;
		// Set range
		int begin = next;
		int end = extendRow;
		if (end < begin)
			std::swap(begin, end);

		// Select range
		Selection newsel;
		for (int i = begin; i <= end; i++)
			newsel.insert(GetDialogue(i));

		context->selectionController->SetSelectedSet(std::move(newsel));

		MakeRowVisible(next);
		return;
	}
}

void BaseGrid::SetByFrame(bool state) {
	if (byFrame == state) return;
	byFrame = state;
	for (auto& column : columns)
		column->SetByFrame(byFrame);
	SetColumnWidths();
	Refresh(false);
}
