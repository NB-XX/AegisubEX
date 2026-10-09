// Copyright (c) 2005-2007, Rodrigo Braz Monteiro
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

#include "video_controller.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_controller.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "time_range.h"
#include "async_video_provider.h"
#include "utils.h"

#include <libaegisub/ass/time.h>

#include <algorithm>
#include <wx/log.h>

VideoController::VideoController(agi::Context *c)
: context(c)
, playAudioOnStep(OPT_GET("Audio/Plays When Stepping Video"))
, connections(agi::signal::make_vector({
	context->ass->AddCommitListener(&VideoController::OnSubtitlesCommit, this),
	context->project->AddVideoProviderListener(&VideoController::OnNewVideoProvider, this),
	context->selectionController->AddActiveLineListener(&VideoController::OnActiveLineChanged, this),
}))
{
	Bind(EVT_VIDEO_ERROR, &VideoController::OnVideoError, this);
	Bind(EVT_SUBTITLES_ERROR, &VideoController::OnSubtitlesError, this);
	playback.Bind(wxEVT_TIMER, &VideoController::OnPlayTimer, this);
	play_speed = std::clamp<int64_t>(OPT_GET("Tool/Playback/Play Speed")->GetInt(), 25, 400) / 100.0;
}

void VideoController::OnNewVideoProvider(AsyncVideoProvider *new_provider) {
	Stop();
	provider = new_provider;
	color_matrix = std::nullopt;
}

void VideoController::OnSubtitlesCommit(int type, const AssDialogue *changed) {
	// Remember when and where a line was modified during a review-mode focus
	// pause, so the "replay after edit" option can send playback back there.
	// Only content edits count: text, timing and per-line metadata, not adds,
	// deletes or file-wide commits.
	if (review_mode && focus_paused && changed && (type & AssFile::COMMIT_DIAG_FULL))
		review_edit_time = changed->Start;

	if (!provider) return;

	if ((type & AssFile::COMMIT_SCRIPTINFO) || type == AssFile::COMMIT_NEW) {
		auto new_matrix = context->ass->GetYCbCrMatrix();
		if (new_matrix != color_matrix) {
			color_matrix = new_matrix;
			provider->SetColorSpace(new_matrix);
		}
	}

	if (!changed)
		provider->LoadSubtitles(context->ass.get());
	else
		provider->UpdateSubtitles(changed);
}

void VideoController::OnActiveLineChanged(AssDialogue *line) {
	if (!review_mode && line && provider && OPT_GET("Video/Subtitle Sync")->GetBool()) {
		Stop();
		JumpToTime(line->Start);
	}
}

void VideoController::RequestFrame() {
	context->ass->Properties.video_position = frame_n;
	provider->RequestFrame(frame_n, TimeAtFrame(frame_n));
}

void VideoController::JumpToFrame(int n) {
	if (!provider) return;

	bool was_playing = IsPlaying();
	if (was_playing)
		Stop();

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	RequestFrame();
	Seek(frame_n);

	if (was_playing)
		Play();
}

void VideoController::JumpToTime(int ms, agi::vfr::Time end) {
	JumpToFrame(FrameAtTime(ms, end));
}

void VideoController::NextFrame() {
	if (!provider || IsPlaying() || frame_n == provider->GetFrameCount())
		return;

	JumpToFrame(frame_n + 1);
	if (playAudioOnStep->GetBool())
		context->audioController->PlayRange(TimeRange(TimeAtFrame(frame_n - 1), TimeAtFrame(frame_n)));
}

void VideoController::PrevFrame() {
	if (!provider || IsPlaying() || frame_n == 0)
		return;

	JumpToFrame(frame_n - 1);
	if (playAudioOnStep->GetBool())
		context->audioController->PlayRange(TimeRange(TimeAtFrame(frame_n), TimeAtFrame(frame_n + 1)));
}

