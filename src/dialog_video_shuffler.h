// Copyright (c) 2026, Aegisub Project
// All rights reserved.
//
// Native Video Shuffler & Cutter Dialog for Aegisub.
// Provides GUI for automated video splitting, filtering, shuffling, and merging
// based on ASS subtitle timing, comments, and chapter annotations.

#pragma once

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_shuffler.h"
#include "format.h"
#include <libaegisub/path.h>
#include <libaegisub/fs.h>

#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

class DialogShufflerLog : public wxDialog {
public:
	DialogShufflerLog(wxWindow* parent, const std::string& log_text)
	: wxDialog(parent, wxID_ANY, to_wx("FFmpeg 执行详细诊断日志"), wxDefaultPosition, wxSize(780, 560),
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	{
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		auto* text_ctrl = new wxTextCtrl(this, wxID_ANY, to_wx(log_text), wxDefaultPosition, wxDefaultSize,
		                                 wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
		wxFont font(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
		text_ctrl->SetFont(font);
		sizer->Add(text_ctrl, 1, wxEXPAND | wxALL, 8);

		auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
		auto* copy_btn = new wxButton(this, wxID_ANY, to_wx("📋 复制全部日志"));
		auto* close_btn = new wxButton(this, wxID_CANCEL, to_wx("关闭"));

		copy_btn->Bind(wxEVT_BUTTON, [log_text](wxCommandEvent&) {
			if (wxTheClipboard->Open()) {
				wxTheClipboard->SetData(new wxTextDataObject(to_wx(log_text)));
				wxTheClipboard->Close();
				wxMessageBox(to_wx("诊断日志已成功复制到系统剪贴板！"), to_wx("提示"), wxOK | wxICON_INFORMATION);
			}
		});

		btn_sizer->Add(copy_btn, 0, wxRIGHT, 8);
		btn_sizer->AddStretchSpacer(1);
		btn_sizer->Add(close_btn, 0);
		sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		SetSizer(sizer);
		Centre();
	}
};

class DialogVideoShuffler : public wxDialog {
private:
	inline static std::wstring s_custom_ffmpeg_path;

	agi::Context* c_ = nullptr;
	std::thread worker_thread_;
	std::shared_ptr<std::atomic<bool>> alive_;
	std::shared_ptr<std::atomic<bool>> cancel_;
	bool running_ = false;

	VideoShuffler::ShufflerResult last_result_;

	// UI 控件
	wxStaticText* video_info_lbl_ = nullptr;
	wxStaticText* subs_info_lbl_ = nullptr;

	wxRadioButton* rb_mode_highlight_ = nullptr;
	wxRadioButton* rb_mode_chapters_ = nullptr;
	wxRadioButton* rb_mode_selected_ = nullptr;

	wxSpinCtrl* spin_gap_threshold_ = nullptr;
	wxSpinCtrl* spin_leadin_ = nullptr;
	wxSpinCtrl* spin_leadout_ = nullptr;

	wxRadioButton* rb_engine_fast_ = nullptr;
	wxRadioButton* rb_engine_reencode_ = nullptr;

	wxStaticText* ffmpeg_status_lbl_ = nullptr;
	wxButton* btn_browse_ffmpeg_ = nullptr;

	wxRadioButton* rb_fmt_mp4_ = nullptr;
	wxRadioButton* rb_fmt_mkv_ = nullptr;
	wxRadioButton* rb_fmt_mp3_ = nullptr;
	wxRadioButton* rb_fmt_wav_ = nullptr;

	wxTextCtrl* out_dir_ctrl_ = nullptr;
	wxButton* browse_dir_btn_ = nullptr;
	wxTextCtrl* prefix_ctrl_ = nullptr;
	wxCheckBox* chk_sync_ass_ = nullptr;

	wxListBox* chapter_list_ = nullptr;

	wxGauge* progress_gauge_ = nullptr;
	wxStaticText* status_lbl_ = nullptr;

	wxButton* btn_start_ = nullptr;
	wxButton* btn_cancel_ = nullptr;
	wxButton* btn_view_log_ = nullptr;
	wxButton* btn_open_dir_ = nullptr;
	wxButton* btn_open_in_aegisub_ = nullptr;
	wxButton* btn_close_ = nullptr;

	std::set<AssDialogue*> selected_lines_;
	int video_duration_ms_ = 0;

	void ShowLogDialog() {
		if (last_result_.detailed_log.empty()) {
			wxMessageBox(to_wx("当前任务尚未生成详细诊断日志。"), to_wx("提示"), wxOK | wxICON_INFORMATION, this);
			return;
		}
		DialogShufflerLog dlg(this, last_result_.detailed_log);
		dlg.ShowModal();
	}

	void InitProjectInfo() {
		if (!c_ || !c_->project) return;

		agi::fs::path vid_path = c_->path->MakeAbsolute(c_->project->VideoName(), "?script");
		if (vid_path.empty() && c_->ass) {
			vid_path = c_->path->MakeAbsolute(c_->ass->Properties.video_file, "?script");
		}

		std::error_code f_ec;
		bool vid_exists = !vid_path.empty() && std::filesystem::exists(vid_path, f_ec);

		if (vid_path.empty()) {
			video_info_lbl_->SetLabel(to_wx("⚠️ 当前工程未载入视频！请先在 Aegisub 中打开视频 (视轨 -> 打开视频...)"));
			video_info_lbl_->SetForegroundColour(wxColour(200, 50, 50));
			btn_start_->Disable();
		} else if (!vid_exists) {
			video_info_lbl_->SetLabel(fmt_wx("⚠️ 视频源文件不存在: %s", vid_path.wstring()));
			video_info_lbl_->SetForegroundColour(wxColour(200, 50, 50));
			btn_start_->Disable();
		} else {
			auto provider = c_->project->VideoProvider();
			if (provider) {
				video_duration_ms_ = static_cast<int>(provider->GetFrameCount() * 1000.0 / provider->GetFPS().FPS());
				video_info_lbl_->SetLabel(fmt_wx("当前视频: %s  (分辨率: %dx%d, 帧率: %.2ffps, 时长: %s)",
					vid_path.filename().wstring(), provider->GetWidth(), provider->GetHeight(),
					provider->GetFPS().FPS(), to_wx(VideoShuffler::FormatMsDuration(video_duration_ms_))));
			} else {
				video_info_lbl_->SetLabel(fmt_wx("当前视频: %s", vid_path.filename().wstring()));
			}
			video_info_lbl_->SetForegroundColour(wxColour(30, 130, 30));

			// 预设默认输出目录为视频所在目录
			if (out_dir_ctrl_->GetValue().empty()) {
				out_dir_ctrl_->SetValue(vid_path.parent_path().wstring());
			}
			if (prefix_ctrl_->GetValue().empty()) {
				prefix_ctrl_->SetValue(vid_path.stem().wstring());
			}
		}

		// 检测 FFmpeg 引擎状态
		std::wstring detected_ffmpeg = VideoShuffler::FindFFmpegExecutable(s_custom_ffmpeg_path);
		if (!detected_ffmpeg.empty()) {
			ffmpeg_status_lbl_->SetLabel(fmt_wx("🟢 FFmpeg 就绪: %s", detected_ffmpeg));
			ffmpeg_status_lbl_->SetForegroundColour(wxColour(30, 130, 30));
		} else {
			ffmpeg_status_lbl_->SetLabel(to_wx("🔴 未检测到 FFmpeg 引擎 (请点击右侧按钮选择 ffmpeg.exe 或将其加入 PATH)"));
			ffmpeg_status_lbl_->SetForegroundColour(wxColour(200, 30, 30));
		}

		// 统计字幕信息
		int total_lines = 0;
		int dialogue_lines = 0;
		int comment_lines = 0;
		int chapter_count = 0;

		std::vector<std::string> found_chapters;

		if (c_->ass) {
			for (auto& entry : c_->ass->Events) {
				total_lines++;
				if (entry.Comment) {
					comment_lines++;
					std::string text = entry.Text.get();
					size_t first = text.find_first_not_of(" \t\r\n");
					std::string tr = (first == std::string::npos) ? "" : text.substr(first);
					if (tr.starts_with("#")) {
						chapter_count++;
						found_chapters.push_back(tr);
					}
				} else {
					dialogue_lines++;
				}
			}
		}

		// 获取当前选中行
		if (c_->selectionController) {
			selected_lines_ = c_->selectionController->GetSelectedSet();
		}

		subs_info_lbl_->SetLabel(fmt_wx("字幕统计: 共 %d 行 (有效对话 %d 行, 注释/跳过 %d 行, 识别章节 %d 个, 选中行 %d 行)",
			total_lines, dialogue_lines, comment_lines, chapter_count, static_cast<int>(selected_lines_.size())));

		// 填充章节列表
		chapter_list_->Clear();
		if (!found_chapters.empty()) {
			for (const auto& ch : found_chapters) {
				chapter_list_->Append(to_wx(ch));
			}
		} else {
			chapter_list_->Append(to_wx("（未检测到以 # 命名的章节注释行。插入形如 Comment: # 序章 即可切片）"));
		}

		if (selected_lines_.empty()) {
			rb_mode_selected_->Disable();
		} else {
			rb_mode_selected_->Enable();
		}
	}

public:
	DialogVideoShuffler(wxWindow* parent, agi::Context* c)
	: wxDialog(parent, wxID_ANY, to_wx("按字幕切分与合并视频"), wxDefaultPosition, wxSize(780, 750),
	           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, c_(c)
	, alive_(std::make_shared<std::atomic<bool>>(true))
	, cancel_(std::make_shared<std::atomic<bool>>(false))
	{
		auto* main_sizer = new wxBoxSizer(wxVERTICAL);

		// 1. 顶部视频与字幕状态信息
		auto* info_box = new wxStaticBox(this, wxID_ANY, to_wx("当前工程媒体与字幕源"));
		auto* info_sizer = new wxStaticBoxSizer(info_box, wxVERTICAL);

		video_info_lbl_ = new wxStaticText(info_box, wxID_ANY, to_wx("正在读取视频信息..."));
		subs_info_lbl_ = new wxStaticText(info_box, wxID_ANY, to_wx("正在统计字幕行..."));
		subs_info_lbl_->SetForegroundColour(wxColour(80, 80, 80));

		info_sizer->Add(video_info_lbl_, 0, wxALL, 4);
		info_sizer->Add(subs_info_lbl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
		main_sizer->Add(info_sizer, 0, wxEXPAND | wxALL, 8);

		// 2. 切分/合并模式选择
		auto* mode_box = new wxStaticBox(this, wxID_ANY, to_wx("剪辑与重组模式 (Operation Mode)"));
		auto* mode_sizer = new wxStaticBoxSizer(mode_box, wxVERTICAL);

		rb_mode_highlight_ = new wxRadioButton(mode_box, wxID_ANY,
			to_wx("🚀 智能精简剪辑 (自动剔除无声空白与废话注释，合并为一个精华视频)"),
			wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		rb_mode_highlight_->SetValue(true);

		rb_mode_chapters_ = new wxRadioButton(mode_box, wxID_ANY,
			to_wx("📑 按章节注释批量切片 (# 章节名 / ## 小节名，导出各个独立分段文件)"));

		rb_mode_selected_ = new wxRadioButton(mode_box, wxID_ANY,
			to_wx("🎯 仅切分当前选中的字幕行 (快速切取高光片段)"));

		mode_sizer->Add(rb_mode_highlight_, 0, wxALL, 3);
		mode_sizer->Add(rb_mode_chapters_, 0, wxALL, 3);
		mode_sizer->Add(rb_mode_selected_, 0, wxALL, 3);
		main_sizer->Add(mode_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 3. 剪辑参数与缓冲配置
		auto* param_box = new wxStaticBox(this, wxID_ANY, to_wx("剪辑缓冲与间隙参数 (Padding & Gap Threshold)"));
		auto* param_sizer = new wxStaticBoxSizer(param_box, wxHORIZONTAL);

		param_sizer->Add(new wxStaticText(param_box, wxID_ANY, to_wx("间隙合并阈值:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		spin_gap_threshold_ = new wxSpinCtrl(param_box, wxID_ANY, "1000", wxDefaultPosition, wxSize(75, -1), wxSP_ARROW_KEYS, 0, 10000, 1000);
		param_sizer->Add(spin_gap_threshold_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		param_sizer->Add(new wxStaticText(param_box, wxID_ANY, to_wx("ms")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);

		param_sizer->Add(new wxStaticText(param_box, wxID_ANY, to_wx("起始提前缓冲:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		spin_leadin_ = new wxSpinCtrl(param_box, wxID_ANY, "100", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 0, 2000, 100);
		param_sizer->Add(spin_leadin_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		param_sizer->Add(new wxStaticText(param_box, wxID_ANY, to_wx("ms")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);

		param_sizer->Add(new wxStaticText(param_box, wxID_ANY, to_wx("结尾延后缓冲:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		spin_leadout_ = new wxSpinCtrl(param_box, wxID_ANY, "150", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 0, 2000, 150);
		param_sizer->Add(spin_leadout_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		param_sizer->Add(new wxStaticText(param_box, wxID_ANY, to_wx("ms")), 0, wxALIGN_CENTER_VERTICAL);

		main_sizer->Add(param_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 4. 编码与格式设置
		auto* enc_box = new wxStaticBox(this, wxID_ANY, to_wx("编码引擎与输出格式 (Encoding Engine & Format)"));
		auto* enc_sizer = new wxStaticBoxSizer(enc_box, wxVERTICAL);

		// FFmpeg 执行引擎行
		auto* ffmpeg_row = new wxBoxSizer(wxHORIZONTAL);
		ffmpeg_status_lbl_ = new wxStaticText(enc_box, wxID_ANY, to_wx("FFmpeg 状态: 检测中..."));
		btn_browse_ffmpeg_ = new wxButton(enc_box, wxID_ANY, to_wx("选择 FFmpeg..."));
		ffmpeg_row->Add(ffmpeg_status_lbl_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		ffmpeg_row->Add(btn_browse_ffmpeg_, 0, wxALIGN_CENTER_VERTICAL);
		enc_sizer->Add(ffmpeg_row, 0, wxEXPAND | wxALL, 4);

		auto* enc_row1 = new wxBoxSizer(wxHORIZONTAL);
		enc_row1->Add(new wxStaticText(enc_box, wxID_ANY, to_wx("编码模式:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		rb_engine_fast_ = new wxRadioButton(enc_box, wxID_ANY, to_wx("⚡ 超快速无损流复制 (-c copy，秒级导出，原画无损)"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		rb_engine_fast_->SetValue(true);
		rb_engine_reencode_ = new wxRadioButton(enc_box, wxID_ANY, to_wx("🎯 逐帧精准重编码 (x264+aac，毫秒级帧准度)"));
		enc_row1->Add(rb_engine_fast_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
		enc_row1->Add(rb_engine_reencode_, 0, wxALIGN_CENTER_VERTICAL);
		enc_sizer->Add(enc_row1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		auto* enc_row2 = new wxBoxSizer(wxHORIZONTAL);
		enc_row2->Add(new wxStaticText(enc_box, wxID_ANY, to_wx("输出格式:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		rb_fmt_mp4_ = new wxRadioButton(enc_box, wxID_ANY, "MP4 (.mp4)", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		rb_fmt_mp4_->SetValue(true);
		rb_fmt_mkv_ = new wxRadioButton(enc_box, wxID_ANY, "MKV (.mkv)");
		rb_fmt_mp3_ = new wxRadioButton(enc_box, wxID_ANY, to_wx("仅音频 (.mp3)"));
		rb_fmt_wav_ = new wxRadioButton(enc_box, wxID_ANY, to_wx("仅音频 (.wav)"));

		enc_row2->Add(rb_fmt_mp4_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		enc_row2->Add(rb_fmt_mkv_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		enc_row2->Add(rb_fmt_mp3_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		enc_row2->Add(rb_fmt_wav_, 0, wxALIGN_CENTER_VERTICAL);
		enc_sizer->Add(enc_row2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		main_sizer->Add(enc_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 5. 输出目录与文件名前缀
		auto* out_box = new wxStaticBox(this, wxID_ANY, to_wx("输出目录与选项 (Output Path & Options)"));
		auto* out_sizer = new wxStaticBoxSizer(out_box, wxVERTICAL);

		auto* path_row = new wxBoxSizer(wxHORIZONTAL);
		path_row->Add(new wxStaticText(out_box, wxID_ANY, to_wx("输出路径:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		out_dir_ctrl_ = new wxTextCtrl(out_box, wxID_ANY, "");
		browse_dir_btn_ = new wxButton(out_box, wxID_ANY, to_wx("浏览..."));
		path_row->Add(out_dir_ctrl_, 1, wxEXPAND | wxRIGHT, 6);
		path_row->Add(browse_dir_btn_, 0);
		out_sizer->Add(path_row, 0, wxEXPAND | wxALL, 4);

		auto* opt_row = new wxBoxSizer(wxHORIZONTAL);
		opt_row->Add(new wxStaticText(out_box, wxID_ANY, to_wx("前缀名称:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		prefix_ctrl_ = new wxTextCtrl(out_box, wxID_ANY, "", wxDefaultPosition, wxSize(180, -1));
		opt_row->Add(prefix_ctrl_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);

		chk_sync_ass_ = new wxCheckBox(out_box, wxID_ANY, to_wx("同步生成与新视频 100% 严丝合缝的新 ASS 字幕 (_clip.ass)"));
		chk_sync_ass_->SetValue(true);
		opt_row->Add(chk_sync_ass_, 0, wxALIGN_CENTER_VERTICAL);
		out_sizer->Add(opt_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

		main_sizer->Add(out_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 6. 章节预览列表
		auto* ch_box = new wxStaticBox(this, wxID_ANY, to_wx("检测到的章节列表 (Chapter Annotations Preview)"));
		auto* ch_sizer = new wxStaticBoxSizer(ch_box, wxVERTICAL);
		chapter_list_ = new wxListBox(ch_box, wxID_ANY, wxDefaultPosition, wxSize(-1, 65));
		ch_sizer->Add(chapter_list_, 1, wxEXPAND | wxALL, 4);
		main_sizer->Add(ch_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 7. 进度条与状态
		progress_gauge_ = new wxGauge(this, wxID_ANY, 100);
		main_sizer->Add(progress_gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		status_lbl_ = new wxStaticText(this, wxID_ANY, to_wx("就绪。请确认配置后点击下方【开始按字幕切分/合并】。"));
		status_lbl_->SetForegroundColour(wxColour(100, 100, 100));
		main_sizer->Add(status_lbl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 8. 操作按钮栏
		auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
		btn_start_ = new wxButton(this, wxID_ANY, to_wx("⚡ 开始按字幕切分/合并"));
		btn_start_->SetDefault();
		btn_cancel_ = new wxButton(this, wxID_ANY, to_wx("取消"));
		btn_cancel_->Disable();

		btn_view_log_ = new wxButton(this, wxID_ANY, to_wx("📋 查看执行日志"));
		btn_view_log_->Disable();

		btn_open_dir_ = new wxButton(this, wxID_ANY, to_wx("📂 打开输出文件夹"));
		btn_open_dir_->Disable();

		btn_open_in_aegisub_ = new wxButton(this, wxID_ANY, to_wx("🎬 在 Aegisub 中载入切片工程"));
		btn_open_in_aegisub_->Disable();

		btn_close_ = new wxButton(this, wxID_ANY, to_wx("关闭"));

		btn_sizer->Add(btn_start_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_cancel_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_view_log_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_open_dir_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_open_in_aegisub_, 0, wxRIGHT, 6);
		btn_sizer->AddStretchSpacer(1);
		btn_sizer->Add(btn_close_, 0);

		main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

		SetSizer(main_sizer);
		Centre();

		// 事件绑定
		InitProjectInfo();

		btn_browse_ffmpeg_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			wxFileDialog dlg(this, to_wx("选择 FFmpeg 可执行程序 (ffmpeg.exe)"), "", "ffmpeg.exe",
			                 to_wx("FFmpeg 可执行文件 (ffmpeg.exe)|ffmpeg.exe;*.exe|所有文件 (*.*)|*.*"),
			                 wxFD_OPEN | wxFD_FILE_MUST_EXIST);
			if (dlg.ShowModal() == wxID_OK) {
				s_custom_ffmpeg_path = dlg.GetPath().ToStdWstring();
				ffmpeg_status_lbl_->SetLabel(fmt_wx("🟢 FFmpeg 已指定: %s", s_custom_ffmpeg_path));
				ffmpeg_status_lbl_->SetForegroundColour(wxColour(30, 130, 30));
			}
		});

		browse_dir_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			wxDirDialog dlg(this, to_wx("选择切片输出目录"), out_dir_ctrl_->GetValue(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
			if (dlg.ShowModal() == wxID_OK) {
				out_dir_ctrl_->SetValue(dlg.GetPath());
			}
		});

		btn_start_->Bind(wxEVT_BUTTON, &DialogVideoShuffler::OnStartProcess, this);

		btn_cancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) {
				cancel_->store(true);
				status_lbl_->SetLabel(to_wx("正在停止 FFmpeg 任务并清理临时文件..."));
				btn_cancel_->Disable();
			}
		});

		btn_view_log_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			ShowLogDialog();
		});

		btn_open_dir_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (!last_result_.output_media_files.empty()) {
				std::wstring target = last_result_.primary_media_file.empty() ? last_result_.output_media_files.front() : last_result_.primary_media_file;
				std::wstring cmd = L"explorer.exe /select,\"" + target + L"\"";
				_wsystem(cmd.c_str());
			} else {
				std::wstring wdir = out_dir_ctrl_->GetValue().ToStdWstring();
				std::wstring cmd = L"explorer.exe \"" + wdir + L"\"";
				_wsystem(cmd.c_str());
			}
		});

		btn_open_in_aegisub_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (c_ && c_->project && !last_result_.primary_media_file.empty()) {
				c_->project->LoadVideo(agi::fs::path(std::filesystem::path(last_result_.primary_media_file)));
				if (!last_result_.primary_ass_file.empty()) {
					c_->project->LoadSubtitles(agi::fs::path(std::filesystem::path(last_result_.primary_ass_file)));
				}
				EndModal(wxID_OK);
			}
		});

		btn_close_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) cancel_->store(true);
			EndModal(wxID_CANCEL);
		});
	}

	~DialogVideoShuffler() override {
		if (alive_) alive_->store(false);
		if (running_) cancel_->store(true);
		if (worker_thread_.joinable()) worker_thread_.join();
	}

	void OnStartProcess(wxCommandEvent&) {
		if (running_) return;

		std::wstring ffmpeg_exe = VideoShuffler::FindFFmpegExecutable(s_custom_ffmpeg_path);
		if (ffmpeg_exe.empty()) {
			wxMessageBox(
				to_wx("未检测到 FFmpeg (ffmpeg.exe)。\n\n请点击【选择 FFmpeg...】按钮定位 ffmpeg.exe，\n或将 ffmpeg.exe 复制到 Aegisub 安装目录下，\n或安装 FFmpeg 并将其 bin 目录添加至系统环境变量 PATH。"),
				to_wx("缺少 FFmpeg 组件"), wxOK | wxICON_WARNING, this);
			return;
		}

		VideoShuffler::ShufflerConfig opt;
		opt.ffmpeg_path = ffmpeg_exe;

		if (rb_mode_highlight_->GetValue()) opt.mode = VideoShuffler::MODE_MERGE_HIGHLIGHT;
		else if (rb_mode_chapters_->GetValue()) opt.mode = VideoShuffler::MODE_SPLIT_CHAPTERS;
		else opt.mode = VideoShuffler::MODE_SELECTED_LINES;

		if (rb_fmt_mp4_->GetValue()) opt.format = VideoShuffler::FMT_MP4;
		else if (rb_fmt_mkv_->GetValue()) opt.format = VideoShuffler::FMT_MKV;
		else if (rb_fmt_mp3_->GetValue()) opt.format = VideoShuffler::FMT_MP3;
		else if (rb_fmt_wav_->GetValue()) opt.format = VideoShuffler::FMT_WAV;

		opt.fast_stream_copy = rb_engine_fast_->GetValue();
		opt.gap_threshold_ms = spin_gap_threshold_->GetValue();
		opt.lead_in_ms = spin_leadin_->GetValue();
		opt.lead_out_ms = spin_leadout_->GetValue();
		opt.generate_synced_ass = chk_sync_ass_->GetValue();
		opt.output_dir = out_dir_ctrl_->GetValue().ToStdWstring();
		opt.output_prefix = prefix_ctrl_->GetValue().ToStdWstring();

		running_ = true;
		cancel_->store(false);

		btn_start_->Disable();
		btn_cancel_->Enable();
		btn_view_log_->Disable();
		btn_open_dir_->Disable();
		btn_open_in_aegisub_->Disable();
		btn_close_->Disable();
		progress_gauge_->SetValue(0);

		auto alive = alive_;
		auto cancel = cancel_;

		if (worker_thread_.joinable()) worker_thread_.join();

		worker_thread_ = std::thread([this, alive, cancel, opt]() {
			auto res = VideoShuffler::RunVideoShuffler(
				c_,
				opt,
				selected_lines_,
				[this, alive](int pct, const wxString& status) {
					wxTheApp->CallAfter([this, alive, pct, status]() {
						if (!alive->load()) return;
						progress_gauge_->SetValue(std::clamp(pct, 0, 100));
						status_lbl_->SetLabel(status);
					});
				},
				cancel
			);

			wxTheApp->CallAfter([this, alive, res]() {
				if (!alive->load()) return;
				running_ = false;
				last_result_ = res;

				btn_start_->Enable();
				btn_cancel_->Disable();
				btn_view_log_->Enable();
				btn_close_->Enable();

				if (res.success) {
					progress_gauge_->SetValue(100);
					status_lbl_->SetLabel(fmt_wx("切分完成！共输出 %d 个文件。", static_cast<int>(res.output_media_files.size())));
					btn_open_dir_->Enable();
					btn_open_in_aegisub_->Enable();

					wxMessageBox(
						fmt_wx("按字幕切分与合并已圆满完成！\n\n共处理输出 %d 个切片文件及配套字幕。\n\n点击【在 Aegisub 中载入切片工程】可立即预览新视频与同步字幕。\n点击【查看执行日志】可查看切片合并完整报告。",
						       static_cast<int>(res.output_media_files.size())),
						to_wx("切分合并成功"), wxOK | wxICON_INFORMATION, this);
				} else {
					status_lbl_->SetLabel(to_wx("切分处理失败！请点击右侧【查看执行日志】排查详细原因"));
					int ret = wxMessageBox(
						res.message + to_wx("\n\n是否立即打开并查看详细的 FFmpeg 执行诊断日志？"),
						to_wx("切分合并失败"), wxYES_NO | wxICON_ERROR, this);
					if (ret == wxYES) {
						ShowLogDialog();
					}
				}
			});
		});
	}
};

inline void ShowVideoShufflerDialog(agi::Context* c) {
	if (!c) return;
	DialogVideoShuffler dlg(c->frame, c);
	dlg.ShowModal();
}
