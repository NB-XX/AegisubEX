// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//	 this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//	 this list of conditions and the following disclaimer in the documentation
//	 and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//	 may be used to endorse or promote products derived from this software
//	 without specific prior written permission.
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

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../async_video_provider.h"
#include "../base_grid.h"
#include "../compat.h"
#include "../dialog_detached_video.h"
#include "../dialog_manager.h"
#include "../dialogs.h"
#include "../dialog_motion_tracker.h"
#include "../dialog_video_shuffler.h"
#include "../format.h"
#include "../frame_main.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/subtitles_provider.h"
#include "../libresrc/libresrc.h"
#include "../options.h"
#include "../project.h"
#include "../provider_file_formats.h"
#include "../video_provider_manager.h"
#include "../selection_controller.h"
#include "../utils.h"
#include "../video_controller.h"
#include "../video_display.h"
#include "../video_frame.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>

namespace {
	using cmd::Command;

struct validator_video_loaded : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider();
	}
};

struct validator_video_attached : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() && !c->dialog->Get<DialogDetachedVideo>();
	}
};

struct video_aspect_cinematic final : public validator_video_loaded {
	CMD_NAME("video/aspect/cinematic")
	STR_MENU("&Cinematic (2.35)")
	STR_DISP("Cinematic (2.35)")
	STR_HELP("Force video to 2.35 aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Cinematic;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Cinematic);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_aspect_custom final : public validator_video_loaded {
	CMD_NAME("video/aspect/custom")
	STR_MENU("C&ustom...")
	STR_DISP("Custom")
	STR_HELP("Force video to a custom aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Custom;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();

		std::string value = from_wx(wxGetTextFromUser(
			_("Enter aspect ratio in either:\n  decimal (e.g. 2.35)\n  fractional (e.g. 16:9)\n  specific resolution (e.g. 853x480)"),
			_("Enter aspect ratio"),
			std::to_wstring(c->videoController->GetAspectRatioValue())));
		if (value.empty()) return;

		double numval = 0;
		if (agi::util::try_parse(value, &numval)) {
			//Nothing to see here, move along
		}
		else {
			std::vector<std::string> chunks;
			split(chunks, value, boost::is_any_of(":/xX"));
			if (chunks.size() == 2) {
				double num, den;
				if (agi::util::try_parse(chunks[0], &num) && agi::util::try_parse(chunks[1], &den))
					numval = num / den;
			}
		}

		if (numval < 0.5 || numval > 5.0)
			wxMessageBox(_("Invalid value! Aspect ratio must be between 0.5 and 5.0."),_("Invalid Aspect Ratio"),wxOK | wxICON_ERROR | wxCENTER);
		else {
			c->videoController->SetAspectRatio(numval);
			c->frame->SetDisplayMode(1,-1);
		}
	}
};

struct video_aspect_default final : public validator_video_loaded {
	CMD_NAME("video/aspect/default")
	STR_MENU("&Default")
	STR_DISP("Default")
	STR_HELP("Use video's original aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Default;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Default);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_aspect_full final : public validator_video_loaded {
	CMD_NAME("video/aspect/full")
	STR_MENU("&Fullscreen (4:3)")
	STR_DISP("Fullscreen (4:3)")
	STR_HELP("Force video to 4:3 aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Fullscreen;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Fullscreen);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_aspect_wide final : public validator_video_loaded {
	CMD_NAME("video/aspect/wide")
	STR_MENU("&Widescreen (16:9)")
	STR_DISP("Widescreen (16:9)")
	STR_HELP("Force video to 16:9 aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Widescreen;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Widescreen);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_close final : public validator_video_loaded {
	CMD_NAME("video/close")
	CMD_ICON(close_video_menu)
	STR_MENU("&Close Video")
	STR_DISP("Close Video")
	STR_HELP("Close the currently open video file")

	void operator()(agi::Context *c) override {
		c->project->CloseVideo();
	}
};

struct video_copy_coordinates final : public validator_video_loaded {
	CMD_NAME("video/copy_coordinates")
	STR_MENU("Copy coordinates to Clipboard")
	STR_DISP("Copy coordinates to Clipboard")
	STR_HELP("Copy the current coordinates of the mouse over the video to the clipboard")

	void operator()(agi::Context *c) override {
		SetClipboard(c->videoDisplay->GetMousePosition().Str());
	}
};

struct video_cycle_subtitles_provider final : public cmd::Command {
	CMD_NAME("video/subtitles_provider/cycle")
	STR_MENU("Cycle active subtitles provider")
	STR_DISP("Cycle active subtitles provider")
	STR_HELP("Cycle through the available subtitles providers")

	void operator()(agi::Context *c) override {
		auto providers = SubtitlesProviderFactory::GetClasses();
		if (providers.empty()) return;

		auto it = find(begin(providers), end(providers), OPT_GET("Subtitle/Provider")->GetString());
		if (it != end(providers)) ++it;
		if (it == end(providers)) it = begin(providers);

		OPT_SET("Subtitle/Provider")->SetString(*it);
		c->frame->StatusTimeout(fmt_tl("Subtitles provider set to %s", *it), 5000);
	}
};

struct video_detach final : public validator_video_loaded {
	CMD_NAME("video/detach")
	CMD_ICON(detach_video_menu)
	STR_MENU("&Detach Video")
	STR_DISP("Detach Video")
	STR_HELP("Detach the video display from the main window, displaying it in a separate Window")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

	bool IsActive(const agi::Context *c) override {
		return !!c->dialog->Get<DialogDetachedVideo>();
	}

	void operator()(agi::Context *c) override {
		if (DialogDetachedVideo *d = c->dialog->Get<DialogDetachedVideo>())
			d->Close();
		else
			c->dialog->Show<DialogDetachedVideo>(c);
	}
};

struct video_details final : public validator_video_loaded {
	CMD_NAME("video/details")
	CMD_ICON(show_video_details_menu)
	STR_MENU("Show &Video Details")
	STR_DISP("Show Video Details")
	STR_HELP("Show video details")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowVideoDetailsDialog(c);
	}
};

struct video_motion_track final : public validator_video_loaded {
	CMD_NAME("video/motion_track")
	wxString StrMenu(const agi::Context *) const override { return to_wx("运动追踪与相对位移 (Motion Tracking)..."); }
	wxString StrDisplay(const agi::Context *) const override { return to_wx("运动追踪与相对位移"); }
	wxString StrHelp() const override { return to_wx("在视频画面上拖拽框选花字目标并生成逐帧相对位移字幕"); }

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<path d=\"M4 18 C8 18 8 8 16 8\" fill=\"none\" stroke=\"#E67E22\" stroke-width=\"2\" stroke-dasharray=\"2,2\"/>"
			"<circle cx=\"4\" cy=\"18\" r=\"2\" fill=\"#E67E22\"/>"
			"<circle cx=\"16\" cy=\"8\" r=\"5\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<line x1=\"16\" y1=\"1\" x2=\"16\" y2=\"5\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<line x1=\"16\" y1=\"11\" x2=\"16\" y2=\"15\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<line x1=\"9\" y1=\"8\" x2=\"13\" y2=\"8\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<line x1=\"19\" y1=\"8\" x2=\"23\" y2=\"8\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<circle cx=\"16\" cy=\"8\" r=\"1.5\" fill=\"#E74C3C\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetTool(std::make_unique<VisualToolMotionTrack>(c->videoDisplay, c));
	}
};

struct video_shuffler final : public validator_video_loaded {
	CMD_NAME("video/shuffler")
	wxString StrMenu(const agi::Context *) const override { return to_wx("按字幕切分/合并视频 (Video Shuffler)..."); }
	wxString StrDisplay(const agi::Context *) const override { return to_wx("按字幕切片/合并"); }
	wxString StrHelp() const override { return to_wx("基于字幕时间轴、注释与章节标记，自动对视频进行精简切分、去除静音、打乱重组与批量分段"); }

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<rect x=\"2\" y=\"4\" width=\"20\" height=\"16\" rx=\"2\" fill=\"none\" stroke=\"#2C3E50\" stroke-width=\"2\"/>"
			"<line x1=\"2\" y1=\"8\" x2=\"22\" y2=\"8\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"2\" y1=\"16\" x2=\"22\" y2=\"16\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"6\" y1=\"4\" x2=\"6\" y2=\"8\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"12\" y1=\"4\" x2=\"12\" y2=\"8\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"18\" y1=\"4\" x2=\"18\" y2=\"8\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"6\" y1=\"16\" x2=\"6\" y2=\"20\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"12\" y1=\"16\" x2=\"12\" y2=\"20\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"18\" y1=\"16\" x2=\"18\" y2=\"20\" stroke=\"#2C3E50\" stroke-width=\"1.5\"/>"
			"<line x1=\"11\" y1=\"8\" x2=\"11\" y2=\"16\" stroke=\"#E74C3C\" stroke-width=\"2\" stroke-dasharray=\"2,1\"/>"
			"<circle cx=\"15\" cy=\"11\" r=\"1.5\" fill=\"#E67E22\"/>"
			"<circle cx=\"15\" cy=\"14\" r=\"1.5\" fill=\"#E67E22\"/>"
			"<path d=\"M14 11 L10 13 M14 14 L10 12\" stroke=\"#E67E22\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowVideoShufflerDialog(c);
	}
};

struct video_focus_seek final : public validator_video_loaded {
	CMD_NAME("video/focus_seek")
	STR_MENU("Toggle video slider focus")
	STR_DISP("Toggle video slider focus")
	STR_HELP("Toggle focus between the video slider and the previous thing to have focus")

