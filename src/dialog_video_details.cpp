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
#include "subocr_engine.h"

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

	auto frame_match_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, to_wx("视觉对帧助手 (Frame Match)"));
	auto frame_match_box = frame_match_sizer->GetStaticBox();

	// 1. Engine Choice
	auto engine_row = new wxBoxSizer(wxHORIZONTAL);
	engine_row->Add(new wxStaticText(frame_match_box, -1, to_wx("匹配引擎:")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	wxArrayString engine_choices;
	engine_choices.Add(to_wx("自动模式 (推荐: 内建DBNet -> 原生像素; 望言就绪时优选)"));
	engine_choices.Add(to_wx("望言OCR 外部引擎 (SubtitleOCR C-ABI)"));
	engine_choices.Add(to_wx("内建 DBNet 神经网络 (GPU硬件加速)"));
	engine_choices.Add(to_wx("经典原生双极性像素模式 (Dice掩模匹配)"));
	auto engine_choice = new wxChoice(frame_match_box, -1, wxDefaultPosition, wxDefaultSize, engine_choices);
	int cur_engine = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 3));
	engine_choice->SetSelection(cur_engine);
	engine_row->Add(engine_choice, 1, wxALIGN_CENTRE_VERTICAL);
	frame_match_sizer->Add(engine_row, 0, wxEXPAND | wxALL, 4);

	// 2. SubtitleOCR Path
	auto path_row = new wxBoxSizer(wxHORIZONTAL);
	path_row->Add(new wxStaticText(frame_match_box, -1, to_wx("望言OCR路径:")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto subocr_path_ctrl = new wxTextCtrl(frame_match_box, -1, to_wx(OPT_GET("Tool/Frame Match/SubtitleOCR Path")->GetString()));
	path_row->Add(subocr_path_ctrl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 5);
	auto browse_btn = new wxButton(frame_match_box, -1, _("Browse..."), wxDefaultPosition, wxSize(70, -1));
	browse_btn->Bind(wxEVT_BUTTON, [&d, subocr_path_ctrl](wxCommandEvent&) {
		wxDirDialog dlg(&d, to_wx("选择望言OCR (SubtitleOCR) 所在目录"), subocr_path_ctrl->GetValue());
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

void ShowFrameMatchSettingsDialog(agi::Context *c) {
	wxDialog d(c->parent, -1, to_wx("对帧模式与望言OCR配置 (Frame Match & SubtitleOCR)"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);

	auto frame_match_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, to_wx("对帧匹配引擎设置"));
	auto frame_match_box = frame_match_sizer->GetStaticBox();

	// 1. Engine Choice
	auto engine_row = new wxBoxSizer(wxHORIZONTAL);
	engine_row->Add(new wxStaticText(frame_match_box, -1, to_wx("匹配引擎:")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	wxArrayString engine_choices;
	engine_choices.Add(to_wx("自动模式 (推荐: 内建DBNet -> 原生像素; 望言就绪时优选)"));
	engine_choices.Add(to_wx("望言OCR 外部引擎 (SubtitleOCR C-ABI)"));
	engine_choices.Add(to_wx("内建 DBNet 深度学习 (DirectML GPU加速)"));
	engine_choices.Add(to_wx("经典原生双极性像素模式 (Dice掩模匹配)"));
	auto engine_choice = new wxChoice(frame_match_box, -1, wxDefaultPosition, wxDefaultSize, engine_choices);
	int cur_engine = static_cast<int>(std::clamp<int64_t>(OPT_GET("Tool/Frame Match/Engine")->GetInt(), 0, 3));
	engine_choice->SetSelection(cur_engine);
	engine_row->Add(engine_choice, 1, wxALIGN_CENTRE_VERTICAL);
	frame_match_sizer->Add(engine_row, 0, wxEXPAND | wxALL, 6);

	// Engine tip text
	auto engine_tip = new wxStaticText(frame_match_box, -1, "");
	engine_tip->SetForegroundColour(wxColour(110, 110, 110));
	frame_match_sizer->Add(engine_tip, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

	auto update_engine_tip = [engine_choice, engine_tip]() {
		int sel = engine_choice->GetSelection();
		if (sel == 0) {
			engine_tip->SetLabel(to_wx("自动: 优先使用内建DBNet神经网络检测；无特征时自动回退经典像素；若配置了有效望言OCR则优先调用。"));
		} else if (sel == 1) {
			engine_tip->SetLabel(to_wx("望言OCR: 专门调用外部 subocr.dll C-ABI 管道进行对帧，需指定包含开发接口库的目录。"));
		} else if (sel == 2) {
			engine_tip->SetLabel(to_wx("内建DBNet: 纯进程内基于 DirectML 神经网络模型检测文字笔画与置信度，无需外部依赖。"));
		} else {
			engine_tip->SetLabel(to_wx("经典原生像素: 采用原本的双极性极值二值化与 Dice 相似度匹配算法，原汁原味。"));
		}
	};
	update_engine_tip();
	engine_choice->Bind(wxEVT_CHOICE, [update_engine_tip](wxCommandEvent&) { update_engine_tip(); });

	// 2. SubtitleOCR Path
	auto subocr_box_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, to_wx("望言OCR (SubtitleOCR) 接口集成"));
	auto subocr_box = subocr_box_sizer->GetStaticBox();

	auto path_row = new wxBoxSizer(wxHORIZONTAL);
	path_row->Add(new wxStaticText(subocr_box, -1, to_wx("目录路径:")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto subocr_path_ctrl = new wxTextCtrl(subocr_box, -1, to_wx(OPT_GET("Tool/Frame Match/SubtitleOCR Path")->GetString()));
	path_row->Add(subocr_path_ctrl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 5);
	auto browse_btn = new wxButton(subocr_box, -1, _("Browse..."), wxDefaultPosition, wxSize(75, -1));
	path_row->Add(browse_btn, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 5);
	auto test_btn = new wxButton(subocr_box, -1, to_wx("检测 DLL"), wxDefaultPosition, wxSize(75, -1));
	path_row->Add(test_btn, 0, wxALIGN_CENTRE_VERTICAL);
	subocr_box_sizer->Add(path_row, 0, wxEXPAND | wxALL, 6);

	auto status_text = new wxStaticText(subocr_box, -1, "");
	subocr_box_sizer->Add(status_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

	auto update_status = [subocr_path_ctrl, status_text](bool probe = false) {
		std::string path_u8 = from_wx(subocr_path_ctrl->GetValue());
		if (path_u8.empty()) {
			if (probe) {
				SubocrEngine::Get().Unload();
				SubocrEngine::Get().Load();
			}
			if (SubocrEngine::Get().IsAvailable()) {
				status_text->SetLabel(wxString::Format(to_wx("状态: 默认集成引擎就绪 (%s)"), wxString(SubocrEngine::Get().GetLoadedPath())));
				status_text->SetForegroundColour(wxColour(0, 130, 0));
			} else {
				status_text->SetLabel(to_wx("状态: 未指定自定义路径 (使用内建神经网络 / 原生像素引擎)"));
				status_text->SetForegroundColour(wxColour(110, 110, 110));
			}
			return;
		}
		if (probe) {
			SubocrEngine::Get().Unload();
			SubocrEngine::Get().Load(path_u8);
		}
		if (SubocrEngine::Get().IsAvailable()) {
			status_text->SetLabel(wxString::Format(to_wx("状态: %s (%s)"),
				to_wx(SubocrEngine::Get().GetStatusMessage()),
				wxString(SubocrEngine::Get().GetLoadedPath())));
			status_text->SetForegroundColour(wxColour(0, 130, 0));
		} else {
			std::string msg = SubocrEngine::Get().GetStatusMessage();
			if (msg.empty()) msg = "该目录下未找到有效的 subocr.dll 或缺少模型资源";
			status_text->SetLabel(wxString::Format(to_wx("状态: %s"), to_wx(msg)));
			status_text->SetForegroundColour(wxColour(180, 50, 0));
		}
	};
	update_status(false);

	browse_btn->Bind(wxEVT_BUTTON, [&d, subocr_path_ctrl, update_status](wxCommandEvent&) {
		wxDirDialog dlg(&d, to_wx("选择望言OCR (SubtitleOCR) 所在目录"), subocr_path_ctrl->GetValue());
		if (dlg.ShowModal() == wxID_OK) {
			subocr_path_ctrl->SetValue(dlg.GetPath());
			update_status(true);
		}
	});

	test_btn->Bind(wxEVT_BUTTON, [update_status](wxCommandEvent&) {
		update_status(true);
	});

	// 3. Similarity & Rerun step
	auto sim_box_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, to_wx("匹配参数与阈值"));
	auto sim_box = sim_box_sizer->GetStaticBox();
	auto sim_row = new wxBoxSizer(wxHORIZONTAL);
	sim_row->Add(new wxStaticText(sim_box, -1, _("Minimum region similarity (%):")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto minimum_similarity = new wxSpinCtrl(sim_box, -1, wxEmptyString, wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 50, 95, OPT_GET("Tool/Frame Match/Minimum Similarity")->GetInt());
	sim_row->Add(minimum_similarity, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 15);
	sim_row->Add(new wxStaticText(sim_box, -1, _("Rerun similarity step (%):")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
	auto rerun_step = new wxSpinCtrl(sim_box, -1, wxEmptyString, wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 5, 30, OPT_GET("Tool/Frame Match/Rerun Step")->GetInt());
	sim_row->Add(rerun_step, 0, wxALIGN_CENTRE_VERTICAL);
	sim_box_sizer->Add(sim_row, 0, wxEXPAND | wxALL, 6);

	main_sizer->Add(frame_match_sizer, 0, wxALL | wxEXPAND, 8);
	main_sizer->Add(subocr_box_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);
	main_sizer->Add(sim_box_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);
	main_sizer->Add(d.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, 8);

	d.SetSizerAndFit(main_sizer);
	d.CenterOnParent();

	if (d.ShowModal() == wxID_OK) {
		OPT_SET("Tool/Frame Match/Engine")->SetInt(engine_choice->GetSelection());
		std::string new_path = from_wx(subocr_path_ctrl->GetValue());
		OPT_SET("Tool/Frame Match/SubtitleOCR Path")->SetString(new_path);
		OPT_SET("Tool/Frame Match/Minimum Similarity")->SetInt(minimum_similarity->GetValue());
		OPT_SET("Tool/Frame Match/Rerun Step")->SetInt(rerun_step->GetValue());
		if (!new_path.empty()) {
			SubocrEngine::Get().Load(new_path);
		}
	}
}
