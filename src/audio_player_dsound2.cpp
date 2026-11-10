// Copyright (c) 2008, 2010, Niels Martin Hansen
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

/// @file audio_player_dsound2.cpp
/// @brief New DirectSound-based audio output
/// @ingroup audio_output
///

#ifdef WITH_DIRECTSOUND
#include "include/aegisub/audio_player.h"

#include "audio_controller.h"
#include "frame_main.h"
#include "options.h"
#include "utils.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/scoped_ptr.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <mmsystem.h>
#include <process.h>
#include <dsound.h>

namespace {
class DirectSoundPlayer2Thread;

/// @class DirectSoundPlayer2
/// @brief New implementation of DirectSound-based audio player
///
/// The core design idea is to have a playback thread that owns the DirectSound COM objects
/// and performs all playback operations, and use the player object as a proxy to
/// send commands to the playback thread.
class DirectSoundPlayer2 final : public AudioPlayer {
	/// The playback thread
	std::unique_ptr<DirectSoundPlayer2Thread> thread;

	/// Desired length in milliseconds to write ahead of the playback cursor
	int WantedLatency;

	/// Multiplier for WantedLatency to get total buffer length
	int BufferLength;

	/// @brief Tell whether playback thread is alive
	/// @return True if there is a playback thread and it's ready
	bool IsThreadAlive();

public:
	/// @brief Constructor
	DirectSoundPlayer2(agi::AudioProvider *provider, wxWindow *parent);
	/// @brief Destructor
	~DirectSoundPlayer2();

	/// @brief Start playback
	/// @param start First audio frame to play
	/// @param count Number of audio frames to play
	void Play(int64_t start,int64_t count);

	/// @brief Stop audio playback
	/// @param timerToo Whether to also stop the playback update timer
	void Stop();

	/// @brief Tell whether playback is active
	/// @return True if audio is playing back
	bool IsPlaying();

	/// @brief Get playback end position
	/// @return Audio frame index
	///
	/// Returns 0 if playback is stopped or there is no playback thread
	int64_t GetEndPosition();
	/// @brief Get approximate playback position
	/// @return Index of audio frame user is currently hearing
	///
	/// Returns 0 if playback is stopped or there is no playback thread
	int64_t GetCurrentPosition();

	/// @brief Change playback end position
	/// @param pos New end position
	void SetEndPosition(int64_t pos);

	/// @brief Change playback volume
	/// @param vol Amplification factor
	void SetVolume(double vol);

	/// @brief Change playback rate so the audio follows the video clock
	/// @param speed Multiplier, 1.0 is normal; the streamed samples are
	/// time-stretched so pitch is preserved at other speeds
	/// @return Achieved multiplier, or 0 when the format cannot be stretched
	double SetSpeed(double speed);
};

/// @brief RAII support class to init and de-init the COM library
struct COMInitialization {

	/// Flag set if an inited COM library is managed
	bool inited = false;

	/// @brief Destructor, de-inits COM if it is inited
	~COMInitialization()
	{
		if (inited) CoUninitialize();
	}

	/// @brief Initialise the COM library as single-threaded apartment if isn't already inited by us
	bool Init()
	{
		if (!inited)
		{
			if (SUCCEEDED(CoInitialize(nullptr)))
				inited = true;
		}
		return inited;
	}
};

struct ReleaseCOMObject {
	void operator()(IUnknown *obj) {
		if (obj) obj->Release();
	}
};

template<typename T>
using COMObjectRetainer = std::unique_ptr<T, ReleaseCOMObject>;

/// @brief Streaming WSOLA time-stretcher for pitch-preserved fast/slow playback
///
/// Simply playing the buffer at a changed frequency is like fast-forwarding
/// a tape: everything comes out faster *and* higher pitched. Video sites
/// instead cut the audio into short natural-pitch segments, move along the
/// content faster (or slower) than the segments play out, and crossfade the
/// splices (weighted overlap-add). The buffer then runs at its natural
/// frequency, so the pitch stays normal while the content still advances
/// speed times per wall-clock second and stays in sync with the video.
class TimeStretchEngine {
	agi::AudioProvider *provider = nullptr;
	int channels = 1;
	int bytes_per_sample = 2;
	bool float_samples = false;

	/// Output frames produced per segment hop (about 20 ms)
	int hop = 0;
	/// Segment length in frames; equals 2 * hop (hann windowed, 50 % overlap)
	int frame_len = 0;
	/// Half-width in content frames of the position search per hop
	int search = 0;
	/// Content frames consumed per hop; hop * speed
	double step = 1.0;

	/// Hann window over frame_len samples (rises then falls, sums to 1 at hop offset)
	std::vector<double> hann;

	/// Content frame where the next segment starts
	double cursor = 0;
	int64_t start_pos = 0;
	int64_t hop_count = 0;
	/// Content frame to stop fetching audio at (silence beyond)
	int64_t limit = 0;

	/// Last fetched content window, converted to double
	std::vector<double> win;
	int64_t win_start = 0;
	int64_t win_frames = 0;
	/// Provider-format staging buffer for the fetch
	std::vector<uint8_t> tmp;

	/// This hop's finalized output samples (hop * channels)
	std::vector<double> emit;
	/// The still-unplayed faded tail of the previous segment
	std::vector<double> tail;
	size_t emit_pos = 0;

