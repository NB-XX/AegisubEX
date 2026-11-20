// Copyright (c) 2008-2009, Karl Blomster
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

/// @file video_provider_ffmpegsource.cpp
/// @brief FFmpegSource2-based video provider
/// @ingroup video_input ffms
///

#include "video_provider_manager.h"
#include "include/aegisub/video_provider.h"

#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"

#include "options.h"
#include "utils.h"
#include "video_frame.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string_view>

using agi::ycbcr_matrix;
using agi::ycbcr_range;
namespace ycbcr = agi::ycbcr;

namespace {
std::mutex s_ffms_lifecycle_mutex;

void ApplyVideoOutputFormat(FFMS_VideoSource *video_source, int width, int height, bool smooth_preview, int &out_width, int &out_height, FFMS_ErrorInfo &err_info) {
	if (!video_source) return;

	if (smooth_preview && height > 720) {
		out_height = 720;
		out_width = (static_cast<int64_t>(width) * 720 / height + 1) & ~1;
	} else {
		out_width = width;
		out_height = height;
	}

	const int TargetFormat[] = { FFMS_GetPixFmt("bgra"), -1 };
	int resizer = smooth_preview ? FFMS_RESIZER_FAST_BILINEAR : FFMS_RESIZER_BICUBIC;
	if (FFMS_SetOutputFormatV2(video_source, TargetFormat, out_width, out_height, resizer, &err_info)) {
		out_width = width;
		out_height = height;
		FFMS_SetOutputFormatV2(video_source, TargetFormat, width, height, FFMS_RESIZER_BICUBIC, &err_info);
	}
}

void ApplyFlipAndRotation(const FFMS_Frame *frame, const FFMS_VideoProperties *video_info, int cur_w, int cur_h, VideoFrame &out) {
	out.data.assign(frame->Data[0], frame->Data[0] + frame->Linesize[0] * cur_h);
	out.flipped = false;
	out.width = cur_w;
	out.height = cur_h;
	out.pitch = frame->Linesize[0];

	// Handle flip
	if (video_info->Flip > 0)
		for (int x = 0; x < cur_h; ++x)
			for (int y = 0; y < cur_w / 2; ++y)
				for (int ch = 0; ch < 4; ++ch)
					std::swap(out.data[frame->Linesize[0] * x + 4 * y + ch], out.data[frame->Linesize[0] * x + 4 * (cur_w - 1 - y) + ch]);

	else if (video_info->Flip < 0)
		for (int x = 0; x < cur_h / 2; ++x)
			for (int y = 0; y < cur_w; ++y)
				for (int ch = 0; ch < 4; ++ch)
					std::swap(out.data[frame->Linesize[0] * x + 4 * y + ch], out.data[frame->Linesize[0] * (cur_h - 1 - x) + 4 * y + ch]);

	// Handle rotation
	if (video_info->Rotation % 360 == 180 || video_info->Rotation % 360 == -180) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(cur_w * cur_h * 4);
		for (int x = 0; x < cur_h; ++x)
			for (int y = 0; y < cur_w; ++y)
				for (int ch = 0; ch < 4; ++ch)
					out.data[4 * (cur_w * x + y) + ch] = data[frame->Linesize[0] * (cur_h - 1 - x) + 4 * (cur_w - 1 - y) + ch];
		out.pitch = 4 * cur_w;
	}
	else if (video_info->Rotation % 180 == 90 || video_info->Rotation % 360 == -270) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(cur_w * cur_h * 4);
		for (int x = 0; x < cur_w; ++x)
			for (int y = 0; y < cur_h; ++y)
				for (int ch = 0; ch < 4; ++ch)
					out.data[4 * (cur_h * x + y) + ch] = data[frame->Linesize[0] * y + 4 * (cur_w - 1 - x) + ch];
		out.width = cur_h;
		out.height = cur_w;
		out.pitch = 4 * cur_h;
	}
	else if (video_info->Rotation % 180 == 270 || video_info->Rotation % 360 == -90) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(cur_w * cur_h * 4);
		for (int x = 0; x < cur_w; ++x)
			for (int y = 0; y < cur_h; ++y)
				for (int ch = 0; ch < 4; ++ch)
					out.data[4 * (cur_h * x + y) + ch] = data[frame->Linesize[0] * (cur_h - 1 - y) + 4 * x + ch];
		out.width = cur_h;
		out.height = cur_w;
		out.pitch = 4 * cur_h;
	}
}

