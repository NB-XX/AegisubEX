// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file toolbar.cpp
/// @brief Dynamic menu toolbar generator.
/// @ingroup toolbar menu

#include "include/aegisub/toolbar.h"

#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/hotkey.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "video_controller.h"

#include <libaegisub/hotkey.h>
#include <libaegisub/json.h>
#include <libaegisub/log.h>
#include <libaegisub/signal.h>
#include <libaegisub/string.h>

#include <boost/interprocess/streams/bufferstream.hpp>
#include <set>
#include <vector>

#include <wx/choice.h>
#include <wx/frame.h>
#include <wx/toolbar.h>

namespace {
	json::Object const& get_root() {
		static json::Object root;
		if (root.empty()) {
			size_t len = sizeof(default_toolbar);
			if (default_toolbar[0] == '{') {
				int depth = 0;
				bool in_string = false;
				bool escape = false;
				for (size_t i = 0; i < 65536; ++i) {
					char c = static_cast<char>(default_toolbar[i]);
					if (escape) {
						escape = false;
						continue;
					}
					if (c == '\\' && in_string) {
						escape = true;
						continue;
					}
					if (c == '"') {
						in_string = !in_string;
						continue;
					}
					if (!in_string) {
						if (c == '{') depth++;
						else if (c == '}') {
							depth--;
							if (depth == 0) {
								len = i + 1;
								break;
							}
						}
					}
				}
			}
			boost::interprocess::ibufferstream stream((const char *)default_toolbar, len);
			root = std::move(static_cast<json::Object&>(agi::json_util::parse(stream)));
		}
		return root;
	}

	class Toolbar final : public wxToolBar {
		/// Window ID of first toolbar control
		static const int TOOL_ID_BASE = 5000;

		/// Toolbar name in config file
		std::string name;
		/// Project context
		agi::Context *context;
		/// Commands for each of the buttons
		std::vector<cmd::Command *> commands;
		std::set<std::string> hidden_commands;
		/// Hotkey context
		std::string ht_context;

		/// Current icon size
		int icon_size;

		/// Playback speed picker embedded in the video toolbar next to the
		/// play button (nullptr on other toolbars; ClearTools() in
		/// RegenerateToolbar destroys the previous one before Populate
		/// recreates it)
		wxChoice *speed_choice = nullptr;

		/// Listener for icon size change signal
		agi::signal::Connection icon_size_slot;

		/// Listener for hotkey change signal
		agi::signal::Connection hotkeys_changed_slot;

		/// Enable/disable the toolbar buttons
		void OnIdle(wxIdleEvent &) {
			for (size_t i = 0; i < commands.size(); ++i) {
				if (commands[i]->Type() & cmd::COMMAND_VALIDATE)
					EnableTool(TOOL_ID_BASE + i, commands[i]->Validate(context));
				if (commands[i]->Type() & cmd::COMMAND_TOGGLE || commands[i]->Type() & cmd::COMMAND_RADIO)
					ToggleTool(TOOL_ID_BASE + i, commands[i]->IsActive(context));
			}
		}

		/// Embedded playback speed picker, added right after the play button
		/// on the video toolbar
		void AddSpeedPicker() {
			speed_choice = new wxChoice(this, -1, wxDefaultPosition, wxSize(72, -1));
			for (const char* const label : { "0.5x", "1x", "2x", "4x" })
				speed_choice->Append(to_wx(label));
			double s = context->videoController->GetSpeed();
			speed_choice->SetSelection(s <= 0.75 ? 0 : s <= 1.5 ? 1 : s <= 3.0 ? 2 : 3);
			speed_choice->SetToolTip(_("Playback speed (sound follows; pitch is preserved at other speeds)"));
			wxChoice *choice = speed_choice;
			choice->Bind(wxEVT_CHOICE, [this, choice](wxCommandEvent&) {
				static constexpr double speeds[4] = { 0.5, 1.0, 2.0, 4.0 };
				int sel = choice->GetSelection();
				if (sel >= 0 && sel < 4)
					context->videoController->SetSpeed(speeds[sel]);
			});
			AddControl(speed_choice);
		}

		/// Toolbar button click handler
		void OnClick(wxCommandEvent &evt) {
			(*commands[evt.GetId() - TOOL_ID_BASE])(context);
		}

	public:
		void SetCommandVisible(std::string const& command_name, bool visible) {
			if (visible)
				hidden_commands.erase(command_name);
			else
				hidden_commands.insert(command_name);
			RegenerateToolbar();
		}

		/// Regenerate the toolbar when the icon size changes
		void OnIconSizeChange(agi::OptionValue const& opt) {
			icon_size = opt.GetInt();
			RegenerateToolbar();
		}

		/// Clear the toolbar and recreate it
		void RegenerateToolbar() {
			Unbind(wxEVT_IDLE, &Toolbar::OnIdle, this);
			ClearTools();
			commands.clear();
			Populate();
		}

