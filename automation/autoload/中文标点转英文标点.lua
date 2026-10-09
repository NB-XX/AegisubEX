script_name = "中文标点转英文标点"
script_description = "将字幕中的全角/中文标点转换为半角英文标点，不影响汉字，可选处理范围和替换类别"
script_author = "Aegisub审校版"
script_version = "1.0"

-- 解码 text 中第 i 字节起的 UTF-8 码点，返回码点和字节长度
local function decode_cp(s, i)
	local b = s:byte(i)
	if not b then return nil, 0 end
	if b < 0x80 then return b, 1 end
	if b >= 0xC0 and b < 0xE0 then
		local b2 = s:byte(i+1)
		if not b2 then return nil, 0 end
		return (b % 32) * 64 + (b2 % 64), 2
	end
	if b >= 0xE0 and b < 0xF0 then
		local b2, b3 = s:byte(i+1, i+2)
		if not b2 or not b3 then return nil, 0 end
		return ((b % 16) * 64 + (b2 % 64)) * 64 + (b3 % 64), 3
	end
	if b >= 0xF0 then
		local b2, b3, b4 = s:byte(i+1, i+3)
		if not b2 or not b3 or not b4 then return nil, 0 end
		return (((b % 8) * 64 + (b2 % 64)) * 64 + (b3 % 64)) * 64 + (b4 % 64), 4
	end
	return nil, 0
end

-- 固定替换：全角空格、顿号、句号、弯引号、浪纹线
local FIXED = {
	[0x3000] = " ",
	[0x3001] = ",",       -- 、
	[0x3002] = ".",       -- 。
	[0x2018] = "'",       -- ‘
	[0x2019] = "'",       -- ’
	[0x201C] = '"',       -- “
	[0x201D] = '"',       -- ”
	[0x301C] = "~",       -- 〜
}

-- 可选替换（按对话框开关启用）
local OPTIONAL = {
	book   = { [0x300A] = "<", [0x300B] = ">" },                    -- 《》→<>
	bracket= { [0x3010] = "[", [0x3011] = "]" },                    -- 【】→[]
	cnquote= { [0x300C] = '"', [0x300D] = '"',                      -- 「」""
	           [0x300E] = "'", [0x300F] = "'" },                    -- 『』''
	dash   = { [0x2014] = "--", [0x2015] = "--", [0x2013] = "-" },  -- ——―–
}

local function map_cp(cp, opt)
	-- FF01–FF5E：全角ASCII变体（，！？：；（）～等）。字母数字保持全角不动
	if cp >= 0xFF01 and cp <= 0xFF5E then
		local h = cp - 0xFEE0
		if h >= 0x30 and h <= 0x39 then return nil end   -- 全角数字
		if h >= 0x41 and h <= 0x5A then return nil end   -- 全角大写字母
		if h >= 0x61 and h <= 0x7A then return nil end   -- 全角小写字母
		return string.char(h)
	end
	if FIXED[cp] then return FIXED[cp] end
	if cp == 0x2026 then return opt.ellipsis and "..." or nil end  -- …
	if cp == 0x2025 then return opt.ellipsis and ".." or nil end   -- ‥
	if opt.book    then local r = OPTIONAL.book[cp];    if r then return r end end
	if opt.bracket then local r = OPTIONAL.bracket[cp]; if r then return r end end
	if opt.cnquote then local r = OPTIONAL.cnquote[cp]; if r then return r end end
	if opt.dash    then local r = OPTIONAL.dash[cp];    if r then return r end end
	return nil
end

-- 转换一行文本；跳过 {\...} 覆盖标签块，只动可见文本部分的标点
local function convert_text(text, opt)
	local out = {}
	local n = #text
	local i = 1
	local in_tag = false
	local touched = false
	while i <= n do
		local b = text:byte(i)
		if b == 0x7B then -- {
			in_tag = true
			out[#out+1] = "{"
			i = i + 1
		elseif b == 0x7D then -- }
			in_tag = false
			out[#out+1] = "}"
			i = i + 1
		elseif in_tag then
			out[#out+1] = text:sub(i, i)
			i = i + 1
		else
			local cp, len = decode_cp(text, i)
			if not cp then
				-- 无法识别的字节（畸形编码）：原样复制并前进一字节，避免死循环
				out[#out+1] = text:sub(i, i)
				i = i + 1
			else
				local rep = map_cp(cp, opt)
				if rep then
					out[#out+1] = rep
					touched = true
					i = i + len
				else
					out[#out+1] = text:sub(i, i + len - 1)
					i = i + len
				end
			end
		end
	end
	return table.concat(out), touched
end

function convert_punct(subs, sel)
	local config = {
		{class = "label", x = 0, y = 0, label = "转换范围:"},
		{class = "dropdown", x = 1, y = 0, name = "scope", items = {"选中行", "全部行"}, value = "选中行", width = 2},

		{class = "checkbox", x = 0, y = 1, name = "comment", label = "连注释行一起转换", value = false, span = 2},
		{class = "checkbox", x = 0, y = 2, name = "book", label = "《》 → <>", value = true, span = 1},
		{class = "checkbox", x = 1, y = 2, name = "bracket", label = "【】 → []", value = true, span = 1},
		{class = "checkbox", x = 2, y = 2, name = "cnquote", label = "「」『』 → \" \" ' '", value = false, span = 1},
		{class = "checkbox", x = 0, y = 3, name = "dash", label = "破折号 —— → --", value = false, span = 2},
		{class = "checkbox", x = 2, y = 3, name = "ellipsis", label = "省略号 … → ...", value = true, span = 1},
	}
	local buttons = {"执行", "取消"}
	local btn, res = aegisub.dialog.display(config, buttons)
	if btn ~= "执行" then return end

	local opt = {
		book = res.book,
		bracket = res.bracket,
		cnquote = res.cnquote,
		dash = res.dash,
		ellipsis = res.ellipsis,
	}

	-- 确定要处理的行号
	local lines
	if res.scope == "全部行" then
		lines = {}
		for i = 1, #subs do lines[#lines+1] = i end
	else
		lines = sel
	end

	local changed = 0
	for _, index in ipairs(lines) do
		local line = subs[index]
		if line.class == "dialogue" or (res.comment and line.class == "comment") then
			local newtext, touched = convert_text(line.text, opt)
			if touched then
				line.text = newtext
				subs[index] = line
				changed = changed + 1
			end
		end
	end

	if changed > 0 then
		aegisub.set_undo_point(script_name)
	end
	aegisub.debug.out(string.format("中文标点转换完成：共修改 %d 行\n", changed))
end

aegisub.register_macro(script_name, script_description, convert_punct)