void PopulateTimecodesAndKeyFrames(FFMS_VideoSource *video_source, const FFMS_VideoProperties *video_info, std::vector<int> &keyframes_list, agi::vfr::Framerate &timecodes) {
	FFMS_Track *FrameData = FFMS_GetTrackFromVideo(video_source);
	if (FrameData == nullptr)
		throw VideoOpenError("failed to get frame data");
	const FFMS_TrackTimeBase *TimeBase = FFMS_GetTimeBase(FrameData);
	if (TimeBase == nullptr)
		throw VideoOpenError("failed to get track time base");

	std::vector<int> TimecodesVector;
	for (int CurFrameNum = 0; CurFrameNum < video_info->NumFrames; CurFrameNum++) {
		const FFMS_FrameInfo *CurFrameData = FFMS_GetFrameInfo(FrameData, CurFrameNum);
		if (!CurFrameData)
			throw VideoOpenError("Couldn't get info about frame " + std::to_string(CurFrameNum));

		if (CurFrameData->KeyFrame)
			keyframes_list.push_back(CurFrameNum);

		// FFMS2 timestamps are rational wall-clock milliseconds. Round to the
		// nearest millisecond instead of truncating: at 60 fps this avoids
		// biasing every frame boundary early by up to 1 ms.
		const long double timestamp = static_cast<long double>(CurFrameData->PTS) * TimeBase->Num / TimeBase->Den;
		if (timestamp < std::numeric_limits<int>::min() || timestamp > std::numeric_limits<int>::max())
			throw VideoOpenError("Video timestamp is outside the supported range");
		int Timestamp = static_cast<int>(std::llround(timestamp));
		TimecodesVector.push_back(Timestamp);
	}
	if (TimecodesVector.size() < 2)
		timecodes = 25.0;
	else
		timecodes = agi::vfr::Framerate(TimecodesVector);
}
bool IsHDRFrame(const FFMS_Frame *frame) {
	switch (frame->TransferCharateristics) {
		case 16:	// SMPTE ST 2084 (PQ)
		case 18:	// ARIB_STD_B67 (HLG)
			return true;
		default:
			break;
	}

	switch (frame->ColorPrimaries) {
		case 9:		// BT.2020
		case 11:	// SMPTE ST 431-2 (2011) (DCI P3)
		case 12:	// SMPTE ST 432-1 (2010) (Display P3)
			return true;
		default:
			break;
	}

	if (frame->DolbyVisionRPUSize) {
		return true;
	}

	return false;
}

/// @class FFmpegSourceVideoProvider
/// @brief Implements video loading through the FFMS library.
class FFmpegSourceVideoProvider final : public VideoProvider, FFmpegSourceProvider {
	/// video source object
	agi::scoped_holder<FFMS_VideoSource*, void (FFMS_CC*)(FFMS_VideoSource*)> VideoSource;
	const FFMS_VideoProperties *VideoInfo = nullptr; ///< video properties

	int Width = -1;                 ///< width in pixels
	int Height = -1;                ///< height in pixels
	ycbcr::header_colorspace VideoColorSpace = ycbcr::header_colorspace::unspecified();
	double DAR;                     ///< display aspect ratio
	std::vector<int> KeyFramesList; ///< list of keyframes
	agi::vfr::Framerate Timecodes;  ///< vfr object
	ycbcr::header_colorspace ColorSpace;   ///< color space currently used to convert the video to RGB

	bool HDR = false;

	char FFMSErrMsg[1024];          ///< FFMS error message
	FFMS_ErrorInfo ErrInfo;         ///< FFMS error codes/messages
	bool has_audio = false;

	int OutputWidth = 0;
	int OutputHeight = 0;
	bool smooth_preview = false;

	void ApplyOutputFormat();
	void LoadVideo(agi::fs::path const& filename, ycbcr::Header colormatrix);

public:
	void SetSmoothPreview(bool enabled) override {
		if (smooth_preview == enabled) return;
		smooth_preview = enabled;
		ApplyOutputFormat();
	}
	bool IsSmoothPreview() const override { return smooth_preview; }
	FFmpegSourceVideoProvider(agi::fs::path const& filename, ycbcr::Header colormatrix, agi::BackgroundRunner *br);

	void GetFrame(int n, VideoFrame &out) override;

