// Copyright (c) 2007, Rodrigo Braz Monteiro
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

/// @file subs_preview.cpp
/// @brief Preview control using a dummy video provider and subtitles provider to render a preview
/// @ingroup custom_control
///

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "ass_style_fx.h"
#include "dialog_progress.h"
#include "subs_preview.h"
#include "include/aegisub/subtitles_provider.h"
#include "video_frame.h"
#include "video_provider_dummy.h"


#include <wx/dcclient.h>
#include <wx/msgdlg.h>

#include <algorithm>

SubtitlesPreview::SubtitlesPreview(wxWindow *parent, wxSize size, int winStyle, agi::Color col)
: wxWindow(parent, -1, wxDefaultPosition, size, winStyle)
, style(std::make_unique<AssStyle>())
, back_color(col)
, line(std::make_unique<AssDialogue>())
{
	line->Text = "{\\q2}preview";
	// The default-constructed dialogue has no style name. Keep it explicitly
	// bound to the preview style so libass never has to resolve an empty style.
	line->Style = "Default";

	SetStyle(*style);

	SetSizeHints(size.GetWidth(), size.GetHeight(), -1, -1);
	wxSizeEvent evt(size);
	OnSize(evt);
	UpdateBitmap();

	Bind(wxEVT_PAINT, &SubtitlesPreview::OnPaint, this);
	Bind(wxEVT_SIZE, &SubtitlesPreview::OnSize, this);
}

SubtitlesPreview::~SubtitlesPreview() {
}

void SubtitlesPreview::SetStyle(AssStyle const& new_style) {
	if (provider && preview_font != new_style.font)
		provider->Reinitialize();

	// Replace the preview copy instead of assigning into it. wx preview refreshes
	// can outlive temporary style edits, and replacing avoids retaining stale
	// AssEntry string storage across repeated control updates.
	auto updated_style = std::make_unique<AssStyle>(new_style);
	updated_style->name = "Default";
	preview_font = updated_style->font;
	style = std::move(updated_style);
	line->Style = "Default";
	style->alignment = 5;
	std::fill(style->Margin.begin(), style->Margin.end(), 0);
	style->UpdateData();
	UpdateBitmap();
}

void SubtitlesPreview::SetText(std::string const& text) {
	std::string new_text = "{\\q2}" + text;
	if (new_text != line->Text) {
		line->Text = new_text;
		UpdateBitmap();
	}
}

void SubtitlesPreview::SetColour(agi::Color col) {
	if (col != back_color) {
		back_color = col;
		vid = std::make_unique<DummyVideoProvider>(0.0, 10, bmp->GetWidth(), bmp->GetHeight(), back_color, true);
		UpdateBitmap();
	}
}

void SubtitlesPreview::UpdateBitmap() {
	if (!vid) return;

	// Build a fresh snapshot for each render instead of tearing down and
	// repopulating intrusive list nodes that were previously handed to the
	// subtitle provider for serialization.
	auto preview_file = std::make_unique<AssFile>();
	preview_file->LoadDefault();
	// Keep a conventional subtitle coordinate scale and match the preview
	// aspect ratio. Using the bitmap's pixel height as PlayResY made large
	// subtitle fonts render at full pixel size and clip the effect preview.
	constexpr int preview_res_y = 288;
	int preview_res_x = std::max(1, bmp->GetWidth()) * preview_res_y / std::max(1, bmp->GetHeight());
	preview_file->SetScriptInfo("PlayResX", std::to_string(preview_res_x));
	preview_file->SetScriptInfo("PlayResY", std::to_string(preview_res_y));
	auto preview_style = new AssStyle(*style);
	preview_style->name = "Default";
	preview_style->UpdateData();
	preview_file->Styles.push_back(*preview_style);
	preview_file->Events.push_back(*new AssDialogue(*line));
	ExpandStyleEffects(*preview_file, false);

	VideoFrame frame;
	vid->GetFrame(0, frame);

	if (provider) {
		try {
			provider->LoadSubtitles(preview_file.get());
			provider->DrawSubtitles(frame, 0.1);
		}
		catch (...) { }
	}

	// Convert frame to bitmap
	*bmp = static_cast<wxBitmap>(GetImage(frame));
	Refresh();
}

void SubtitlesPreview::OnPaint(wxPaintEvent &) {
	wxPaintDC(this).DrawBitmap(*bmp, 0, 0);
}

void SubtitlesPreview::OnSize(wxSizeEvent &evt) {
	if (bmp.get() && evt.GetSize() == bmp->GetSize()) return;

	int w = evt.GetSize().GetWidth();
	int h = evt.GetSize().GetHeight();

	bmp = std::make_unique<wxBitmap>(w, h, -1);
	vid = std::make_unique<DummyVideoProvider>(0.0, 10, w, h, back_color, true);
	try {
		if (!progress)
			progress = std::make_unique<DialogProgress>(this);
		if (!provider)
			provider = SubtitlesProviderFactory::GetProvider(progress.get());
	}
	catch (...) {
		wxMessageBox(
			_("Could not get any subtitles provider for the preview box. Make sure that you have a provider installed."),
			_("No subtitles provider"), wxOK | wxICON_ERROR | wxCENTER);
	}

	UpdateBitmap();
}