	void operator()(agi::Context *c) override {
		wxWindow *curFocus = wxWindow::FindFocus();
		if (curFocus == c->videoSlider) {
			if (c->previousFocus) c->previousFocus->SetFocus();
		}
		else {
			c->previousFocus = curFocus;
			c->videoSlider->SetFocus();
		}
	}
};

wxImage get_image(agi::Context *c, bool raw, bool subsonly = false) {
	auto frame = c->videoController->GetFrameN();
	if (subsonly) {
		return GetImageWithAlpha(c->project->VideoProvider()->GetSubtitles(c->project->Timecodes().TimeAtFrame(frame)));
	} else {
		return GetImage(*c->project->VideoProvider()->GetFrame(frame, c->project->Timecodes().TimeAtFrame(frame), raw));
	}
}

struct video_frame_copy final : public validator_video_loaded {
	CMD_NAME("video/frame/copy")
	STR_MENU("Copy image to Clipboard")
	STR_DISP("Copy image to Clipboard")
	STR_HELP("Copy the currently displayed frame to the clipboard")

	void operator()(agi::Context *c) override {
		SetClipboard(wxBitmap(get_image(c, false), 24));
	}
};

struct video_frame_copy_raw final : public validator_video_loaded {
	CMD_NAME("video/frame/copy/raw")
	STR_MENU("Copy image to Clipboard (no subtitles)")
	STR_DISP("Copy image to Clipboard (no subtitles)")
	STR_HELP("Copy the currently displayed frame to the clipboard, without the subtitles")