	void SetColorSpace(ycbcr::Header matrix) override {
		auto [CM, CR] = VideoColorSpace;
		matrix.override_colorspace(CM, CR, Width, Height);

		if (FFMS_SetInputFormatV(VideoSource, static_cast<int>(CM), static_cast<int>(CR), FFMS_GetPixFmt(""), &ErrInfo))
			throw VideoOpenError(std::string("Failed to set input format: ") + ErrInfo.Buffer);

		ColorSpace = {CM, CR};
	}

	int GetFrameCount() const override             { return VideoInfo->NumFrames; }

	int GetWidth() const override  { return (VideoInfo->Rotation % 180 == 90 || VideoInfo->Rotation % 180 == -90) ? Height : Width; }
	int GetHeight() const override { return (VideoInfo->Rotation % 180 == 90 || VideoInfo->Rotation % 180 == -90) ? Width : Height; }
	double GetDAR() const override { return (VideoInfo->Rotation % 180 == 90 || VideoInfo->Rotation % 180 == -90) ? 1 / DAR : DAR; }

	agi::vfr::Framerate GetFPS() const override    { return Timecodes; }
	ycbcr::header_colorspace GetColorSpace() const override     { return ColorSpace; }
	ycbcr::header_colorspace GetRealColorSpace() const override { return VideoColorSpace; }
	bool IsHDRorWCG() const override { return HDR; }
	std::vector<int> GetKeyFrames() const override { return KeyFramesList; };
	std::string GetDecoderName() const override    { return "FFmpegSource"; }
	bool WantsCaching() const override             { return true; }
	bool HasAudio() const override                 { return has_audio; }
};

FFmpegSourceVideoProvider::FFmpegSourceVideoProvider(agi::fs::path const& filename, ycbcr::Header colormatrix, agi::BackgroundRunner *br) try
: FFmpegSourceProvider(br)
, VideoSource(nullptr, FFMS_DestroyVideoSource)
{
	ErrInfo.Buffer		= FFMSErrMsg;
	ErrInfo.BufferSize	= sizeof(FFMSErrMsg);
	ErrInfo.ErrorType	= FFMS_ERROR_SUCCESS;
	ErrInfo.SubType		= FFMS_ERROR_SUCCESS;

	SetLogLevel();

	LoadVideo(filename, colormatrix);
}
catch (agi::EnvironmentError const& err) {
	throw VideoOpenError(err.GetMessage());
}