	/// Fetch at most count content frames starting at start; clamps to limit
	void FetchWindow(int64_t start, int64_t count, double volume)
	{
		win_start = start;
		win_frames = count;
		if (count <= 0)
			return;

		tmp.resize(static_cast<size_t>(count) * channels * bytes_per_sample);
		provider->GetAudioWithVolume(tmp.data(), start, count, volume);

		size_t const n = static_cast<size_t>(count) * channels;
		win.resize(n);
		if (bytes_per_sample == 2) {
			auto const *src = reinterpret_cast<int16_t const *>(tmp.data());
			for (size_t i = 0; i < n; ++i) win[i] = src[i];
		}
		else if (float_samples) {
			auto const *src = reinterpret_cast<float const *>(tmp.data());
			for (size_t i = 0; i < n; ++i) win[i] = src[i];
		}
		else {
			auto const *src = reinterpret_cast<int32_t const *>(tmp.data());
			for (size_t i = 0; i < n; ++i) win[i] = src[i];
		}
	}

	/// Sample at a content frame, or silence outside the fetched window
	double At(int64_t frame, int ch) const
	{
		int64_t const rel = frame - win_start;
		if (rel < 0 || rel >= win_frames)
			return 0.0;
		return win[static_cast<size_t>(rel) * channels + ch];
	}

	/// Write one double sample out in the provider's sample format
	void Put(void *dst, int64_t frame, int ch, double v) const
	{
		size_t const i = static_cast<size_t>(frame) * channels + ch;
		if (bytes_per_sample == 2) {
			int const s = static_cast<int>(std::lround(v));
			reinterpret_cast<int16_t *>(dst)[i] = static_cast<int16_t>(std::clamp(s, -32768, 32767));
		}
		else if (float_samples) {
			reinterpret_cast<float *>(dst)[i] = static_cast<float>(std::clamp(v, -1.0, 1.0));
		}
		else {
			int64_t const s = std::llround(v);
			reinterpret_cast<int32_t *>(dst)[i] = static_cast<int32_t>(std::clamp<int64_t>(s, -2147483647LL - 1, 2147483647LL));
		}
	}

	/// Produce one hop's worth of output samples into emit
	void RunHop(double volume)
	{
		int64_t const e = static_cast<int64_t>(std::floor(cursor + 0.5));
		++hop_count;
		int64_t const nominal = static_cast<int64_t>(std::floor(start_pos + hop_count * step + 0.5));

		// Need this segment plus the candidate heads for the next start search
		int64_t const reach = std::max<int64_t>(frame_len, (nominal + search + hop) - e) + 2;
		FetchWindow(e, std::max<int64_t>(0, std::min(reach, limit - e)), volume);

		for (int f = 0; f < hop; ++f)
			for (int ch = 0; ch < channels; ++ch) {
				size_t const i = static_cast<size_t>(f) * channels + ch;
				emit[i] = tail[i] + At(e + f, ch) * hann[f];
				tail[i] = At(e + hop + f, ch) * hann[hop + f];
			}

		// WSOLA similarity search: pick the next segment's start so that its
		// head matches the phase of this segment's tail as closely as the
		// content allows, which keeps the crossfaded splices smooth.
		int best = 0;
		double best_cost = std::numeric_limits<double>::max();
		for (int d = -search; d <= search; d += 2) {
			double cost = 0.0;
			for (int f = 0; f < hop; f += 2)
				for (int ch = 0; ch < channels; ++ch) {
					double const diff = At(e + hop + f, ch) - At(nominal + d + f, ch);
					cost += diff * diff;
				}
			if (cost < best_cost) {
				best_cost = cost;
				best = d;
			}
		}

		cursor = static_cast<double>(nominal + best);
		emit_pos = 0;
	}

public:
	/// Can samples from this provider be stretched? (16-bit, 32-bit int or float)
	static bool Supported(agi::AudioProvider const *provider)
	{
		int const bps = provider->GetBytesPerSample();
		return bps == 2 || bps == 4;
	}

	/// @brief Bind to a provider and prepare per-format state
	/// Call once before Reset/Generate.
	void SetProvider(agi::AudioProvider *p)
	{
		provider = p;
		channels = std::max(1, p->GetChannels());
		bytes_per_sample = p->GetBytesPerSample();
		float_samples = p->AreSamplesFloat();
		hop = std::max(32, p->GetSampleRate() / 50);
		frame_len = 2 * hop;
		search = std::clamp(p->GetSampleRate() / 140, 4, hop / 4);

		double const pi = std::acos(-1.0);
		hann.resize(frame_len);
		for (int i = 0; i < frame_len; ++i)
			hann[i] = 0.5 - 0.5 * std::cos(pi * i / hop);

		tail.assign(static_cast<size_t>(hop) * channels, 0.0);
		emit.assign(static_cast<size_t>(hop) * channels, 0.0);
	}

	/// @brief Restart stretching from a content position
	/// @param start First content frame to play
	/// @param end Content frame to go silent after
	/// @param speed Content frames consumed per output frame multiplier
	void Reset(int64_t start, int64_t end, double speed)
	{
		start_pos = start;
		cursor = static_cast<double>(start);
		limit = end;
		step = hop * speed;
		hop_count = 0;
		std::fill(tail.begin(), tail.end(), 0.0);
		emit_pos = emit.size(); // force a fresh hop on first Generate
	}

