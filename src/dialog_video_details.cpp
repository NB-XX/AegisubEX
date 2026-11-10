// Copyright (c) 2007, Alysson Souza e Silva (demi_alucard)
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

#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"

#include <libaegisub/ass/time.h>

#include <boost/rational.hpp>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

void ShowVideoDetailsDialog(agi::Context *c) {
	wxDialog d(c->parent, -1, _("Video Details"));

	auto provider = c->project->VideoProvider();
	auto width = provider->GetWidth();
	auto height = provider->GetHeight();
	auto framecount = provider->GetFrameCount();
	auto fps = provider->GetFPS();
	boost::rational<int> ar(width, height);

	auto video_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, _("Video"));
	wxWindow *video_sizer_box = video_sizer->GetStaticBox();

	auto fg = new wxFlexGridSizer(2, 5, 10);
	auto make_field = [&, video_sizer_box](wxString const& name, wxString const& value) {
		fg->Add(new wxStaticText(video_sizer_box, -1, name), 0, wxALIGN_CENTRE_VERTICAL);
		fg->Add(new wxTextCtrl(video_sizer_box, -1, value, wxDefaultPosition, wxSize(300,-1), wxTE_READONLY), 0, wxALIGN_CENTRE_VERTICAL | wxEXPAND);
	};
	make_field(_("File name:"), c->project->VideoName().wstring());
	make_field(_("FPS:"), fmt_wx("%.3f", fps.FPS()));
	make_field(_("Resolution:"), fmt_wx("%dx%d (%d:%d)", width, height, ar.numerator(), ar.denominator()));
	make_field(_("Length:"), fmt_plural(framecount, "%d frame (%s)", "%d frames (%s)",
		framecount, agi::Time(fps.TimeAtFrame(framecount - 1)).GetAssFormatted(true)));
	make_field(_("Actual color matrix:"), agi::ycbcr::matrix_to_string(provider->GetRealColorSpace().matrix));
	make_field(_("Forced color matrix:"), agi::ycbcr::matrix_to_string(provider->GetColorSpace().matrix));
	make_field(_("Actual color range:"), agi::ycbcr::range_to_string(provider->GetRealColorSpace().range));
	make_field(_("Forced color range:"), agi::ycbcr::range_to_string(provider->GetColorSpace().range));
	make_field(_("Decoder:"), to_wx(provider->GetDecoderName()));

	video_sizer->Add(fg);

	auto frame_match_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, _("Frame Match Assistant"));
	auto frame_match_box = frame_match_sizer->GetStaticBox();

	// 1. Engine Choice
	auto engine_row = new wxBoxSizer(wxHORIZONTAL);
	engine_row->Add(new wxStaticText(frame_match_box, -1, _("Match Engine:")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	wxArrayString engine_choices;
	engine_choices.Add(_("Auto (SubtitleOCR -> DBNet -> Classic)"));
	engine_choices.Add(_("SubtitleOCR External Engine (望言OCR)"));
	engine_choices.Add(_("Native DBNet Neural Matcher (内置深度学习)"));
	engine_choices.Add(_("Classic Native Pixel Matcher (经典原生像素/Dice)"));
	auto engine_choice = new wxChoice(frame_match_box, -1, wxDefaultPosition, wxDefaultSize, engine_choices);
	int cur_engine = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 3));
	engine_choice->SetSelection(cur_engine);
	engine_row->Add(engine_choice, 1, wxALIGN_CENTRE_VERTICAL);
	frame_match_sizer->Add(engine_row, 0, wxEXPAND | wxALL, 4);

	// 2. SubtitleOCR Path
	auto path_row = new wxBoxSizer(wxHORIZONTAL);
	path_row->Add(new wxStaticText(frame_match_box, -1, _("SubtitleOCR Path:")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto subocr_path_ctrl = new wxTextCtrl(frame_match_box, -1, to_wx(OPT_GET("Tool/Frame Match/SubtitleOCR Path")->GetString()));
	path_row->Add(subocr_path_ctrl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 5);
	auto browse_btn = new wxButton(frame_match_box, -1, _("Browse..."), wxDefaultPosition, wxSize(70, -1));
	browse_btn->Bind(wxEVT_BUTTON, [&d, subocr_path_ctrl](wxCommandEvent&) {
		wxDirDialog dlg(&d, _("Select SubtitleOCR (望言OCR) Directory"), subocr_path_ctrl->GetValue());
		if (dlg.ShowModal() == wxID_OK) {
			subocr_path_ctrl->SetValue(dlg.GetPath());
		}
	});
	path_row->Add(browse_btn, 0, wxALIGN_CENTRE_VERTICAL);
	frame_match_sizer->Add(path_row, 0, wxEXPAND | wxALL, 4);

	// 3. Similarity & Rerun step
	auto sim_row = new wxBoxSizer(wxHORIZONTAL);
	sim_row->Add(new wxStaticText(frame_match_box, -1, _("Minimum region similarity (%):")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto minimum_similarity = new wxSpinCtrl(frame_match_box, -1, wxEmptyString, wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 50, 95, OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt());
	sim_row->Add(minimum_similarity, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 15);
	sim_row->Add(new wxStaticText(frame_match_box, -1, _("Rerun similarity step (%):")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto rerun_step = new wxSpinCtrl(frame_match_box, -1, wxEmptyString, wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 5, 30, OPT_GET("Tool/Frame Match/Rerun Step")->GetInt());
	sim_row->Add(rerun_step, 0, wxALIGN_CENTRE_VERTICAL);
	frame_match_sizer->Add(sim_row, 0, wxEXPAND | wxALL, 4);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(video_sizer, 1, wxALL|wxEXPAND, 5);
	main_sizer->Add(frame_match_sizer, 0, wxLEFT|wxRIGHT|wxBOTTOM|wxEXPAND, 5);
	main_sizer->Add(d.CreateSeparatedButtonSizer(wxOK), 0, wxALL|wxEXPAND, 5);
	d.SetSizerAndFit(main_sizer);

	d.CenterOnParent();
	if (d.ShowModal() == wxID_OK)
	{
		OPT_SET("Tool/Frame Match/Engine")->SetInt(engine_choice->GetSelection());
		OPT_SET("Tool/Frame Match/SubtitleOCR Path")->SetString(from_wx(subocr_path_ctrl->GetValue()));
		OPT_SET("Tool/Frame Match/Minimum Similarity")->SetInt(minimum_similarity->GetValue());
		OPT_SET("Tool/Frame Match/Rerun Step")->SetInt(rerun_step->GetValue());
	}
}