		/// Populate the toolbar with buttons
		void Populate() {
			// Any control from a previous run was destroyed by ClearTools().
			speed_choice = nullptr;
			json::Object const& root = get_root();
			auto root_it = root.find(name);
			if (root_it == root.end()) {
				// Toolbar names are all hardcoded so this should never happen
				throw agi::InternalError("Toolbar named " + name + " not found.");
			}

			json::Array const& arr = root_it->second;
			commands.reserve(arr.size());
			bool needs_onidle = false;
			bool last_was_sep = false;

			for (json::String const& command_name : arr) {
				if (command_name.empty()) {
					if (!last_was_sep)
						AddSeparator();
					last_was_sep = true;
					continue;
				}

				cmd::Command *command;
				try {
					command = cmd::get(command_name);
				}
				catch (cmd::CommandNotFound const&) {
					LOG_W("toolbar/command/not_found") << "Command '" << command_name << "' not found; skipping";
					continue;
				}
				if (hidden_commands.count(command->name()))
					continue;

				last_was_sep = false;

				int flags = command->Type();
				wxItemKind kind =
					flags & cmd::COMMAND_RADIO ? wxITEM_RADIO :
					flags & cmd::COMMAND_TOGGLE ? wxITEM_CHECK :
					wxITEM_NORMAL;

				AddTool(TOOL_ID_BASE + commands.size(), command->StrDisplay(context), command->Icon(icon_size, GetLayoutDirection()), command->GetTooltip(ht_context), kind);

				commands.push_back(command);
				needs_onidle = needs_onidle || flags != cmd::COMMAND_NORMAL;

				// The play button gets a speed picker right after it
				if (name == "video" && command_name == "video/play")
					AddSpeedPicker();
			}

			// Only bind the update function if there are actually any dynamic tools
			if (needs_onidle) {
				Bind(wxEVT_IDLE, &Toolbar::OnIdle, this);
			}

			Realize();
		}

		Toolbar(wxWindow *parent, std::string name, agi::Context *c, std::string ht_context, bool vertical)
		: wxToolBar(parent, -1, wxDefaultPosition, wxDefaultSize, wxTB_NODIVIDER | wxTB_FLAT | (vertical ? wxTB_VERTICAL : wxTB_HORIZONTAL))
		, name(std::move(name))
		, context(c)
		, ht_context(std::move(ht_context))
		, icon_size(OPT_GET("App/Toolbar Icon Size")->GetInt())
		, icon_size_slot(OPT_SUB("App/Toolbar Icon Size", &Toolbar::OnIconSizeChange, this))
		, hotkeys_changed_slot(hotkey::inst->AddHotkeyChangeListener(&Toolbar::RegenerateToolbar, this))
		{
			Populate();
			Bind(wxEVT_TOOL, &Toolbar::OnClick, this);
			Bind(wxEVT_DPI_CHANGED, [this] (wxDPIChangedEvent &e) { RegenerateToolbar(); e.Skip(); });
		}

		Toolbar(wxFrame *parent, std::string name, agi::Context *c, std::string ht_context)
		: wxToolBar(parent, -1, wxDefaultPosition, wxDefaultSize, wxTB_FLAT | wxTB_HORIZONTAL)
		, name(std::move(name))
		, context(c)
		, ht_context(std::move(ht_context))
#ifndef __WXMAC__
		, icon_size(OPT_GET("App/Toolbar Icon Size")->GetInt())
		, icon_size_slot(OPT_SUB("App/Toolbar Icon Size", &Toolbar::OnIconSizeChange, this))
#else
		, icon_size(32)
#endif
		, hotkeys_changed_slot(hotkey::inst->AddHotkeyChangeListener(&Toolbar::RegenerateToolbar, this))
		{
			parent->SetToolBar(this);
			Populate();
			Bind(wxEVT_TOOL, &Toolbar::OnClick, this);
			Bind(wxEVT_DPI_CHANGED, [this] (wxDPIChangedEvent &e) { RegenerateToolbar(); e.Skip(); });
		}
	};
}

namespace toolbar {
	void AttachToolbar(wxFrame *frame, std::string const& name, agi::Context *c, std::string const& hotkey) {
		new Toolbar(frame, name, c, hotkey);
	}

	wxToolBar *GetToolbar(wxWindow *parent, std::string const& name, agi::Context *c, std::string const& hotkey, bool vertical) {
		return new Toolbar(parent, name, c, hotkey, vertical);
	}

	void SetCommandVisible(wxToolBar *toolbar, std::string const& command, bool visible) {
		if (auto dynamic_toolbar = dynamic_cast<Toolbar *>(toolbar))
			dynamic_toolbar->SetCommandVisible(command, visible);
	}
}