	void operator()(agi::Context *c) override {
		SetClipboard(wxBitmap(get_image(c, true), 24));
	}
};

struct video_frame_copy_subs final : public validator_video_loaded {
	CMD_NAME("video/frame/copy/subs")
	STR_MENU("Copy image to Clipboard (only subtitles)")
	STR_DISP("Copy image to Clipboard (only subtitles)")
	STR_HELP("Copy the currently displayed subtitles to the clipboard, with transparent background")

	void operator()(agi::Context *c) override {
		SetClipboard(wxBitmap(get_image(c, false, true), 32));
	}
};

struct video_frame_next final : public validator_video_loaded {
	CMD_NAME("video/frame/next")
	STR_MENU("Next Frame")
	STR_DISP("Next Frame")
	STR_HELP("Seek to the next frame")

	void operator()(agi::Context *c) override {
		c->videoController->NextFrame();
	}
};

struct video_frame_next_boundary final : public validator_video_loaded {
	CMD_NAME("video/frame/next/boundary")
	STR_MENU("Next Boundary")
	STR_DISP("Next Boundary")
	STR_HELP("Seek to the next beginning or end of a subtitle")

	void operator()(agi::Context *c) override {
		AssDialogue *active_line = c->selectionController->GetActiveLine();
		if (!active_line) return;

		int target = c->videoController->FrameAtTime(active_line->Start, agi::vfr::START);
		if (target > c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		target = c->videoController->FrameAtTime(active_line->End, agi::vfr::END);
		if (target > c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		c->selectionController->NextLine();
		AssDialogue *new_line = c->selectionController->GetActiveLine();
		if (new_line != active_line)
		c->videoController->JumpToTime(new_line->Start);
	}
};

struct video_frame_next_keyframe final : public validator_video_loaded {
	CMD_NAME("video/frame/next/keyframe")
	STR_MENU("Next Keyframe")
	STR_DISP("Next Keyframe")
	STR_HELP("Seek to the next keyframe")

	void operator()(agi::Context *c) override {
		auto const& kf = c->project->Keyframes();
		auto pos = lower_bound(kf.begin(), kf.end(), c->videoController->GetFrameN() + 1);

		c->videoController->JumpToFrame(pos == kf.end() ? c->project->VideoProvider()->GetFrameCount() - 1 : *pos);
	}
};

struct video_frame_next_large final : public validator_video_loaded {
	CMD_NAME("video/frame/next/large")
	STR_MENU("Fast jump forward")
	STR_DISP("Fast jump forward")
	STR_HELP("Fast jump forward")

	void operator()(agi::Context *c) override {
		c->videoController->JumpToFrame(
			c->videoController->GetFrameN() +
			OPT_GET("Video/Slider/Fast Jump Step")->GetInt());
	}
};

struct video_frame_prev final : public validator_video_loaded {
	CMD_NAME("video/frame/prev")
	STR_MENU("Previous Frame")
	STR_DISP("Previous Frame")
	STR_HELP("Seek to the previous frame")

	void operator()(agi::Context *c) override {
		c->videoController->PrevFrame();
	}
};

struct video_frame_prev_boundary final : public validator_video_loaded {
	CMD_NAME("video/frame/prev/boundary")
	STR_MENU("Previous Boundary")
	STR_DISP("Previous Boundary")
	STR_HELP("Seek to the previous beginning or end of a subtitle")

	void operator()(agi::Context *c) override {
		AssDialogue *active_line = c->selectionController->GetActiveLine();
		if (!active_line) return;

		int target = c->videoController->FrameAtTime(active_line->End, agi::vfr::END);
		if (target < c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		target = c->videoController->FrameAtTime(active_line->Start, agi::vfr::START);
		if (target < c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		c->selectionController->PrevLine();
		AssDialogue *new_line = c->selectionController->GetActiveLine();
		if (new_line != active_line)
			c->videoController->JumpToTime(new_line->End, agi::vfr::END);
	}
};

struct video_frame_prev_keyframe final : public validator_video_loaded {
	CMD_NAME("video/frame/prev/keyframe")
	STR_MENU("Previous Keyframe")
	STR_DISP("Previous Keyframe")
	STR_HELP("Seek to the previous keyframe")

	void operator()(agi::Context *c) override {
		auto const& kf = c->project->Keyframes();
		if (kf.empty()) {
			c->videoController->JumpToFrame(0);
			return;
		}

		auto pos = lower_bound(kf.begin(), kf.end(), c->videoController->GetFrameN());

		if (pos != kf.begin())
			--pos;

		c->videoController->JumpToFrame(*pos);
	}
};

struct video_frame_prev_large final : public validator_video_loaded {
	CMD_NAME("video/frame/prev/large")
	STR_MENU("Fast jump backwards")
	STR_DISP("Fast jump backwards")
	STR_HELP("Fast jump backwards")

	void operator()(agi::Context *c) override {
		c->videoController->JumpToFrame(
			c->videoController->GetFrameN() -
			OPT_GET("Video/Slider/Fast Jump Step")->GetInt());
	}
};

static void save_snapshot(agi::Context *c, bool raw, bool subsonly = false) {
	auto option = OPT_GET("Path/Screenshot")->GetString();
	agi::fs::path basepath;

	auto videoname = c->project->VideoName();
	bool is_dummy = videoname.string().starts_with("?dummy");

	// Is it a path specifier and not an actual fixed path?
	if (option[0] == '?') {
		// If dummy video is loaded, we can't save to the video location
		if (option.starts_with("?video") && is_dummy) {
			// So try the script location instead
			option = "?script";
		}
		// Find out where the ?specifier points to
		basepath = c->path->Decode(option);
		// If where ever that is isn't defined, we can't save there
		if ((basepath == "\\") || (basepath == "/")) {
			// So save to the current user's home dir instead
			basepath = wxGetHomeDir().utf8_str().data();
		}
	}
	// Actual fixed (possibly relative) path, decode it
	else
		basepath = c->path->MakeAbsolute(option, "?user/");

	basepath /= is_dummy ? "dummy" : videoname.stem();

	// Get full path
	int session_shot_count = 1;
	std::string path;
	do {
		path = agi::format("%s_%03d_%d.png", basepath.string(), session_shot_count++, c->videoController->GetFrameN());
	} while (agi::fs::FileExists(path));

	get_image(c, raw, subsonly).SaveFile(to_wx(path), wxBITMAP_TYPE_PNG);
}

struct video_frame_save final : public validator_video_loaded {
	CMD_NAME("video/frame/save")
	STR_MENU("Save PNG snapshot")
	STR_DISP("Save PNG snapshot")
	STR_HELP("Save the currently displayed frame to a PNG file in the video's directory")

	void operator()(agi::Context *c) override {
		save_snapshot(c, false);
	}
};

struct video_frame_save_raw final : public validator_video_loaded {
	CMD_NAME("video/frame/save/raw")
	STR_MENU("Save PNG snapshot (no subtitles)")
	STR_DISP("Save PNG snapshot (no subtitles)")
	STR_HELP("Save the currently displayed frame without the subtitles to a PNG file in the video's directory")

	void operator()(agi::Context *c) override {
		save_snapshot(c, true);
	}
};

struct video_frame_save_subs final : public validator_video_loaded {
	CMD_NAME("video/frame/save/subs")
	STR_MENU("Save PNG snapshot (only subtitles)")
	STR_DISP("Save PNG snapshot (only subtitles)")
	STR_HELP("Save the currently displayed subtitles with transparent background to a PNG file in the video's directory")

	void operator()(agi::Context *c) override {
		save_snapshot(c, false, true);
	}
};

struct video_jump final : public validator_video_loaded {
	CMD_NAME("video/jump")
	CMD_ICON(jumpto_button)
	STR_MENU("&Jump to...")
	STR_DISP("Jump to")
	STR_HELP("Jump to frame or time")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowJumpToDialog(c);
		c->videoSlider->SetFocus();
	}
};

struct video_jump_end final : public validator_video_loaded {
	CMD_NAME("video/jump/end")
	CMD_ICON(video_to_subend)
	STR_MENU("Jump Video to &End")
	STR_DISP("Jump Video to End")
	STR_HELP("Jump the video to the end frame of current subtitle")

	void operator()(agi::Context *c) override {
		if (auto active_line = c->selectionController->GetActiveLine())
			c->videoController->JumpToTime(active_line->End, agi::vfr::END);
	}
};

struct video_jump_start final : public validator_video_loaded {
	CMD_NAME("video/jump/start")
	CMD_ICON(video_to_substart)
	STR_MENU("Jump Video to &Start")
	STR_DISP("Jump Video to Start")
	STR_HELP("Jump the video to the start frame of current subtitle")

	void operator()(agi::Context *c) override {
		if (auto active_line = c->selectionController->GetActiveLine())
			c->videoController->JumpToTime(active_line->Start);
	}
};

struct video_open final : public Command {
	CMD_NAME("video/open")
	CMD_ICON(open_video_menu)
	STR_MENU("&Open Video...")
	STR_DISP("Open Video")
	STR_HELP("Open a video file")

	void operator()(agi::Context *c) override {
		auto formats = MakeWildcard(VideoProviderFactory::GetFileExtensions());
		auto str = from_wx(_("Video Formats")) + " (" + formats + ")|" + formats + "|"
		         + from_wx(_("All Files")) + " (*.*)|*.*";
		auto filename = OpenFileSelector(c, _("Open Video File"), "Path/Last/Video", "", "", str, c->parent, "?script");
		if (!filename.empty())
			c->project->LoadVideo(filename);
	}
};

struct video_open_dummy final : public Command {
	CMD_NAME("video/open/dummy")
	CMD_ICON(use_dummy_video_menu)
	STR_MENU("&Use Dummy Video...")
	STR_DISP("Use Dummy Video")
	STR_HELP("Open a placeholder video clip with solid color")

	void operator()(agi::Context *c) override {
		std::string fn = CreateDummyVideo(c->parent);
		if (!fn.empty())
			c->project->LoadVideo(fn);
	}
};

struct video_opt_autoscroll final : public Command {
	CMD_NAME("video/opt/autoscroll")
	CMD_ICON(toggle_video_autoscroll)
	STR_MENU("Toggle autoscroll of video")
	STR_DISP("Toggle autoscroll of video")
	STR_HELP("Toggle automatically seeking video to the start time of selected lines")
	CMD_TYPE(COMMAND_TOGGLE)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Video/Subtitle Sync")->GetBool();
	}

	void operator()(agi::Context *) override {
		OPT_SET("Video/Subtitle Sync")->SetBool(!OPT_GET("Video/Subtitle Sync")->GetBool());
	}
};

struct video_play final : public validator_video_loaded {
	CMD_NAME("video/play")
	CMD_ICON(button_play)
	STR_MENU("Play")
	STR_DISP("Play")
	STR_HELP("Play the video starting on this position")

	void operator()(agi::Context *c) override {
		c->videoController->Play();
	}
};

struct review_mode final : public validator_video_loaded {
	CMD_NAME("review/mode")
	wxString StrMenu(const agi::Context *) const override { return to_wx("字幕审校模式"); }
	wxString StrDisplay(const agi::Context *) const override { return to_wx("审校模式"); }
	wxString StrHelp() const override { return to_wx("字幕审校模式：进入或退出字幕审校模式"); }
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"3\" fill=\"none\" stroke=\"#2C3E50\" stroke-width=\"2\"/>"
			"<line x1=\"6\" y1=\"7\" x2=\"14\" y2=\"7\" stroke=\"#2C3E50\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
			"<line x1=\"6\" y1=\"11\" x2=\"18\" y2=\"11\" stroke=\"#2C3E50\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
			"<line x1=\"6\" y1=\"15\" x2=\"12\" y2=\"15\" stroke=\"#2C3E50\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
			"<circle cx=\"17\" cy=\"17\" r=\"5\" fill=\"#E67E22\"/>"
			"<path d=\"M15 17 L16.5 18.5 L19.5 15.5\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	bool IsActive(const agi::Context *c) override {
		return c->videoController->IsReviewMode();
	}

	void operator()(agi::Context *c) override {
		c->videoController->SetReviewMode(!c->videoController->IsReviewMode());
	}
};

struct review_play_pause final : public validator_video_loaded {
	CMD_NAME("review/play_pause")
	wxString StrMenu(const agi::Context *) const override { return to_wx("审校模式播放/暂停"); }
	wxString StrDisplay(const agi::Context *) const override { return ""; }
	wxString StrHelp() const override { return to_wx("审校模式播放/暂停：继续或暂停播放，不改变当前选中的字幕行"); }