void VideoController::Play() {
	if (IsPlaying()) {
		Stop();
		return;
	}

	if (!provider) return;

	start_ms = TimeAtFrame(frame_n);
	end_frame = provider->GetFrameCount() - 1;

	// Sound follows the video clock: the player time-stretches the audio so
	// speed changes keep the normal pitch. Players that can offer neither
	// stretching nor resampling would let audio drift against the sped-up
	// video, so those go silent instead.
	if (context->audioController->SetSpeed(play_speed) > 0)
		context->audioController->PlayToEnd(start_ms);
	else
		context->audioController->Stop();

	playback_start_time = std::chrono::steady_clock::now();
	playback.Start(10);
	PlaybackChange(true);
}

void VideoController::SetReviewMode(bool active) {
	if (review_mode == active) return;
	review_mode = active;
	if (!active) {
		focus_paused = false;
		review_edit_time = -1;
		if (review_preview)
			FinishReviewPreview(false);
	}
	ReviewModeChange(active);
}

void VideoController::OnEditAreaFocus(bool in) {
	if (!review_mode) {
		focus_paused = false;
		review_edit_time = -1;
		return;
	}
	if (in) {
		// Focus landed in the editing areas (text box, style row, timing
		// fields or audio display): pause so the reviewer can work, and
		// remember that this pause was automatic.
		if (IsPlaying()) {
			Stop();
			focus_paused = true;
			review_edit_time = -1;
		}
	}
	else if (focus_paused) {
		// Focus left the editing areas (typically back to the subtitle
		// grid): resume from the frame the automatic pause stopped on, or
		// — with "replay after edit" on and the line actually modified
		// during the pause — restart from the edited line's own start so
		// the reviewer immediately re-watches the change.
		int replay_time = review_edit_time;
		focus_paused = false;
		review_edit_time = -1;
		if (!IsPlaying()) {
			if (replay_time >= 0 && OPT_GET("Tool/Playback/Review Replay After Edit")->GetBool())
				JumpToTime(replay_time);
			Play();
		}
	}
}

void VideoController::PlayReviewPreview(int start, int end, AssFile const *subtitles) {
	if (!provider || !subtitles || end <= start) return;
	bool resume_after_preview = review_preview ? review_was_playing : IsPlaying();
	int saved_frame = review_preview ? review_saved_frame : frame_n;
	if (review_preview) FinishReviewPreview(false);
	review_was_playing = resume_after_preview;
	review_saved_frame = saved_frame;
	if (IsPlaying()) {
		playback.Stop();
		context->audioController->Stop();
		PlaybackChange(false);
	}
	review_preview = true;
	provider->LoadSubtitles(subtitles);
	int first = mid(0, FrameAtTime(std::max(0, start - 1000), agi::vfr::START), provider->GetFrameCount() - 1);
	int last_exclusive = mid(first + 1, FrameAtTime(end + 1000, agi::vfr::END) + 1, provider->GetFrameCount());
	frame_n = first;
	RequestFrame();
	Seek(frame_n);
	start_ms = TimeAtFrame(frame_n);
	end_frame = last_exclusive;
	if (context->audioController->SetSpeed(play_speed) > 0)
		context->audioController->PlayRange(TimeRange(start_ms, TimeAtFrame(std::max(first, last_exclusive - 1), agi::vfr::END)));
	else
		context->audioController->Stop();
	playback_start_time = std::chrono::steady_clock::now();
	playback.Start(10);
	PlaybackChange(true);
}

void VideoController::FinishReviewPreview(bool resume_playback) {
	if (!review_preview) return;
	playback.Stop();
	context->audioController->Stop();
	review_preview = false;
	provider->LoadSubtitles(context->ass.get());
	frame_n = mid(0, review_saved_frame, provider->GetFrameCount() - 1);
	RequestFrame();
	Seek(frame_n);
	PlaybackChange(false);
	if (resume_playback && review_was_playing)
		Play();
}

void VideoController::PlayLine() {
	Stop();

	AssDialogue *curline = context->selectionController->GetActiveLine();
	if (!curline) return;

	if (context->audioController->SetSpeed(play_speed) > 0)
		context->audioController->PlayRange(TimeRange(curline->Start, curline->End));

	// Round-trip conversion to convert start to exact
	int startFrame = FrameAtTime(context->selectionController->GetActiveLine()->Start, agi::vfr::START);
	start_ms = TimeAtFrame(startFrame);
	end_frame = FrameAtTime(context->selectionController->GetActiveLine()->End, agi::vfr::END) + 1;

	JumpToFrame(startFrame);

	playback_start_time = std::chrono::steady_clock::now();
	playback.Start(10);
	PlaybackChange(true);
}

