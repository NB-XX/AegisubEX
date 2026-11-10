// Copyright (c) 2026, Aegisub Project
// All rights reserved.
//
// Native In-App Motion Tracking Dialog for Aegisub.

#pragma once

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "motion_tracker.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "visual_tool_cross.h"
#include "visual_tool_motion_track.h"
#include "format.h"

#include <libaegisub/format.h>

#include <atomic>
#include <memory>
#include <thread>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

class DialogMotionTracker : public wxDialog {
private:
	agi::Context* c_;
	std::shared_ptr<std::atomic<bool>> cancel_;
	std::shared_ptr<std::atomic<bool>> alive_;
	std::atomic<bool> running_{false};
	std::thread worker_thread_;

	AssDialogue* target_line_ = nullptr;
	int start_frame_ = 0;
	int end_frame_ = 0;
	int total_line_frames_ = 0;

	// UI 控件
	wxStaticText* line_info_lbl_ = nullptr;
	wxStaticText* frame_range_lbl_ = nullptr;

	wxSpinCtrl* box_x_ctrl_ = nullptr;
	wxSpinCtrl* box_y_ctrl_ = nullptr;
	wxSpinCtrl* box_w_ctrl_ = nullptr;
	wxSpinCtrl* box_h_ctrl_ = nullptr;
	wxButton* pick_box_btn_ = nullptr;

	wxSpinCtrl* ref_frame_ctrl_ = nullptr;
	wxButton* set_cur_frame_btn_ = nullptr;

	wxRadioButton* rb_relative_ = nullptr;
	wxRadioButton* rb_absolute_ = nullptr;

	wxCheckBox* chk_smooth_ = nullptr;
	wxSpinCtrl* spin_smooth_win_ = nullptr;
	wxSpinCtrl* spin_search_radius_ = nullptr;
	wxCheckBox* chk_split_frame_ = nullptr;
	wxCheckBox* chk_merge_static_ = nullptr;
	wxCheckBox* chk_fold_in_grid_ = nullptr;
	wxCheckBox* chk_backup_ = nullptr;

	wxGauge* progress_gauge_ = nullptr;
	wxStaticText* status_lbl_ = nullptr;

	wxButton* btn_track_and_apply_ = nullptr;
	wxButton* btn_repick_ = nullptr;
	wxButton* btn_copy_mocha_ = nullptr;
	wxButton* btn_cancel_ = nullptr;
	wxButton* btn_close_ = nullptr;

	MotionTrack::MotionTrajectory last_trajectory_;

	void UpdateLineInfo() {
		if (!c_ || !c_->selectionController) return;
		target_line_ = c_->selectionController->GetActiveLine();
		if (!target_line_ || !c_->videoController) {
			line_info_lbl_->SetLabel(to_wx("未选中字幕行或未加载视频"));
			frame_range_lbl_->SetLabel("");
			return;
		}

		auto vctrl = c_->videoController.get();
		start_frame_ = vctrl->FrameAtTime(target_line_->Start, agi::vfr::START);
		end_frame_ = vctrl->FrameAtTime(target_line_->End, agi::vfr::END);
		if (start_frame_ > end_frame_) std::swap(start_frame_, end_frame_);
		total_line_frames_ = end_frame_ - start_frame_ + 1;

		wxString text_snippet = to_wx(target_line_->Text.get());
		if (text_snippet.length() > 36) text_snippet = text_snippet.Left(33) + "...";

		line_info_lbl_->SetLabel(fmt_wx("当前字幕: %s", text_snippet));
		frame_range_lbl_->SetLabel(fmt_wx("时间区间: %s ~ %s  (帧号: %d ~ %d, 共 %d 帧)",
			to_wx(target_line_->Start.GetAssFormatted()),
			to_wx(target_line_->End.GetAssFormatted()),
			start_frame_, end_frame_, total_line_frames_));

		// 若当前视频帧在区间内，基准帧优先取当前视频帧，否则取起始帧
		int cur_f = vctrl->GetFrameN();
		if (cur_f >= start_frame_ && cur_f <= end_frame_) {
			ref_frame_ctrl_->SetValue(cur_f);
		} else {
			ref_frame_ctrl_->SetValue(start_frame_);
		}
	}