	void operator()(agi::Context *c) override {
		c->videoController->Play();
	}
};

struct review_preview final : public validator_video_loaded {
	CMD_NAME("review/preview")
	wxString StrMenu(const agi::Context *) const override { return to_wx("预览当前字幕 (±1秒)"); }
	wxString StrDisplay(const agi::Context *) const override { return ""; }
	wxString StrHelp() const override { return to_wx("单句预览 (±1秒)：播放当前选中字幕及其前后各1秒的画面"); }

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<path d=\"M9 6 L18 12 L9 18 Z\" fill=\"#2B78E4\"/>"
			"<path d=\"M3 5 L3 19\" stroke=\"#E67E22\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
			"<path d=\"M21 5 L21 19\" stroke=\"#E67E22\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
			"<path d=\"M3 12 L7 12\" stroke=\"#E67E22\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>"
			"<path d=\"M17 12 L21 12\" stroke=\"#E67E22\" stroke-width=\"1.5\" stroke-linecap=\"round\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	void operator()(agi::Context *c) override {
		if (auto line = c->selectionController->GetActiveLine())
			c->videoController->PlayReviewPreview(line->Start, line->End, c->ass.get());
	}
};

struct review_overlap final : public validator_video_loaded {
	CMD_NAME("review/overlap")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	wxString StrMenu(const agi::Context *) const override { return to_wx("重叠行高亮"); }
	wxString StrDisplay(const agi::Context *) const override { return ""; }
	wxString StrHelp() const override { return to_wx("重叠行高亮：高亮表格中所有时间互相重叠的字幕行"); }

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<rect x=\"2\" y=\"5\" width=\"13\" height=\"5\" rx=\"1.5\" fill=\"#3498DB\"/>"
			"<rect x=\"9\" y=\"14\" width=\"13\" height=\"5\" rx=\"1.5\" fill=\"#3498DB\"/>"
			"<rect x=\"9\" y=\"5\" width=\"6\" height=\"5\" rx=\"1\" fill=\"#E74C3C\"/>"
			"<rect x=\"9\" y=\"14\" width=\"6\" height=\"5\" rx=\"1\" fill=\"#E74C3C\"/>"
			"<line x1=\"12\" y1=\"2\" x2=\"12\" y2=\"22\" stroke=\"#E74C3C\" stroke-width=\"1.8\" stroke-dasharray=\"2,2\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	bool IsActive(const agi::Context *c) override {
		return c->subsGrid && c->subsGrid->IsOverlapHighlightEnabled();
	}