void FFmpegSourceVideoProvider::LoadVideo(agi::fs::path const& filename, ycbcr::Header colormatrix) {
	FFMS_Indexer *Indexer = FFMS_CreateIndexer(filename.string().c_str(), &ErrInfo);
	if (!Indexer) {
		if (ErrInfo.SubType == FFMS_ERROR_FILE_READ)
			throw agi::fs::FileNotFound(std::string(ErrInfo.Buffer));
		else
			throw VideoNotSupported(ErrInfo.Buffer);
	}

	std::map<int, std::string> TrackList = GetTracksOfType(Indexer, FFMS_TYPE_VIDEO);
	if (TrackList.size() <= 0)
		throw VideoNotSupported("no video tracks found");

	int TrackNumber = -1;
	if (TrackList.size() > 1) {
		auto Selection = AskForTrackSelection(TrackList, FFMS_TYPE_VIDEO);
		if (Selection == TrackSelection::None)
			throw agi::UserCancelException("video loading cancelled by user");
		TrackNumber = static_cast<int>(Selection);
	}

	// generate a name for the cache file
	auto CacheName = GetCacheFilename(filename);

	// try to read index
	agi::scoped_holder<FFMS_Index*, void (FFMS_CC*)(FFMS_Index*)>
		Index(FFMS_ReadIndex(CacheName.string().c_str(), &ErrInfo), FFMS_DestroyIndex);

	if (Index && FFMS_IndexBelongsToFile(Index, filename.string().c_str(), &ErrInfo))
		Index = nullptr;

	// time to examine the index and check if the track we want is indexed
	// technically this isn't really needed since all video tracks should always be indexed,
	// but a bit of sanity checking never hurt anyone
	if (Index && TrackNumber >= 0) {
		FFMS_Track *TempTrackData = FFMS_GetTrackFromIndex(Index, TrackNumber);
		if (FFMS_GetNumFrames(TempTrackData) <= 0)
			Index = nullptr;
	}

	// moment of truth
	if (!Index) {
		auto TrackMask = TrackSelection::None;
		if (OPT_GET("Provider/FFmpegSource/Index All Tracks")->GetBool() || OPT_GET("Video/Open Audio")->GetBool())
			TrackMask = TrackSelection::All;
		Index = DoIndexing(Indexer, CacheName, TrackMask, GetErrorHandlingMode());
	}
	else {
		FFMS_CancelIndexing(Indexer);
	}

	// update access time of index file so it won't get cleaned away
	agi::fs::Touch(CacheName);

	// we have now read the index and may proceed with cleaning the index cache
	CleanCache();

	// track number still not set?
	if (TrackNumber < 0) {
		// just grab the first track
		TrackNumber = FFMS_GetFirstIndexedTrackOfType(Index, FFMS_TYPE_VIDEO, &ErrInfo);
		if (TrackNumber < 0)
			throw VideoNotSupported(std::string("Couldn't find any video tracks: ") + ErrInfo.Buffer);
	}

	// Check if there's an audio track
	has_audio = FFMS_GetFirstTrackOfType(Index, FFMS_TYPE_AUDIO, nullptr) != -1;

	// set thread count
	int Threads = OPT_GET("Provider/Video/FFmpegSource/Decoding Threads")->GetInt();

	// set seekmode
	// TODO: give this its own option?
	int SeekMode;
	if (OPT_GET("Provider/Video/FFmpegSource/Unsafe Seeking")->GetBool())
		SeekMode = FFMS_SEEK_UNSAFE;
	else
		SeekMode = FFMS_SEEK_NORMAL;

	VideoSource = FFMS_CreateVideoSource(filename.string().c_str(), TrackNumber, Index, Threads, SeekMode, &ErrInfo);
	if (!VideoSource)
		throw VideoOpenError(std::string("Failed to open video track: ") + ErrInfo.Buffer);

	// load video properties
	VideoInfo = FFMS_GetVideoProperties(VideoSource);

	const FFMS_Frame *TempFrame = FFMS_GetFrame(VideoSource, 0, &ErrInfo);
	if (!TempFrame)
		throw VideoOpenError(std::string("Failed to decode first frame: ") + ErrInfo.Buffer);

	Width  = TempFrame->EncodedWidth;
	Height = TempFrame->EncodedHeight;
	OutputWidth = Width;
	OutputHeight = Height;
	if (VideoInfo->SARDen > 0 && VideoInfo->SARNum > 0)
		DAR = double(Width) * VideoInfo->SARNum / ((double)Height * VideoInfo->SARDen);
	else
		DAR = double(Width) / Height;

	VideoColorSpace = {static_cast<ycbcr_matrix>(TempFrame->ColorSpace), static_cast<ycbcr_range>(TempFrame->ColorRange)};
	HDR = IsHDRFrame(TempFrame);

	SetColorSpace(colormatrix);

	ApplyOutputFormat();

	PopulateTimecodesAndKeyFrames(VideoSource, VideoInfo, KeyFramesList, Timecodes);
}

void FFmpegSourceVideoProvider::ApplyOutputFormat() {
	ApplyVideoOutputFormat(VideoSource, Width, Height, smooth_preview, OutputWidth, OutputHeight, ErrInfo);
}

void FFmpegSourceVideoProvider::GetFrame(int n, VideoFrame &out) {
	n = mid(0, n, GetFrameCount() - 1);

	auto frame = FFMS_GetFrame(VideoSource, n, &ErrInfo);
	if (!frame)
		throw VideoDecodeError(std::string("Failed to retrieve frame: ") + ErrInfo.Buffer);

	int cur_w = OutputWidth > 0 ? OutputWidth : Width;
	int cur_h = OutputHeight > 0 ? OutputHeight : Height;
	ApplyFlipAndRotation(frame, VideoInfo, cur_w, cur_h, out);
}

class FFmpegSourceWorkerProvider final : public VideoProvider {
	agi::scoped_holder<FFMS_VideoSource*, void (FFMS_CC*)(FFMS_VideoSource*)> VideoSource;
	const FFMS_VideoProperties *VideoInfo = nullptr;

