// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "selection_controller.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "include/aegisub/context.h"
#include "subs_controller.h"

#include <algorithm>

#include "background_task_manager.h"

SelectionController::SelectionController(agi::Context *c) : context(c) { }

void SelectionController::SetSelectedSet(Selection new_selection) {
	if (context && context->bgTasks) {
		for (auto it = new_selection.begin(); it != new_selection.end(); ) {
			if (context->bgTasks->IsLineLocked(*it))
				it = new_selection.erase(it);
			else
				++it;
		}
	}
	selection = std::move(new_selection);
	AnnounceSelectedSetChanged();
}

void SelectionController::SetActiveLine(AssDialogue *new_line) {
	if (new_line && context && context->bgTasks && context->bgTasks->IsLineLocked(new_line))
		return;
	if (new_line != active_line) {
		active_line = new_line;
		if (active_line)
			context->ass->Properties.active_row = active_line->Row;
		AnnounceActiveLineChanged(new_line);
	}
}

void SelectionController::SetSelectionAndActive(Selection new_selection, AssDialogue *new_line) {
	if (context && context->bgTasks) {
		for (auto it = new_selection.begin(); it != new_selection.end(); ) {
			if (context->bgTasks->IsLineLocked(*it))
				it = new_selection.erase(it);
			else
				++it;
		}
		if (new_line && context->bgTasks->IsLineLocked(new_line)) {
			new_line = nullptr;
			if (!new_selection.empty() && context->ass) {
				for (auto& diag : context->ass->Events) {
					if (new_selection.count(&diag)) {
						new_line = &diag;
						break;
					}
				}
			}
			if (!new_line && active_line && !context->bgTasks->IsLineLocked(active_line)) {
				new_line = active_line;
			}
		}
	}
	bool active_line_changed = new_line != active_line;
	selection = std::move(new_selection);
	active_line = new_line;
	if (active_line)
		context->ass->Properties.active_row = active_line->Row;

	AnnounceSelectedSetChanged();
	if (active_line_changed)
		AnnounceActiveLineChanged(new_line);
}

std::vector<AssDialogue *> SelectionController::GetSortedSelection() const {
	std::vector<AssDialogue *> ret(selection.begin(), selection.end());
	sort(begin(ret), end(ret), [](AssDialogue *a, AssDialogue *b) { return a->Row < b->Row; });
	return ret;
}

void SelectionController::PrevLine() {
	if (!active_line) return;
	auto it = context->ass->iterator_to(*active_line);
	while (it != context->ass->Events.begin()) {
		--it;
		if (!context->bgTasks || !context->bgTasks->IsLineLocked(&*it)) {
			SetSelectionAndActive({&*it}, &*it);
			return;
		}
	}
}

void SelectionController::NextLine() {
	if (!active_line) return;
	auto it = context->ass->iterator_to(*active_line);
	while (++it != context->ass->Events.end()) {
		if (!context->bgTasks || !context->bgTasks->IsLineLocked(&*it)) {
			SetSelectionAndActive({&*it}, &*it);
			return;
		}
	}
}