	void operator()(agi::Context *c) override {
		if (c->subsGrid)
			c->subsGrid->SetOverlapHighlight(!c->subsGrid->IsOverlapHighlightEnabled());
	}
};

struct review_replay_after_edit final : public validator_video_loaded {
	CMD_NAME("review/replay_after_edit")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	wxString StrMenu(const agi::Context *) const override { return to_wx("改后重播"); }
	wxString StrDisplay(const agi::Context *) const override { return ""; }
	wxString StrHelp() const override { return to_wx("改后重播：在编辑框修改本行离开后，从该行开头重新播放"); }

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<path d=\"M12 4 A 8 8 0 1 0 20 12\" fill=\"none\" stroke=\"#27AE60\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>"
			"<polygon points=\"12,1 12,7 7,4\" fill=\"#27AE60\"/>"
			"<path d=\"M11 13 L16 8 L18 10 L13 15 L10 16 Z\" fill=\"#E67E22\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Tool/Playback/Review Replay After Edit")->GetBool();
	}

	void operator()(agi::Context *) override {
		bool cur = OPT_GET("Tool/Playback/Review Replay After Edit")->GetBool();
		OPT_SET("Tool/Playback/Review Replay After Edit")->SetBool(!cur);
	}
};

struct review_smooth_preview final : public validator_video_loaded {
	CMD_NAME("review/smooth_preview")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)
	wxString StrMenu(const agi::Context *) const override { return to_wx("流畅预览"); }
	wxString StrDisplay(const agi::Context *) const override { return ""; }
	wxString StrHelp() const override { return to_wx("流畅预览：审校模式对 4K/2K 高清视频以 720P 极速预览，消除卡顿与音画脱节"); }

	wxBitmapBundle Icon(int height, wxLayoutDirection) const override {
		static const char svg[] =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
			"<rect x=\"2\" y=\"3\" width=\"20\" height=\"14\" rx=\"2\" fill=\"none\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<line x1=\"8\" y1=\"21\" x2=\"16\" y2=\"21\" stroke=\"#2980B9\" stroke-width=\"2\" stroke-linecap=\"round\"/>"
			"<line x1=\"12\" y1=\"17\" x2=\"12\" y2=\"21\" stroke=\"#2980B9\" stroke-width=\"2\"/>"
			"<polygon points=\"13,5 7,12 12,12 11,16 17,9 12,9\" fill=\"#F39C12\"/>"
			"</svg>";
		return wxBitmapBundle::FromSVG(svg, wxSize(height, height));
	}

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Tool/Playback/Review Smooth Preview")->GetBool();
	}

	void operator()(agi::Context *c) override {
		bool cur = OPT_GET("Tool/Playback/Review Smooth Preview")->GetBool();
		OPT_SET("Tool/Playback/Review Smooth Preview")->SetBool(!cur);
		c->videoController->UpdateSmoothPreview(true);
	}
};

