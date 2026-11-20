// Copyright (c) 2026, Aegisub Contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR PROFITS,
// WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
// OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"

#include <libaegisub/signal.h>

#include <wx/string.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct BackgroundTaskHandle {
	uint64_t id = 0;
	std::shared_ptr<std::atomic<bool>> cancel;
	std::shared_ptr<std::atomic<bool>> alive;
};

class BackgroundTaskManager {
	struct TaskEntry {
		uint64_t id = 0;
		wxString title;
		wxString tag;
		wxString message;
		int done = 0;
		int total = 0;
		bool cancel_requested = false;
		std::vector<int> locked_ids;
		std::shared_ptr<std::atomic<bool>> cancel_flag;
		std::shared_ptr<std::atomic<bool>> alive_flag;
		std::thread coordinator;
	};

	agi::Context *ctx_;
	uint64_t next_task_id_ = 1;
	std::vector<std::unique_ptr<TaskEntry>> tasks_;
	std::map<int, uint64_t> locked_line_owners_;

	agi::signal::Signal<> AnnounceLockStateChanged;
	agi::signal::Signal<bool, int, int, wxString const&> AnnounceTaskProgress;

	void RebuildLockedOwners() {
		locked_line_owners_.clear();
		for (auto const& task : tasks_) {
			for (int id : task->locked_ids) {
				locked_line_owners_[id] = task->id;
			}
		}
	}

	void EmitProgress() {
		if (tasks_.empty()) {
			AnnounceTaskProgress(false, 0, 0, wxString());
			return;
		}
		int sum_done = 0;
		int sum_total = 0;
		for (auto const& t : tasks_) {
			sum_done += std::max(0, t->done);
			sum_total += std::max(1, t->total);
		}
		auto const& front_or_back = *tasks_.back();
		wxString text = front_or_back.message.IsEmpty()
			? wxString::Format("%s (%d/%d)", front_or_back.title, front_or_back.done, front_or_back.total)
			: front_or_back.message;
		if (tasks_.size() > 1) {
			text = wxString::Format(to_wx("%zu项任务 | %s"), tasks_.size(), text);
		}
		AnnounceTaskProgress(true, sum_done, sum_total, text);
	}

	TaskEntry* FindTask(uint64_t task_id) {
		for (auto& t : tasks_) {
			if (t->id == task_id) return t.get();
		}
		return nullptr;
	}

public:
	explicit BackgroundTaskManager(agi::Context *ctx) : ctx_(ctx) {}

	~BackgroundTaskManager() {
		CancelAllAndJoin();
	}

	DEFINE_SIGNAL_ADDERS(AnnounceLockStateChanged, AddLockStateListener)
	DEFINE_SIGNAL_ADDERS(AnnounceTaskProgress, AddTaskProgressListener)

	bool HasActiveTasks() const {
		return !tasks_.empty();
	}

	bool IsLineLocked(int line_id) const {
		return locked_line_owners_.find(line_id) != locked_line_owners_.end();
	}

	bool IsLineLocked(const AssDialogue *d) const {
		return d && IsLineLocked(d->Id);
	}

	wxString GetLockTag(const AssDialogue *d) const {
		if (!d) return wxString();
		auto it = locked_line_owners_.find(d->Id);
		if (it == locked_line_owners_.end()) return wxString();
		for (auto const& t : tasks_) {
			if (t->id == it->second) return t->tag;
		}
		return wxString();
	}

	AssDialogue* FindLineById(int line_id) const {
		if (!ctx_ || !ctx_->ass) return nullptr;
		for (auto& diag : ctx_->ass->Events) {
			if (diag.Id == line_id) return &diag;
		}
		return nullptr;
	}

	BackgroundTaskHandle BeginTask(wxString const& title, wxString const& lock_tag, std::vector<int> const& locked_line_ids, int total_steps) {
		auto entry = std::make_unique<TaskEntry>();
		entry->id = next_task_id_++;
		entry->title = title;
		entry->tag = lock_tag;
		entry->total = std::max(1, total_steps);
		entry->locked_ids = locked_line_ids;
		entry->cancel_flag = std::make_shared<std::atomic<bool>>(false);
		entry->alive_flag = std::make_shared<std::atomic<bool>>(true);
		entry->message = wxString::Format("%s (0/%d)", title, total_steps);

		BackgroundTaskHandle handle{entry->id, entry->cancel_flag, entry->alive_flag};
		tasks_.push_back(std::move(entry));
		RebuildLockedOwners();
		MoveSelectionOffLockedLines();
		AnnounceLockStateChanged();
		EmitProgress();
		return handle;
	}