	void SyncBoxFromVisualTool() {
		int x0, y0, x1, y1;
		if (VisualToolMotionTrack::GetCurrentBox(x0, y0, x1, y1)) {
			box_x_ctrl_->SetValue(x0);
			box_y_ctrl_->SetValue(y0);
			box_w_ctrl_->SetValue(x1 - x0);
			box_h_ctrl_->SetValue(y1 - y0);
			status_lbl_->SetLabel(fmt_wx("已选择花字选框: [%d, %d] 宽:%d 高:%d", x0, y0, x1 - x0, y1 - y0));
		}
	}

public:
	DialogMotionTracker(wxWindow* parent, agi::Context* c)
	: wxDialog(parent, wxID_ANY, to_wx("视频运动追踪与相对位移 (Motion Tracking)"),
		wxDefaultPosition, wxSize(620, 680),
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, c_(c)
	, cancel_(std::make_shared<std::atomic<bool>>(false))
	, alive_(std::make_shared<std::atomic<bool>>(true))
	{
		auto* main_sizer = new wxBoxSizer(wxVERTICAL);

		// 1. 字幕行信息
		auto* info_box = new wxStaticBox(this, wxID_ANY, to_wx("目标字幕行"));
		auto* info_sizer = new wxStaticBoxSizer(info_box, wxVERTICAL);

		line_info_lbl_ = new wxStaticText(info_box, wxID_ANY, to_wx("当前字幕: "));
		wxFont bold_font = line_info_lbl_->GetFont();
		bold_font.SetWeight(wxFONTWEIGHT_BOLD);
		line_info_lbl_->SetFont(bold_font);
		info_sizer->Add(line_info_lbl_, 0, wxALL, 4);

		frame_range_lbl_ = new wxStaticText(info_box, wxID_ANY, to_wx("时间区间: "));
		frame_range_lbl_->SetForegroundColour(wxColour(80, 80, 80));
		info_sizer->Add(frame_range_lbl_, 0, wxALL, 4);

		main_sizer->Add(info_sizer, 0, wxEXPAND | wxALL, 8);

		// 2. 追踪目标选框
		auto* box_box = new wxStaticBox(this, wxID_ANY, to_wx("运动花字目标选框 (ROI)"));
		auto* box_sizer = new wxStaticBoxSizer(box_box, wxVERTICAL);

		auto* coord_sizer = new wxBoxSizer(wxHORIZONTAL);
		box_x_ctrl_ = new wxSpinCtrl(box_box, wxID_ANY, "100", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 0, 8192, 100);
		box_y_ctrl_ = new wxSpinCtrl(box_box, wxID_ANY, "100", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 0, 8192, 100);
		box_w_ctrl_ = new wxSpinCtrl(box_box, wxID_ANY, "160", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 10, 2048, 160);
		box_h_ctrl_ = new wxSpinCtrl(box_box, wxID_ANY, "80", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 10, 2048, 80);

		coord_sizer->Add(new wxStaticText(box_box, wxID_ANY, to_wx("X:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		coord_sizer->Add(box_x_ctrl_, 0, wxRIGHT, 8);
		coord_sizer->Add(new wxStaticText(box_box, wxID_ANY, to_wx("Y:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		coord_sizer->Add(box_y_ctrl_, 0, wxRIGHT, 8);
		coord_sizer->Add(new wxStaticText(box_box, wxID_ANY, to_wx("宽:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		coord_sizer->Add(box_w_ctrl_, 0, wxRIGHT, 8);
		coord_sizer->Add(new wxStaticText(box_box, wxID_ANY, to_wx("高:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		coord_sizer->Add(box_h_ctrl_, 0, wxRIGHT, 12);

		pick_box_btn_ = new wxButton(box_box, wxID_ANY, to_wx("🎯 重新在画面上框选花字"));
		pick_box_btn_->SetToolTip(to_wx("关闭此设置面板，回到视频画面上重新拖拽框选花字，框选完毕后自动重新弹出本面板"));
		coord_sizer->Add(pick_box_btn_, 1, wxALIGN_CENTER_VERTICAL);

		box_sizer->Add(coord_sizer, 0, wxEXPAND | wxALL, 6);
		main_sizer->Add(box_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 3. 基准参考帧设置
		auto* ref_box = new wxStaticBox(this, wxID_ANY, to_wx("对齐基准参考帧 (Reference Frame)"));
		auto* ref_sizer = new wxStaticBoxSizer(ref_box, wxVERTICAL);

		auto* ref_row = new wxBoxSizer(wxHORIZONTAL);
		ref_row->Add(new wxStaticText(ref_box, wxID_ANY, to_wx("基准帧号:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		ref_frame_ctrl_ = new wxSpinCtrl(ref_box, wxID_ANY, "0", wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, 9999999, 0);
		ref_row->Add(ref_frame_ctrl_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

		set_cur_frame_btn_ = new wxButton(ref_box, wxID_ANY, to_wx("设为当前播放指针所在帧"));
		ref_row->Add(set_cur_frame_btn_, 0, wxALIGN_CENTER_VERTICAL);

		ref_sizer->Add(ref_row, 0, wxEXPAND | wxALL, 6);
		auto* ref_hint = new wxStaticText(ref_box, wxID_ANY, to_wx("说明：在此帧将字幕调到满意的位置。追踪时字幕将以此帧为锚点，跟随花字做相对运动。"));
		ref_hint->SetForegroundColour(wxColour(110, 110, 110));
		ref_sizer->Add(ref_hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

		main_sizer->Add(ref_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 4. 运动模式与平滑选项
		auto* opt_box = new wxStaticBox(this, wxID_ANY, to_wx("运动模式与平滑处理"));
		auto* opt_sizer = new wxStaticBoxSizer(opt_box, wxVERTICAL);

		rb_relative_ = new wxRadioButton(opt_box, wxID_ANY, to_wx("相对位移运动 (推荐：字幕保持与花字的相对轨迹同步运动)"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
		rb_relative_->SetValue(true);
		opt_sizer->Add(rb_relative_, 0, wxALL, 4);

		rb_absolute_ = new wxRadioButton(opt_box, wxID_ANY, to_wx("绝对坐标跟随 (字幕中心直接跟随花字追踪框中心)"));
		opt_sizer->Add(rb_absolute_, 0, wxALL, 4);

		auto* grid_opts = new wxFlexGridSizer(2, wxSize(10, 6));
		chk_smooth_ = new wxCheckBox(opt_box, wxID_ANY, to_wx("轨迹平滑滤波 (去除视频高频微抖动)"));
		chk_smooth_->SetValue(true);
		grid_opts->Add(chk_smooth_, 0, wxALIGN_CENTER_VERTICAL);

		auto* win_sizer = new wxBoxSizer(wxHORIZONTAL);
		win_sizer->Add(new wxStaticText(opt_box, wxID_ANY, to_wx("平滑窗口:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		spin_smooth_win_ = new wxSpinCtrl(opt_box, wxID_ANY, "3", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 1, 9, 3);
		win_sizer->Add(spin_smooth_win_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		win_sizer->Add(new wxStaticText(opt_box, wxID_ANY, to_wx("帧")), 0, wxALIGN_CENTER_VERTICAL);
		grid_opts->Add(win_sizer, 0, wxALIGN_CENTER_VERTICAL);

		chk_split_frame_ = new wxCheckBox(opt_box, wxID_ANY, to_wx("逐帧切分生成字幕 (1 行 / 帧，带精确 \\pos)"));
		chk_split_frame_->SetValue(true);
		grid_opts->Add(chk_split_frame_, 0, wxALIGN_CENTER_VERTICAL);

		auto* radius_sizer = new wxBoxSizer(wxHORIZONTAL);
		radius_sizer->Add(new wxStaticText(opt_box, wxID_ANY, to_wx("搜索半径:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		spin_search_radius_ = new wxSpinCtrl(opt_box, wxID_ANY, "40", wxDefaultPosition, wxSize(70, -1), wxSP_ARROW_KEYS, 15, 120, 40);
		radius_sizer->Add(spin_search_radius_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
		radius_sizer->Add(new wxStaticText(opt_box, wxID_ANY, to_wx("px")), 0, wxALIGN_CENTER_VERTICAL);
		grid_opts->Add(radius_sizer, 0, wxALIGN_CENTER_VERTICAL);

		chk_backup_ = new wxCheckBox(opt_box, wxID_ANY, to_wx("保留原字幕为注释行备份 (支持随时 Ctrl+Z 一键撤销)"));
		chk_backup_->SetValue(true);
		grid_opts->Add(chk_backup_, 0, wxALIGN_CENTER_VERTICAL);

		chk_merge_static_ = new wxCheckBox(opt_box, wxID_ANY, to_wx("静止帧智能合并 (定格/静止帧自动合并为单行)"));
		chk_merge_static_->SetValue(true);
		chk_merge_static_->SetToolTip(to_wx("当花字飞入定格或位移不动时，自动将连续多帧合并为单行并延长其时间区间，极大减少冗余字幕行"));
		grid_opts->Add(chk_merge_static_, 0, wxALIGN_CENTER_VERTICAL);

		chk_fold_in_grid_ = new wxCheckBox(opt_box, wxID_ANY, to_wx("字幕列表中折叠显示 (折叠为 1 行摘要，双击展开)"));
		chk_fold_in_grid_->SetValue(true);
		chk_fold_in_grid_->SetToolTip(to_wx("在字幕列表中将全部追踪帧折叠为单行摘要 (▶ [运动追踪])，保持时间轴清爽；双击该行可随时展开/收起"));
		grid_opts->Add(chk_fold_in_grid_, 0, wxALIGN_CENTER_VERTICAL);

		opt_sizer->Add(grid_opts, 0, wxEXPAND | wxALL, 6);
		main_sizer->Add(opt_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 5. 进度条与状态
		progress_gauge_ = new wxGauge(this, wxID_ANY, 100);
		main_sizer->Add(progress_gauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		status_lbl_ = new wxStaticText(this, wxID_ANY, to_wx("就绪。请确认花字选框无误后点击下方【开始追踪并应用】。"));
		status_lbl_->SetForegroundColour(wxColour(100, 100, 100));
		main_sizer->Add(status_lbl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		// 6. 操作按钮
		auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
		btn_track_and_apply_ = new wxButton(this, wxID_ANY, to_wx("⚡ 一键追踪并应用到字幕"));
		btn_track_and_apply_->SetDefault();
		btn_repick_ = new wxButton(this, wxID_ANY, to_wx("🎯 重新框选"));
		btn_repick_->SetToolTip(to_wx("关闭面板并在视频画面上重新拖拽框选花字目标"));
		btn_copy_mocha_ = new wxButton(this, wxID_ANY, to_wx("📋 复制 Mocha 关键帧数据"));
		btn_cancel_ = new wxButton(this, wxID_ANY, to_wx("取消"));
		btn_cancel_->Disable();
		btn_close_ = new wxButton(this, wxID_ANY, to_wx("关闭"));

		btn_sizer->Add(btn_track_and_apply_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_repick_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_copy_mocha_, 0, wxRIGHT, 6);
		btn_sizer->Add(btn_cancel_, 0, wxRIGHT, 6);
		btn_sizer->AddStretchSpacer(1);
		btn_sizer->Add(btn_close_, 0);

		main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);

		SetSizerAndFit(main_sizer);
		Centre();

		// 事件绑定
		UpdateLineInfo();
		SyncBoxFromVisualTool();

		VisualToolMotionTrack::SetBoxCallback([this](int x0, int y0, int x1, int y1) {
			box_x_ctrl_->SetValue(x0);
			box_y_ctrl_->SetValue(y0);
			box_w_ctrl_->SetValue(x1 - x0);
			box_h_ctrl_->SetValue(y1 - y0);
			status_lbl_->SetLabel(fmt_wx("已选框选目标: [%d, %d] 宽:%d 高:%d", x0, y0, x1 - x0, y1 - y0));
		});

		pick_box_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) return;
			EndModal(1001);
		});

		btn_repick_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) return;
			EndModal(1001);
		});

		set_cur_frame_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (c_ && c_->videoController) {
				ref_frame_ctrl_->SetValue(c_->videoController->GetFrameN());
			}
		});

		btn_track_and_apply_->Bind(wxEVT_BUTTON, &DialogMotionTracker::OnTrackAndApply, this);
		btn_copy_mocha_->Bind(wxEVT_BUTTON, &DialogMotionTracker::OnCopyMochaData, this);

		btn_cancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) {
				cancel_->store(true);
				status_lbl_->SetLabel(to_wx("正在取消追踪..."));
				btn_cancel_->Disable();
			}
		});

		btn_close_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (running_) cancel_->store(true);
			EndModal(wxID_CANCEL);
		});
	}

	~DialogMotionTracker() override {
		VisualToolMotionTrack::SetBoxCallback(nullptr);
		if (alive_) alive_->store(false);
		if (running_) cancel_->store(true);
		if (worker_thread_.joinable()) worker_thread_.join();
	}

	void OnTrackAndApply(wxCommandEvent&) {
		if (running_) return;
		if (!c_ || !c_->project->VideoProvider()) {
			wxMessageBox(to_wx("请先打开视频文件。"), to_wx("提示"), wxOK | wxICON_WARNING, this);
			return;
		}
		if (!target_line_) {
			wxMessageBox(to_wx("请先在字幕表格中选择要追踪的字幕行。"), to_wx("提示"), wxOK | wxICON_WARNING, this);
			return;
		}

		int box_x = box_x_ctrl_->GetValue();
		int box_y = box_y_ctrl_->GetValue();
		int box_w = box_w_ctrl_->GetValue();
		int box_h = box_h_ctrl_->GetValue();
		if (box_w < 8 || box_h < 8) {
			wxMessageBox(to_wx("追踪选框过小，请先点击【在画面上框选花字】拖拽框选完整目标。"), to_wx("提示"), wxOK | wxICON_WARNING, this);
			return;
		}

		int ref_frame = ref_frame_ctrl_->GetValue();
		int start_frame = start_frame_;
		int end_frame = end_frame_;

		MotionTrack::TrackingOptions opt;
		opt.relative_mode = rb_relative_->GetValue();
		opt.smooth_window = chk_smooth_->GetValue() ? spin_smooth_win_->GetValue() : 1;
		opt.search_radius = spin_search_radius_->GetValue();
		opt.split_per_frame = chk_split_frame_->GetValue();
		opt.merge_static = chk_merge_static_->GetValue();
		opt.fold_in_grid = chk_fold_in_grid_->GetValue();
		opt.backup_original = chk_backup_->GetValue();

		running_ = true;
		cancel_->store(false);
		btn_track_and_apply_->Disable();
		btn_copy_mocha_->Disable();
		btn_cancel_->Enable();
		btn_close_->Disable();
		progress_gauge_->SetValue(0);
		status_lbl_->SetLabel(to_wx("正在执行多尺度光流/NCC目标追踪..."));

		if (worker_thread_.joinable()) worker_thread_.join();

		auto alive = alive_;
		auto cancel = cancel_;

		worker_thread_ = std::thread([this, alive, cancel, ref_frame, box_x, box_y, box_w, box_h, start_frame, end_frame, opt]() {
			auto on_prog = [this, alive](int cur, int total, double cur_x, double cur_y) {
				wxTheApp->CallAfter([this, alive, cur, total, cur_x, cur_y]() {
					if (!alive->load()) return;
					if (total > 0 && progress_gauge_) {
						progress_gauge_->SetValue(std::clamp(static_cast<int>(cur * 100 / total), 0, 100));
					}
					if (status_lbl_) {
						status_lbl_->SetLabel(fmt_wx("正在追踪第 %d/%d 帧 (坐标: X=%.1f, Y=%.1f)...", cur, total, cur_x, cur_y));
					}
				});
			};

			MotionTrack::MotionTrajectory traj = MotionTrack::TrackFullRange(
				c_, ref_frame, box_x, box_y, box_w, box_h, start_frame, end_frame, opt, on_prog, cancel);

			wxTheApp->CallAfter([this, alive, traj, opt]() {
				if (!alive->load()) return;
				running_ = false;
				btn_track_and_apply_->Enable();
				btn_copy_mocha_->Enable();
				btn_cancel_->Disable();
				btn_close_->Enable();

				if (cancel_->load()) {
					status_lbl_->SetLabel(to_wx("已取消追踪"));
					return;
				}

				if (traj.frames.empty()) {
					status_lbl_->SetLabel(to_wx("追踪失败：未获取到有效图像"));
					return;
				}

				last_trajectory_ = traj;
				progress_gauge_->SetValue(100);

				// 将追踪生成的相对位移写入 ASS 字幕行
				int count = MotionTrack::ApplyTrajectoryToLine(c_, target_line_, traj, opt);
				status_lbl_->SetLabel(fmt_wx("追踪成功！已生成 %d 帧相对运动字幕。", count));

				wxMessageBox(fmt_wx("运动追踪完成！\n已成功生成 %d 行逐帧相对位移字幕。\n\n如需调整，可随时按 Ctrl+Z 一键撤销还原。", count),
					to_wx("追踪成功"), wxOK | wxICON_INFORMATION, this);

				// 更新字幕视图并定位到首帧
				if (c_->videoController) {
					c_->videoController->JumpToFrame(traj.ref_frame);
				}
			});
		});
	}

	void OnCopyMochaData(wxCommandEvent&) {
		if (last_trajectory_.frames.empty()) {
			// 若尚未追踪过，先跑一次
			wxMessageBox(to_wx("请先点击【一键追踪并应用到字幕】生成追踪轨迹数据。"), to_wx("提示"), wxOK | wxICON_INFORMATION, this);
			return;
		}

		std::string mocha_data = MotionTrack::BuildMochaClipboardData(
			last_trajectory_,
			ref_frame_ctrl_->GetValue(),
			last_trajectory_.video_width,
			last_trajectory_.video_height,
			last_trajectory_.fps);

		if (wxTheClipboard->Open()) {
			wxTheClipboard->SetData(new wxTextDataObject(to_wx(mocha_data)));
			wxTheClipboard->Close();
			wxMessageBox(fmt_wx("已成功将 %d 帧的 Mocha 关键帧数据（Adobe After Effects 6.0 Keyframe Data）复制到剪贴板！\n\n可直接粘贴至 Aegisub-Motion 或外部软件。",
				static_cast<int>(last_trajectory_.frames.size())),
				to_wx("复制成功"), wxOK | wxICON_INFORMATION, this);
		}
	}
};

inline void ShowMotionTrackerDialog(agi::Context* c) {
	if (!c) return;
	VisualToolMotionTrack::SetBoxFinishedCallback([](agi::Context* ctx) {
		ShowMotionTrackerDialog(ctx);
	});
	DialogMotionTracker dlg(c->frame, c);
	int ret = dlg.ShowModal();
	if (ret == 1001) {
		if (c->videoDisplay) {
			c->videoDisplay->SetTool(std::make_unique<VisualToolMotionTrack>(c->videoDisplay, c));
		}
	}
}