struct video_play_line final : public validator_video_loaded {
	CMD_NAME("video/play/line")
	CMD_ICON(button_playline)
	STR_MENU("Play line")
	STR_DISP("Play line")
	STR_HELP("Play the video for the current line")

	void operator()(agi::Context *c) override {
		c->videoController->PlayLine();
	}
};

struct video_show_overscan final : public validator_video_loaded {
	CMD_NAME("video/show_overscan")
	STR_MENU("Show &Overscan Mask")
	STR_DISP("Show Overscan Mask")
	STR_HELP("Show a mask over the video, indicating areas that might get cropped off by overscan on televisions")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Video/Overscan Mask")->GetBool();
	}

	void operator()(agi::Context *c) override {
		OPT_SET("Video/Overscan Mask")->SetBool(!OPT_GET("Video/Overscan Mask")->GetBool());
		c->videoDisplay->Render();
	}
};

struct video_reset_pan final : public validator_video_loaded {
       CMD_NAME("video/reset_pan")
       STR_MENU("Reset Video &Pan")
       STR_DISP("Reset Video Pan")
       STR_HELP("Reset the video's position in the video display")

       void operator()(agi::Context *c) override {
		   c->videoDisplay->ResetContentZoom();
       }
};

class video_zoom_100: public validator_video_attached {
public:
	CMD_NAME("video/zoom/100")
	STR_MENU("&100%")
	STR_DISP("100%")
	STR_HELP("Set zoom to 100%")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoDisplay->GetWindowZoom() == 1.;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetWindowZoom(1.);
	}
};