	int Width = -1;
	int Height = -1;
	ycbcr::header_colorspace VideoColorSpace = ycbcr::header_colorspace::unspecified();
	double DAR = 1.0;
	std::vector<int> KeyFramesList;
	agi::vfr::Framerate Timecodes;
	ycbcr::header_colorspace ColorSpace;
	bool HDR = false;

	char FFMSErrMsg[1024] = {};
	FFMS_ErrorInfo ErrInfo{};
	bool has_audio = false;

	int OutputWidth = 0;
	int OutputHeight = 0;
	bool smooth_preview = false;

	void ApplyColorSpaceLocked(ycbcr::Header matrix) {
		auto [CM, CR] = VideoColorSpace;
		matrix.override_colorspace(CM, CR, Width, Height);

		if (FFMS_SetInputFormatV(VideoSource, static_cast<int>(CM), static_cast<int>(CR), FFMS_GetPixFmt(""), &ErrInfo))
			throw VideoOpenError(std::string("Failed to set input format: ") + ErrInfo.Buffer);

		ColorSpace = {CM, CR};
	}

	void ApplyOutputFormatLocked() {
		ApplyVideoOutputFormat(VideoSource, Width, Height, smooth_preview, OutputWidth, OutputHeight, ErrInfo);
	}

public:
	FFmpegSourceWorkerProvider(agi::fs::path const& filename, ycbcr::Header colormatrix, WorkerVideoOptions const& opts)
	: VideoSource(nullptr, FFMS_DestroyVideoSource)
	{
		ErrInfo.Buffer = FFMSErrMsg;
		ErrInfo.BufferSize = sizeof(FFMSErrMsg);
		ErrInfo.ErrorType = FFMS_ERROR_SUCCESS;
		ErrInfo.SubType = FFMS_ERROR_SUCCESS;

		std::lock_guard<std::mutex> lock(s_ffms_lifecycle_mutex);
		FFMS_Init(0, 0);

		agi::scoped_holder<FFMS_Index*, void (FFMS_CC*)(FFMS_Index*)>
			Index(FFMS_ReadIndex(opts.ffms_cache_file.string().c_str(), &ErrInfo), FFMS_DestroyIndex);
		if (!Index || FFMS_IndexBelongsToFile(Index, filename.string().c_str(), &ErrInfo))
			throw VideoOpenError("Worker index missing or stale");

		int TrackNumber = FFMS_GetFirstIndexedTrackOfType(Index, FFMS_TYPE_VIDEO, &ErrInfo);
		if (TrackNumber < 0)
			throw VideoNotSupported(std::string("Couldn't find any video tracks: ") + ErrInfo.Buffer);

		has_audio = FFMS_GetFirstTrackOfType(Index, FFMS_TYPE_AUDIO, nullptr) != -1;

		int Threads = opts.ffms_threads > 0 ? std::min(opts.ffms_threads, 2) : 2;
		int SeekMode = opts.ffms_unsafe_seeking ? FFMS_SEEK_UNSAFE : FFMS_SEEK_NORMAL;

		VideoSource = FFMS_CreateVideoSource(filename.string().c_str(), TrackNumber, Index, Threads, SeekMode, &ErrInfo);
		if (!VideoSource)
			throw VideoOpenError(std::string("Failed to open worker video track: ") + ErrInfo.Buffer);

		VideoInfo = FFMS_GetVideoProperties(VideoSource);
		const FFMS_Frame *TempFrame = FFMS_GetFrame(VideoSource, 0, &ErrInfo);
		if (!TempFrame)
			throw VideoOpenError(std::string("Failed to decode first frame: ") + ErrInfo.Buffer);

		Width = TempFrame->EncodedWidth;
		Height = TempFrame->EncodedHeight;
		OutputWidth = Width;
		OutputHeight = Height;
		if (VideoInfo->SARDen > 0 && VideoInfo->SARNum > 0)
			DAR = double(Width) * VideoInfo->SARNum / ((double)Height * VideoInfo->SARDen);
		else
			DAR = double(Width) / Height;

		VideoColorSpace = {static_cast<ycbcr_matrix>(TempFrame->ColorSpace), static_cast<ycbcr_range>(TempFrame->ColorRange)};
		HDR = IsHDRFrame(TempFrame);

		ApplyColorSpaceLocked(colormatrix);
		ApplyOutputFormatLocked();
		PopulateTimecodesAndKeyFrames(VideoSource, VideoInfo, KeyFramesList, Timecodes);
	}