	void AttachCoordinator(uint64_t task_id, std::thread&& t) {
		if (TaskEntry* entry = FindTask(task_id)) {
			entry->coordinator = std::move(t);
		} else if (t.joinable()) {
			t.join();
		}
	}

	void UpdateProgress(uint64_t task_id, int done, int total, wxString const& msg) {
		TaskEntry* entry = FindTask(task_id);
		if (!entry) return;
		entry->done = std::max(0, done);
		entry->total = std::max(1, total);
		if (entry->cancel_requested) {
			entry->message = wxString::Format(to_wx("正在停止: %s"), entry->title);
		} else {
			entry->message = msg;
		}
		EmitProgress();
	}

	void FinishTask(uint64_t task_id) {
		for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
			if ((*it)->id == task_id) {
				std::thread coord = std::move((*it)->coordinator);
				(*it)->alive_flag->store(false);
				tasks_.erase(it);
				RebuildLockedOwners();
				if (coord.joinable()) {
					coord.join();
				}
				AnnounceLockStateChanged();
				EmitProgress();
				return;
			}
		}
	}

	void CancelAll() {
		if (tasks_.empty()) return;
		for (auto& t : tasks_) {
			t->cancel_requested = true;
			t->cancel_flag->store(true);
			t->message = wxString::Format(to_wx("正在停止: %s"), t->title);
		}
		EmitProgress();
	}

	void CancelAllAndJoin() {
		if (tasks_.empty()) return;
		std::vector<std::thread> coords;
		for (auto& t : tasks_) {
			t->alive_flag->store(false);
			t->cancel_flag->store(true);
			if (t->coordinator.joinable()) {
				coords.push_back(std::move(t->coordinator));
			}
		}
		tasks_.clear();
		locked_line_owners_.clear();
		for (auto& coord : coords) {
			if (coord.joinable()) coord.join();
		}
		AnnounceLockStateChanged();
		EmitProgress();
	}

	void MoveSelectionOffLockedLines() {
		if (!ctx_ || !ctx_->ass || !ctx_->selectionController) return;
		auto const& sel = ctx_->selectionController->GetSelectedSet();
		AssDialogue* active = ctx_->selectionController->GetActiveLine();

		bool needs_move = IsLineLocked(active);
		if (!needs_move) {
			for (AssDialogue* d : sel) {
				if (IsLineLocked(d)) {
					needs_move = true;
					break;
				}
			}
		}
		if (!needs_move) return;

		Selection new_sel;
		for (AssDialogue* d : sel) {
			if (!IsLineLocked(d)) {
				new_sel.insert(d);
			}
		}

		AssDialogue* new_active = (active && !IsLineLocked(active)) ? active : nullptr;
		if (!new_active && !new_sel.empty()) {
			for (auto& diag : ctx_->ass->Events) {
				if (new_sel.count(&diag)) {
					new_active = &diag;
					break;
				}
			}
		}

		if (!new_active && !ctx_->ass->Events.empty()) {
			auto anchor = active ? ctx_->ass->iterator_to(*active) : ctx_->ass->Events.begin();
			for (auto it = anchor; it != ctx_->ass->Events.end(); ++it) {
				if (!IsLineLocked(&*it)) {
					new_active = &*it;
					break;
				}
			}
			if (!new_active && anchor != ctx_->ass->Events.begin()) {
				auto it = anchor;
				do {
					--it;
					if (!IsLineLocked(&*it)) {
						new_active = &*it;
						break;
					}
				} while (it != ctx_->ass->Events.begin());
			}
			if (new_active) {
				new_sel.insert(new_active);
			}
		}

		ctx_->selectionController->SetSelectionAndActive(std::move(new_sel), new_active);
	}
};