class video_stop: public validator_video_loaded {
public:
	CMD_NAME("video/stop")
	CMD_ICON(button_pause)
	STR_MENU("Stop video")
	STR_DISP("Stop video")
	STR_HELP("Stop video playback")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
	}
};

class video_zoom_200: public validator_video_attached {
public:
	CMD_NAME("video/zoom/200")
	STR_MENU("&200%")
	STR_DISP("200%")
	STR_HELP("Set zoom to 200%")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoDisplay->GetWindowZoom() == 2.;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetWindowZoom(2.);
	}
};

class video_zoom_50: public validator_video_attached {
public:
	CMD_NAME("video/zoom/50")
	STR_MENU("&50%")
	STR_DISP("50%")
	STR_HELP("Set zoom to 50%")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoDisplay->GetWindowZoom() == .5;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetWindowZoom(.5);
	}
};

struct video_zoom_in final : public validator_video_attached {
	CMD_NAME("video/zoom/in")
	CMD_ICON(zoom_in_button)
	STR_MENU("Zoom In")
	STR_DISP("Zoom In")
	STR_HELP("Zoom video in")

	void operator()(agi::Context *c) override {
		c->videoDisplay->SetWindowZoom(c->videoDisplay->GetWindowZoom() + .125);
	}
};

struct video_zoom_out final : public validator_video_attached {
	CMD_NAME("video/zoom/out")
	CMD_ICON(zoom_out_button)
	STR_MENU("Zoom Out")
	STR_DISP("Zoom Out")
	STR_HELP("Zoom video out")