	/// Follow playback end changes (audio display drags)
	void SetLimit(int64_t end) { limit = end; }

	/// @brief Produce stretched output frames into the DirectSound buffer
	/// @param dst   Locked buffer memory, provider sample format
	/// @param frames Number of output frames to write
	/// @param volume Amplification applied when fetching input
	void Generate(void *dst, int64_t frames, double volume)
	{
		for (int64_t f = 0; f < frames; ++f) {
			if (emit_pos >= emit.size())
				RunHop(volume);
			for (int ch = 0; ch < channels; ++ch)
				Put(dst, f, ch, emit[emit_pos + ch]);
			emit_pos += channels;
		}
	}

	/// Content frame the stretcher is working at (for end-of-clip bookkeeping)
	int64_t Cursor() const { return static_cast<int64_t>(std::floor(cursor)); }
};

/// @brief RAII wrapper around Win32 HANDLE type
struct Win32KernelHandle final : public agi::scoped_holder<HANDLE, BOOL (__stdcall *)(HANDLE)> {
	/// @brief Create with a managed handle
	/// @param handle Win32 handle to manage
	Win32KernelHandle(HANDLE handle = 0)
	: scoped_holder(handle, CloseHandle)
	{
	}

	Win32KernelHandle& operator=(HANDLE new_handle)
	{
		scoped_holder::operator=(new_handle);
		return *this;
	}
};

/// @class DirectSoundPlayer2Thread
/// @brief Playback thread class for DirectSoundPlayer2
///
/// Not based on wxThread, but uses Win32 threads directly
class DirectSoundPlayer2Thread {
	/// @brief Win32 thread entry point
	/// @param parameter Pointer to our thread object
	/// @return Thread return value, always 0 here
	static unsigned int __stdcall ThreadProc(void *parameter);
	/// @brief Thread entry point
	void Run();

	/// @brief Fill audio data into a locked buffer-pair and unlock the buffers
	/// @param buf1        First buffer in pair
	/// @param buf1sz      Byte-size of first buffer in pair
	/// @param buf2        Second buffer in pair, or null
	/// @param buf2sz      Byte-size of second buffer in pair
	/// @param input_frame First audio frame to fill into buffers
	/// @param bfr         DirectSound buffer object owning the buffer pair
	/// @return Number of bytes written
	DWORD FillAndUnlockBuffers(void *buf1, DWORD buf1sz, void *buf2, DWORD buf2sz, int64_t &input_frame, IDirectSoundBuffer8 *bfr);

	/// @brief Check for error state and throw exception if one occurred
	void CheckError();

	HWND parent;

	/// Win32 handle to the thread
	Win32KernelHandle thread_handle;

	/// Event object, world to thread, set to start playback
	Win32KernelHandle event_start_playback;

	/// Event object, world to thread, set to stop playback
	Win32KernelHandle event_stop_playback;

	/// Event object, world to thread, set if playback end time was updated
	Win32KernelHandle event_update_end_time;

	/// Event object, world to thread, set if the volume was changed
	Win32KernelHandle event_set_volume;

	/// Event object, world to thread, set if the playback speed was changed
	Win32KernelHandle event_set_speed;

	/// Event object, world to thread, set if the thread should end as soon as possible
	Win32KernelHandle event_kill_self;

	/// Event object, thread to world, set when the thread has entered its main loop
	Win32KernelHandle thread_running;

	/// Event object, thread to world, set when playback is ongoing
	Win32KernelHandle is_playing;

	/// Event object, thread to world, set if an error state has occurred (implies thread is dying)
	Win32KernelHandle error_happened;

	/// Statically allocated error message text describing reason for error_happened being set
	const char *error_message = nullptr;

	/// Playback volume, 1.0 is "unchanged"
	double volume = 1.0;

	/// Playback speed actually in effect; the position estimate scales
	/// wall-clock elapsed by it. Written by the thread only.
	volatile double speed = 1.0;

	/// Speed requested via SetSpeed; the thread applies it and copies it
	/// into speed.
	volatile double pending_speed = 1.0;

	/// Audio frame to start playback at
	int64_t start_frame = 0;

	/// Audio frame to end playback at
	int64_t end_frame = 0;

	/// Desired length in milliseconds to write ahead of the playback cursor
	int wanted_latency;

	/// Multiplier for WantedLatency to get total buffer length
	int buffer_length;

	/// System millisecond timestamp of last playback start, used to calculate playback position
	DWORD last_playback_restart;

	/// Audio provider to take sample data from
	agi::AudioProvider *provider;

	/// True when the provider's sample format supports time stretching, so
	/// that speeds other than 1.0 preserve pitch instead of shifting it
	bool stretch_mode = false;

	/// Pitch-preserving stretch engine, used when stretch_mode and speed != 1.0
	TimeStretchEngine stretcher;

public:
	/// @brief Constructor, creates and starts playback thread
	/// @param provider       Audio provider to take sample data from
	/// @param WantedLatency Desired length in milliseconds to write ahead of the playback cursor
	/// @param BufferLength  Multiplier for WantedLatency to get total buffer length
	DirectSoundPlayer2Thread(agi::AudioProvider *provider, int WantedLatency, int BufferLength, wxWindow *parent);
	/// @brief Destructor, waits for thread to have died
	~DirectSoundPlayer2Thread();

