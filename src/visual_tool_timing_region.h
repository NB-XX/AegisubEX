#pragma once

#include "visual_tool.h"

namespace agi { struct Context; }

/// Track the color of a picked subtitle pixel to match the line's visible interval.
class VisualToolTimingRegion final : public VisualToolBase {
	Vector2D start;
	Vector2D end;
	bool has_region = false;
	bool dragging = false;
	void DetectInterval();
	void ClearRegion();

public:
	VisualToolTimingRegion(VideoDisplay *parent, agi::Context *context);
	static bool HasPresetRegion();
	static void BatchMatchSelection(agi::Context *context);
	void OnMouseEvent(wxMouseEvent &event) override;
	bool OnKeyDown(wxKeyEvent &event) override;
	void Draw() override;
};