void VideoController::Stop() {
	if (review_preview) {
		FinishReviewPreview(false);
		return;
	}
	if (IsPlaying()) {
		playback.Stop();
		context->audioController->Stop();
		PlaybackChange(false);
	}
}

void VideoController::OnPlayTimer(wxTimerEvent &) {
	using namespace std::chrono;
	// Virtual timeline: wall-clock elapsed scaled by the playback speed, so a
	// 10 ms timer tick advances 10/20/40 ms of video on 1/2/4x etc.
	int64_t elapsed = duration_cast<milliseconds>(steady_clock::now() - playback_start_time).count();
	int64_t virtual_ms = start_ms + static_cast<int64_t>(elapsed * play_speed);
	int next_frame = FrameAtTime(static_cast<int>(std::min<int64_t>(virtual_ms, 2000000000LL)));
	if (next_frame == frame_n) return;

	if (next_frame >= end_frame) {
		if (review_preview)
			FinishReviewPreview(true);
		else
			Stop();
	}
	else {
		frame_n = next_frame;
		RequestFrame();
		Seek(frame_n);
	}
}

void VideoController::SetSpeed(double speed) {
	if (play_speed == speed) return;
	bool was_playing = IsPlaying();
	play_speed = speed;
	OPT_SET("Tool/Playback/Play Speed")->SetInt(static_cast<int>(speed * 100));

	// Sync the audio player's rate right away, even while stopped: previews
	// started from the audio box follow the player's own speed, and should
	// use the newly picked rate instead of the one left over from the last
	// video playback.
	bool audio_follows = context->audioController->SetSpeed(play_speed) > 0;
	if (!was_playing || !provider) return;

	// Re-anchor the clock at the currently displayed frame so the new speed
	// takes effect without a jump, then keep audio in step: it follows the
	// rate when the player can deliver it, and stays silent when it cannot.
	start_ms = TimeAtFrame(frame_n);
	playback_start_time = std::chrono::steady_clock::now();
	if (audio_follows)
		context->audioController->PlayToEnd(start_ms);
	else
		context->audioController->Stop();
}

double VideoController::GetARFromType(AspectRatio type) const {
	switch (type) {
		case AspectRatio::Default:    return (double)provider->GetWidth()/provider->GetHeight();
		case AspectRatio::Fullscreen: return 4.0/3.0;
		case AspectRatio::Widescreen: return 16.0/9.0;
		case AspectRatio::Cinematic:  return 2.35;
        default: throw agi::InternalError("Bad AR type");
	}
}

void VideoController::SetAspectRatio(double value) {
	ar_type = AspectRatio::Custom;
	ar_value = mid(.5, value, 5.);
	context->ass->Properties.ar_mode = (int)ar_type;
	context->ass->Properties.ar_value = ar_value;
	ARChange(ar_type, ar_value);
}

void VideoController::SetAspectRatio(AspectRatio type) {
	ar_value = mid(.5, GetARFromType(type), 5.);
	ar_type = type;
	context->ass->Properties.ar_mode = (int)ar_type;
	context->ass->Properties.ar_value = ar_value;
	ARChange(ar_type, ar_value);
}

int VideoController::TimeAtFrame(int frame, agi::vfr::Time type) const {
	return context->project->Timecodes().TimeAtFrame(frame, type);
}

int VideoController::FrameAtTime(int time, agi::vfr::Time type) const {
	return context->project->Timecodes().FrameAtTime(time, type);
}

void VideoController::OnVideoError(VideoProviderErrorEvent const& err) {
	wxLogError(
		fmt_tl("Failed seeking video. The video file may be corrupt or incomplete.\nError message reported: %s",
		err.GetMessage()));
}

void VideoController::OnSubtitlesError(SubtitlesProviderErrorEvent const& err) {
	wxLogError(
		fmt_tl("Failed rendering subtitles.\nError message reported: %s",
		err.GetMessage()));
}