	/// @brief Start audio playback
	/// @param start Audio frame to start playback at
	/// @param count Number of audio frames to play
	void Play(int64_t start, int64_t count);

	/// @brief Stop audio playback
	void Stop();

	/// @brief Change audio playback end point
	/// @param new_end_frame New last audio frame to play
	///
	/// Playback stops instantly if new_end_frame is before the current playback position
	void SetEndFrame(int64_t new_end_frame);

	/// @brief Change audio playback volume
	/// @param new_volume New playback amplification factor, 1.0 is "unchanged"
	void SetVolume(double new_volume);

	/// @brief Change playback speed, preserving pitch when the format allows
	/// @param speed Multiplier, 1.0 is normal
	/// @return The multiplier actually achieved, or 0 when the player can
	/// neither time-stretch nor reach the matching buffer frequency (then
	/// audio would drift against the video clock, so the caller should stay
	/// silent instead)
	double SetSpeed(double speed);

	/// @brief Tell whether audio playback is active
	/// @return True if audio is being played back, false if it is not
	bool IsPlaying();

	/// @brief Get approximate current audio frame being heard by the user
	/// @return Audio frame index
	///
	/// Returns 0 if not playing
	int64_t GetCurrentFrame();

	/// @brief Get audio playback end point
	/// @return Audio frame index
	int64_t GetEndFrame();