	void operator()(agi::Context *c) override {
		c->videoDisplay->SetWindowZoom(c->videoDisplay->GetWindowZoom() - .125);
	}
};
}

namespace cmd {
	void init_video() {
		reg(std::make_unique<video_aspect_cinematic>());
		reg(std::make_unique<video_aspect_custom>());
		reg(std::make_unique<video_aspect_default>());
		reg(std::make_unique<video_aspect_full>());
		reg(std::make_unique<video_aspect_wide>());
		reg(std::make_unique<video_close>());
		reg(std::make_unique<video_copy_coordinates>());
		reg(std::make_unique<video_cycle_subtitles_provider>());
		reg(std::make_unique<video_detach>());
		reg(std::make_unique<video_details>());
		reg(std::make_unique<video_motion_track>());
		reg(std::make_unique<video_shuffler>());
		reg(std::make_unique<video_focus_seek>());
		reg(std::make_unique<video_frame_copy>());
		reg(std::make_unique<video_frame_copy_raw>());
		reg(std::make_unique<video_frame_copy_subs>());
		reg(std::make_unique<video_frame_next>());
		reg(std::make_unique<video_frame_next_boundary>());
		reg(std::make_unique<video_frame_next_keyframe>());
		reg(std::make_unique<video_frame_next_large>());
		reg(std::make_unique<video_frame_prev>());
		reg(std::make_unique<video_frame_prev_boundary>());
		reg(std::make_unique<video_frame_prev_keyframe>());
		reg(std::make_unique<video_frame_prev_large>());
		reg(std::make_unique<video_frame_save>());
		reg(std::make_unique<video_frame_save_raw>());
		reg(std::make_unique<video_frame_save_subs>());
		reg(std::make_unique<video_jump>());
		reg(std::make_unique<video_jump_end>());
		reg(std::make_unique<video_jump_start>());
		reg(std::make_unique<video_open>());
		reg(std::make_unique<video_open_dummy>());
		reg(std::make_unique<video_opt_autoscroll>());
		reg(std::make_unique<video_play>());
		reg(std::make_unique<review_mode>());
		reg(std::make_unique<review_play_pause>());
		reg(std::make_unique<review_preview>());
		reg(std::make_unique<review_overlap>());
		reg(std::make_unique<review_replay_after_edit>());
		reg(std::make_unique<review_smooth_preview>());
		reg(std::make_unique<video_play_line>());
		reg(std::make_unique<video_show_overscan>());
		reg(std::make_unique<video_reset_pan>());
		reg(std::make_unique<video_stop>());
		reg(std::make_unique<video_zoom_100>());
		reg(std::make_unique<video_zoom_200>());
		reg(std::make_unique<video_zoom_50>());
		reg(std::make_unique<video_zoom_in>());
		reg(std::make_unique<video_zoom_out>());
	}
}