	~FFmpegSourceWorkerProvider() override {
		std::lock_guard<std::mutex> lock(s_ffms_lifecycle_mutex);
		VideoSource = nullptr;
	}

	void SetSmoothPreview(bool enabled) override {
		std::lock_guard<std::mutex> lock(s_ffms_lifecycle_mutex);
		if (smooth_preview == enabled) return;
		smooth_preview = enabled;
		ApplyOutputFormatLocked();
	}
	bool IsSmoothPreview() const override { return smooth_preview; }

	void GetFrame(int n, VideoFrame &out) override {
		n = mid(0, n, GetFrameCount() - 1);
		auto frame = FFMS_GetFrame(VideoSource, n, &ErrInfo);
		if (!frame)
			throw VideoDecodeError(std::string("Failed to retrieve frame: ") + ErrInfo.Buffer);

		int cur_w = OutputWidth > 0 ? OutputWidth : Width;
		int cur_h = OutputHeight > 0 ? OutputHeight : Height;
		ApplyFlipAndRotation(frame, VideoInfo, cur_w, cur_h, out);
	}

	void SetColorSpace(ycbcr::Header matrix) override {
		std::lock_guard<std::mutex> lock(s_ffms_lifecycle_mutex);
		ApplyColorSpaceLocked(matrix);
	}

	int GetFrameCount() const override             { return VideoInfo->NumFrames; }
	int GetWidth() const override  { return (VideoInfo->Rotation % 180 == 90 || VideoInfo->Rotation % 180 == -90) ? Height : Width; }
	int GetHeight() const override { return (VideoInfo->Rotation % 180 == 90 || VideoInfo->Rotation % 180 == -90) ? Width : Height; }
	double GetDAR() const override { return (VideoInfo->Rotation % 180 == 90 || VideoInfo->Rotation % 180 == -90) ? 1 / DAR : DAR; }

	agi::vfr::Framerate GetFPS() const override                 { return Timecodes; }
	ycbcr::header_colorspace GetColorSpace() const override     { return ColorSpace; }
	ycbcr::header_colorspace GetRealColorSpace() const override { return VideoColorSpace; }
	bool IsHDRorWCG() const override                            { return HDR; }
	std::vector<int> GetKeyFrames() const override              { return KeyFramesList; }
	std::string GetDecoderName() const override                 { return "FFmpegSourceWorker"; }
	bool WantsCaching() const override                          { return false; }
	bool HasAudio() const override                              { return has_audio; }
};
}

WorkerVideoOptions SnapshotWorkerVideoOptions(agi::fs::path const& video_file) {
	WorkerVideoOptions opts;
	opts.ffms_threads = OPT_GET("Provider/Video/FFmpegSource/Decoding Threads")->GetInt();
	opts.ffms_unsafe_seeking = OPT_GET("Provider/Video/FFmpegSource/Unsafe Seeking")->GetBool();
	if (!video_file.empty() && agi::fs::FileExists(video_file)) {
		std::lock_guard<std::mutex> lock(s_ffms_lifecycle_mutex);
		FFmpegSourceProvider helper(nullptr);
		opts.ffms_cache_file = helper.GetCacheFilename(video_file);
	}
	return opts;
}

std::unique_ptr<VideoProvider> CreateFFmpegSourceWorkerProvider(agi::fs::path const& video_file, agi::ycbcr::Header colormatrix, WorkerVideoOptions const& opts) {
	if (video_file.empty() || opts.ffms_cache_file.empty())
		return nullptr;
	if (!agi::fs::FileExists(video_file) || !agi::fs::FileExists(opts.ffms_cache_file))
		return nullptr;
	try {
		return std::make_unique<FFmpegSourceWorkerProvider>(video_file, colormatrix, opts);
	}
	catch (...) {
		return nullptr;
	}
}

std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(agi::fs::path const& path, ycbcr::Header colormatrix, agi::BackgroundRunner *br) {
	return std::make_unique<FFmpegSourceVideoProvider>(path, colormatrix, br);
}

#else

WorkerVideoOptions SnapshotWorkerVideoOptions(agi::fs::path const&) {
	return {};
}

std::unique_ptr<VideoProvider> CreateFFmpegSourceWorkerProvider(agi::fs::path const&, agi::ycbcr::Header, WorkerVideoOptions const&) {
	return nullptr;
}

#endif /* WITH_FFMS2 */