	/// @brief Tell whether playback thread has died
	/// @return True if thread is no longer running
	bool IsDead();
};

unsigned int __stdcall DirectSoundPlayer2Thread::ThreadProc(void *parameter)
{
	static_cast<DirectSoundPlayer2Thread*>(parameter)->Run();
	return 0;
}

/// Macro used to set error_message, error_happened and end the thread
#define REPORT_ERROR(msg) \
{ \
	ResetEvent(is_playing); \
	error_message = "DirectSoundPlayer2Thread: " msg; \
	SetEvent(error_happened); \
	return; \
}

void DirectSoundPlayer2Thread::Run()
{
	COMInitialization COM_library;
	if (!COM_library.Init())
		REPORT_ERROR("Could not initialise COM")

	// Create DirectSound object
	IDirectSound8 *ds_raw = nullptr;
	if (FAILED(DirectSoundCreate8(&DSDEVID_DefaultPlayback, &ds_raw, nullptr)))
		REPORT_ERROR("Could not create DirectSound object")

	COMObjectRetainer<IDirectSound8> ds(ds_raw);

	// Ensure we can get interesting wave formats (unless we have PRIORITY we can only use a standard 8 bit format)
	ds->SetCooperativeLevel(parent, DSSCL_PRIORITY);

	// Describe the wave format
	WAVEFORMATEX waveFormat;
	waveFormat.wFormatTag = WAVE_FORMAT_PCM;
	waveFormat.nSamplesPerSec = provider->GetSampleRate();
	waveFormat.nChannels = provider->GetChannels();
	waveFormat.wBitsPerSample = provider->GetBytesPerSample() * 8;
	waveFormat.nBlockAlign = waveFormat.nChannels * waveFormat.wBitsPerSample / 8;
	waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
	waveFormat.cbSize = sizeof(waveFormat);

	// And the buffer itself
	int aim = waveFormat.nAvgBytesPerSec * (wanted_latency*buffer_length)/1000;
	int min = DSBSIZE_MIN;
	int max = DSBSIZE_MAX;
	DWORD bufSize = mid(min,aim,max); // size of entire playback buffer
	DSBUFFERDESC desc;
	desc.dwSize = sizeof(DSBUFFERDESC);
	desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLFREQUENCY;
	desc.dwBufferBytes = bufSize;
	desc.dwReserved = 0;
	desc.lpwfxFormat = &waveFormat;
	desc.guid3DAlgorithm = GUID_NULL;

	// And then create the buffer
	IDirectSoundBuffer *bfr7 = 0;
	if FAILED(ds->CreateSoundBuffer(&desc, &bfr7, 0))
		REPORT_ERROR("Could not create buffer")

	// But it's an old version interface we get, query it for the DSound8 interface
	IDirectSoundBuffer8 *bfr_raw = nullptr;
	if (FAILED(bfr7->QueryInterface(IID_IDirectSoundBuffer8, (LPVOID*)&bfr_raw)))
		REPORT_ERROR("Buffer doesn't support version 8 interface")
	COMObjectRetainer<IDirectSoundBuffer8> bfr(bfr_raw);
	bfr7->Release();
	bfr7 = 0;

	//wx Log Debug("DirectSoundPlayer2: Created buffer of %d bytes, supposed to be %d milliseconds or %d frames", bufSize, WANTED_LATENCY*BUFFER_LENGTH, bufSize/provider->GetBytesPerSample());

	// Now we're ready to roll!
	SetEvent(thread_running);
	bool running = true;

	HANDLE events_to_wait[] = {
		event_start_playback,
		event_stop_playback,
		event_update_end_time,
		event_set_volume,
		event_set_speed,
		event_kill_self
	};

	int64_t next_input_frame = 0;
	DWORD buffer_offset = 0;
	bool playback_should_be_running = false;
	int current_latency = wanted_latency;
	const DWORD wanted_latency_bytes = wanted_latency*waveFormat.nSamplesPerSec*provider->GetBytesPerSample()/1000;

	while (running)
	{
		// In stretch mode the buffer always drains at its natural rate (only
		// the content inside it is sped up), so no rate compensation is
		// needed. Legacy pitch-shifting playback drains faster at higher
		// speeds, so wake up sooner to keep refilling ahead of the cursor.
		double const drain = stretch_mode ? 1.0 : std::max(0.25, 1.0 / speed);
		DWORD wait_ms = static_cast<DWORD>(current_latency * drain);
		DWORD wait_result = WaitForMultipleObjects(sizeof(events_to_wait)/sizeof(HANDLE), events_to_wait, FALSE, wait_ms);

		switch (wait_result)
		{
		case WAIT_OBJECT_0+0:
restart_playback:
			{
				// Start or restart playback
				bfr->Stop();
				speed = pending_speed;

				next_input_frame = start_frame;
				if (stretch_mode)
					stretcher.Reset(start_frame, end_frame, speed);

				DWORD buf_size; // size of buffer locked for filling
				void *buf;
				buffer_offset = 0;

				if (FAILED(bfr->SetCurrentPosition(0)))
					REPORT_ERROR("Could not reset playback buffer cursor before filling first buffer.")

				HRESULT res = bfr->Lock(buffer_offset, 0, &buf, &buf_size, 0, 0, DSBLOCK_ENTIREBUFFER);
				if (FAILED(res))
				{
					if (res == DSERR_BUFFERLOST)
					{
						// Try to regain the buffer
						if (FAILED(bfr->Restore()) ||
							FAILED(bfr->Lock(buffer_offset, 0, &buf, &buf_size, 0, 0, DSBLOCK_ENTIREBUFFER)))
						{
							REPORT_ERROR("Lost buffer and could not restore it.")
						}
					}
					else
					{
						REPORT_ERROR("Could not lock buffer for playback.")
					}
				}

				// Clear the buffer in case we can't fill it completely
				memset(buf, 0, buf_size);

				DWORD bytes_filled = FillAndUnlockBuffers(buf, buf_size, 0, 0, next_input_frame, bfr.get());
				buffer_offset += bytes_filled;
				if (buffer_offset >= bufSize) buffer_offset -= bufSize;

				if (FAILED(bfr->SetCurrentPosition(0)))
					REPORT_ERROR("Could not reset playback buffer cursor before playback.")

				if (bytes_filled < wanted_latency_bytes)
				{
					// Very short playback length, do without streaming playback
					current_latency = (bytes_filled*1000) / (waveFormat.nSamplesPerSec*provider->GetBytesPerSample());
					if (FAILED(bfr->Play(0, 0, 0)))
						REPORT_ERROR("Could not start single-buffer playback.")
				}
				else
				{
					// We filled the entire buffer so there's reason to do streaming playback
					current_latency = wanted_latency;
					if (FAILED(bfr->Play(0, 0, DSBPLAY_LOOPING)))
						REPORT_ERROR("Could not start looping playback.")
				}

				SetEvent(is_playing);
				playback_should_be_running = true;

				break;
			}

		case WAIT_OBJECT_0+1:
stop_playback:
			// Stop playing
			bfr->Stop();
			ResetEvent(is_playing);
			playback_should_be_running = false;
			break;

		case WAIT_OBJECT_0+2:
			// Set end frame
			if (end_frame <= next_input_frame)
			{
				goto stop_playback;
			}

			// If the user is dragging the start or end point in the audio display
			// the set end frame events might come in faster than the timeouts happen
			// and then new data never get filled into the buffer. See bug #915.
			goto do_fill_buffer;

		case WAIT_OBJECT_0+3:
			// Change volume
			// We aren't thread safe right now, filling the buffers grabs volume directly
			// from the field set by the controlling thread, but it shouldn't be a major
			// problem if race conditions do occur, just some momentary distortion.
			goto do_fill_buffer;

		case WAIT_OBJECT_0+4:
			// Change playback speed
			{
				if (stretch_mode) {
					// The buffer keeps running at its natural frequency (so
					// pitch is unchanged); only the content advance per second
					// changes, matching the video controller's virtual clock.
					// Fold the elapsed samples into start_frame first to keep
					// the tick-based position estimate continuous, then refill
					// the buffer from there with the new stretch ratio.
					if (playback_should_be_running)
					{
						DWORD elapsed = GetTickCount() - last_playback_restart;
						start_frame += static_cast<int64_t>(elapsed * provider->GetSampleRate() * speed / 1000.0);
						last_playback_restart = GetTickCount();
					}
					speed = pending_speed;
					if (playback_should_be_running)
						goto restart_playback;
					break;
				}

				// Legacy providers with unsupported sample formats: the
				// buffer consumes its samples at the new frequency, so
				// content time advances speed-times wall-clock time at the
				// cost of a matching pitch shift.
				DWORD new_hz = static_cast<DWORD>(provider->GetSampleRate() * pending_speed);
				if (FAILED(bfr->SetFrequency(new_hz)))
				{
					// Shouldn't happen (SetSpeed pre-checks the documented
					// frequency range); keep the old speed rather than let
					// the audio drift.
					pending_speed = speed;
					break;
				}
				// Keep the tick-based position estimate continuous across the
				// change: fold the elapsed samples into start_frame first.
				if (playback_should_be_running)
				{
					DWORD elapsed = GetTickCount() - last_playback_restart;
					start_frame += static_cast<int64_t>(elapsed * provider->GetSampleRate() * speed / 1000.0);
					last_playback_restart = GetTickCount();
				}
				speed = pending_speed;
			}
			break;

		case WAIT_OBJECT_0+5:
			// Perform suicide
			running = false;
			goto stop_playback;

		case WAIT_TIMEOUT:
do_fill_buffer:
			{
				// Time to fill more into buffer
				if (!playback_should_be_running)
					break;

				DWORD status;
				if (FAILED(bfr->GetStatus(&status)))
					REPORT_ERROR("Could not get playback buffer status")

				if (!(status & DSBSTATUS_LOOPING))
				{
					// Not looping playback...
					// hopefully we only triggered timeout after being done with the buffer
					goto stop_playback;
				}

				DWORD play_cursor;
				if (FAILED(bfr->GetCurrentPosition(&play_cursor, 0)))
					REPORT_ERROR("Could not get play cursor position for filling buffer.")

				int bytes_needed = (int)play_cursor - (int)buffer_offset;
				if (bytes_needed < 0) bytes_needed += (int)bufSize;

				// Requesting zero buffer makes Windows cry, and zero buffer seemed to be
				// a common request on Windows 7. (Maybe related to the new timer coalescing?)
				// We'll probably get non-zero bytes requested on the next iteration.
				if (bytes_needed == 0)
					break;

				DWORD buf1sz, buf2sz;
				void *buf1, *buf2;

				assert(bytes_needed > 0);
				assert(buffer_offset < bufSize);
				assert((DWORD)bytes_needed <= bufSize);

				HRESULT res = bfr->Lock(buffer_offset, bytes_needed, &buf1, &buf1sz, &buf2, &buf2sz, 0);
				switch (res)
				{
				case DSERR_BUFFERLOST:
					// Try to regain the buffer
					// When the buffer was lost the entire contents was lost too, so we have to start over
					if (SUCCEEDED(bfr->Restore()) &&
					    SUCCEEDED(bfr->Lock(0, bufSize, &buf1, &buf1sz, &buf2, &buf2sz, 0)) &&
					    SUCCEEDED(bfr->Play(0, 0, DSBPLAY_LOOPING)))
					{
						LOG_D("audio/player/dsound") << "Lost and restored buffer";
						break;
					}
					REPORT_ERROR("Lost buffer and could not restore it.")

				case DSERR_INVALIDPARAM:
					REPORT_ERROR("Invalid parameters to IDirectSoundBuffer8::Lock().")

				case DSERR_INVALIDCALL:
					REPORT_ERROR("Invalid call to IDirectSoundBuffer8::Lock().")

				case DSERR_PRIOLEVELNEEDED:
					REPORT_ERROR("Incorrect priority level set on DirectSoundBuffer8 object.")

				default:
					if (FAILED(res))
						REPORT_ERROR("Could not lock audio buffer, unknown error.")
					break;
				}

				DWORD bytes_filled = FillAndUnlockBuffers(buf1, buf1sz, buf2, buf2sz, next_input_frame, bfr.get());
				buffer_offset += bytes_filled;
				if (buffer_offset >= bufSize) buffer_offset -= bufSize;

				if (bytes_filled < 1024)
				{
					// Arbitrary low number, we filled in very little so better get back to filling in the rest with silence
					// really fast... set latency to zero in this case.
					current_latency = 0;
				}
				else if (bytes_filled < wanted_latency_bytes)
				{
					// Didn't fill as much as we wanted to, let's get back to filling sooner than normal
					current_latency = (bytes_filled*1000) / (waveFormat.nSamplesPerSec*provider->GetBytesPerSample());
				}
				else
				{
					// Plenty filled in, do regular latency
					current_latency = wanted_latency;
				}

				break;
			}

		default:
			REPORT_ERROR("Something bad happened while waiting on events in playback loop, either the wait failed or an event object was abandoned.")
			break;
		}
	}
}

#undef REPORT_ERROR

DWORD DirectSoundPlayer2Thread::FillAndUnlockBuffers(void *buf1, DWORD buf1sz, void *buf2, DWORD buf2sz, int64_t &input_frame, IDirectSoundBuffer8 *bfr)
{
	// Assume buffers have been locked and are ready to be filled

	DWORD bytes_per_frame = provider->GetChannels() * provider->GetBytesPerSample();
	DWORD buf1szf = buf1sz / bytes_per_frame;
	DWORD buf2szf = buf2sz / bytes_per_frame;

	if (input_frame >= end_frame)
	{
		// Silence

		if (buf1)
			memset(buf1, 0, buf1sz);

		if (buf2)
			memset(buf2, 0, buf2sz);

		input_frame += buf1szf + buf2szf;

		bfr->Unlock(buf1, buf1sz, buf2, buf2sz); // should be checking for success

		return buf1sz + buf2sz;
	}

	if (stretch_mode && speed != 1.0)
	{
		// Pitch-preserved speed change: hand the buffer stretched samples
		// and advance the cursor by the content the stretcher consumed. The
		// engine fades to silence itself as the cursor passes end_frame.
		stretcher.SetLimit(end_frame);

		if (buf1 && buf1sz)
			stretcher.Generate(buf1, buf1szf, volume);

		if (buf2 && buf2sz)
			stretcher.Generate(buf2, buf2szf, volume);

		input_frame = stretcher.Cursor();

		bfr->Unlock(buf1, buf1sz, buf2, buf2sz);

		return buf1sz + buf2sz;
	}

	if (buf1 && buf1sz)
	{
		if (buf1szf + input_frame > end_frame)
		{
			buf1szf = end_frame - input_frame;
			buf1sz = buf1szf * bytes_per_frame;
			buf2szf = 0;
			buf2sz = 0;
		}

		provider->GetAudioWithVolume(buf1, input_frame, buf1szf, volume);

		input_frame += buf1szf;
	}

	if (buf2 && buf2sz)
	{
		if (buf2szf + input_frame > end_frame)
		{
			buf2szf = end_frame - input_frame;
			buf2sz = buf2szf * bytes_per_frame;
		}

		provider->GetAudioWithVolume(buf2, input_frame, buf2szf, volume);

		input_frame += buf2szf;
	}

	bfr->Unlock(buf1, buf1sz, buf2, buf2sz); // bad? should check for success

	return buf1sz + buf2sz;
}

void DirectSoundPlayer2Thread::CheckError()
{
	try
	{
		switch (WaitForSingleObject(error_happened, 0))
		{
		case WAIT_OBJECT_0:
			throw error_message;

		case WAIT_ABANDONED:
			throw "The DirectShowPlayer2Thread error signal event was abandoned, somehow. This should not happen.";

		case WAIT_FAILED:
			throw "Failed checking state of DirectShowPlayer2Thread error signal event.";

		case WAIT_TIMEOUT:
		default:
			return;
		}
	}
	catch (...)
	{
		ResetEvent(is_playing);
		ResetEvent(thread_running);
		throw;
	}
}

DirectSoundPlayer2Thread::DirectSoundPlayer2Thread(agi::AudioProvider *provider, int WantedLatency, int BufferLength, wxWindow *parent)
: parent((HWND)parent->GetHandle())
, event_start_playback  (CreateEvent(0, FALSE, FALSE, 0))
, event_stop_playback   (CreateEvent(0, FALSE, FALSE, 0))
, event_update_end_time (CreateEvent(0, FALSE, FALSE, 0))
, event_set_volume      (CreateEvent(0, FALSE, FALSE, 0))
, event_set_speed       (CreateEvent(0, FALSE, FALSE, 0))
, event_kill_self       (CreateEvent(0, FALSE, FALSE, 0))
, thread_running        (CreateEvent(0,  TRUE, FALSE, 0))
, is_playing            (CreateEvent(0,  TRUE, FALSE, 0))
, error_happened        (CreateEvent(0, FALSE, FALSE, 0))
, wanted_latency(WantedLatency)
, buffer_length(BufferLength)
, provider(provider)
{
	if (TimeStretchEngine::Supported(provider)) {
		stretch_mode = true;
		stretcher.SetProvider(provider);
	}

	thread_handle = (HANDLE)_beginthreadex(0, 0, ThreadProc, this, 0, 0);

	if (!thread_handle)
		throw AudioPlayerOpenError("Failed creating playback thread in DirectSoundPlayer2. This is bad.");

	HANDLE running_or_error[] = { thread_running, error_happened };
	switch (WaitForMultipleObjects(2, running_or_error, FALSE, INFINITE))
	{
	case WAIT_OBJECT_0:
		// running, all good
		return;

	case WAIT_OBJECT_0 + 1:
		// error happened, we fail
		throw AudioPlayerOpenError(error_message);

	default:
		throw AudioPlayerOpenError("Failed wait for thread start or thread error in DirectSoundPlayer2. This is bad.");
	}
}

DirectSoundPlayer2Thread::~DirectSoundPlayer2Thread()
{
	SetEvent(event_kill_self);
	WaitForSingleObject(thread_handle, INFINITE);
}

void DirectSoundPlayer2Thread::Play(int64_t start, int64_t count)
{
	CheckError();

	start_frame = start;
	end_frame = start+count;
	SetEvent(event_start_playback);

	last_playback_restart = GetTickCount();

	// Block until playback actually begins to avoid race conditions with
	// checking if playback is in progress
	HANDLE events_to_wait[] = { is_playing, error_happened };
	switch (WaitForMultipleObjects(2, events_to_wait, FALSE, INFINITE))
	{
	case WAIT_OBJECT_0+0: // Playing
		LOG_D("audio/player/dsound") << "Playback begun";
		break;
	case WAIT_OBJECT_0+1: // Error
		throw error_message;
	default:
		throw agi::InternalError("Unexpected result from WaitForMultipleObjects in DirectSoundPlayer2Thread::Play");
	}
}

void DirectSoundPlayer2Thread::Stop()
{
	CheckError();

	SetEvent(event_stop_playback);
}

void DirectSoundPlayer2Thread::SetEndFrame(int64_t new_end_frame)
{
	CheckError();

	end_frame = new_end_frame;
	SetEvent(event_update_end_time);
}

void DirectSoundPlayer2Thread::SetVolume(double new_volume)
{
	CheckError();

	volume = new_volume;
	SetEvent(event_set_volume);
}

double DirectSoundPlayer2Thread::SetSpeed(double new_speed)
{
	CheckError();

	if (new_speed <= 0.0) return 0.0;

	if (stretch_mode) {
		// Pitch is preserved by time-stretching the streamed samples instead
		// of changing the buffer frequency, so the device's supported
		// frequency range does not limit what the video clock can run at.
		pending_speed = new_speed;
		SetEvent(event_set_speed);
		return pending_speed;
	}

	DWORD const natural = provider->GetSampleRate();
	DWORD const target = new_speed == 1.0 ? natural : static_cast<DWORD>(natural * new_speed);

	// Legacy fallback (unsupported sample format): the frequency must sit
	// inside the range IDirectSoundBuffer::SetFrequency documents; outside it
	// the call is refused, the audio would keep its old rate and drift
	// against the video clock, so the caller mutes instead.
	if (target < DSBFREQUENCY_MIN || target > DSBFREQUENCY_MAX) return 0.0;

	pending_speed = static_cast<double>(target) / natural;
	SetEvent(event_set_speed);

	// The playback thread applies the change asynchronously; report the rate
	// it is about to run at so the caller's clock matches.
	return pending_speed;
}

bool DirectSoundPlayer2Thread::IsPlaying()
{
	CheckError();

	switch (WaitForSingleObject(is_playing, 0))
	{
	case WAIT_ABANDONED:
		throw "The DirectShowPlayer2Thread playback state event was abandoned, somehow. This should not happen.";

	case WAIT_FAILED:
		throw "Failed checking state of DirectShowPlayer2Thread playback state event.";

	case WAIT_OBJECT_0:
		return true;

	case WAIT_TIMEOUT:
	default:
		return false;
	}
}

int64_t DirectSoundPlayer2Thread::GetCurrentFrame()
{
	CheckError();

	if (!IsPlaying()) return 0;

	int64_t milliseconds_elapsed = GetTickCount() - last_playback_restart;

	// Content is consumed at speed times the natural rate either way: in
	// stretch mode the streamed samples themselves are compressed in time
	// while the buffer runs at its natural frequency, and in the legacy
	// fallback the buffer plays faster with a matching pitch shift. Either
	// way the heard frame advances proportionally faster than wall-clock.
	return start_frame + static_cast<int64_t>(milliseconds_elapsed * provider->GetSampleRate() * speed / 1000.0);
}

int64_t DirectSoundPlayer2Thread::GetEndFrame()
{
	CheckError();

	return end_frame;
}

bool DirectSoundPlayer2Thread::IsDead()
{
	switch (WaitForSingleObject(thread_running, 0))
	{
	case WAIT_OBJECT_0:
		return false;

	default:
		return true;
	}
}

DirectSoundPlayer2::DirectSoundPlayer2(agi::AudioProvider *provider, wxWindow *parent)
: AudioPlayer(provider)
{
	// The buffer will hold BufferLength times WantedLatency milliseconds of audio
	WantedLatency = OPT_GET("Player/Audio/DirectSound/Buffer Latency")->GetInt();
	BufferLength = OPT_GET("Player/Audio/DirectSound/Buffer Length")->GetInt();

	// sanity checking
	if (WantedLatency <= 0)
		WantedLatency = 100;
	if (BufferLength <= 0)
		BufferLength = 5;

	try
	{
		thread = std::make_unique<DirectSoundPlayer2Thread>(provider, WantedLatency, BufferLength, parent);
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
		throw AudioPlayerOpenError(msg);
	}
}

DirectSoundPlayer2::~DirectSoundPlayer2()
{
}

bool DirectSoundPlayer2::IsThreadAlive()
{
	if (thread && thread->IsDead())
	{
		thread.reset();
	}

	return !!thread;
}

void DirectSoundPlayer2::Play(int64_t start,int64_t count)
{
	try
	{
		thread->Play(start, count);
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
	}
}

void DirectSoundPlayer2::Stop()
{
	try
	{
		if (IsThreadAlive()) thread->Stop();
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
	}
}

bool DirectSoundPlayer2::IsPlaying()
{
	try
	{
		if (!IsThreadAlive()) return false;
		return thread->IsPlaying();
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
		return false;
	}
}

int64_t DirectSoundPlayer2::GetEndPosition()
{
	try
	{
		if (!IsThreadAlive()) return 0;
		return thread->GetEndFrame();
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
		return 0;
	}
}

int64_t DirectSoundPlayer2::GetCurrentPosition()
{
	try
	{
		if (!IsThreadAlive()) return 0;
		return thread->GetCurrentFrame();
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
		return 0;
	}
}

void DirectSoundPlayer2::SetEndPosition(int64_t pos)
{
	try
	{
		if (IsThreadAlive()) thread->SetEndFrame(pos);
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
	}
}

void DirectSoundPlayer2::SetVolume(double vol)
{
	try
	{
		if (IsThreadAlive()) thread->SetVolume(vol);
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
	}
}

double DirectSoundPlayer2::SetSpeed(double speed)
{
	try
	{
		if (!IsThreadAlive()) return 0.0;
		return thread->SetSpeed(speed);
	}
	catch (const char *msg)
	{
		LOG_E("audio/player/dsound") << msg;
		return 0.0;
	}
}
}

std::unique_ptr<AudioPlayer> CreateDirectSound2Player(agi::AudioProvider *provider, wxWindow *parent) {
	return std::make_unique<DirectSoundPlayer2>(provider, parent);
}

#endif // WITH_DIRECTSOUND
