script_name = "去除间隙并拼接（增强版）"
script_description = "按阈值去除间隙，支持忽略重叠 + 最大拉伸限制"
script_author = "ChatGPT"
script_version = "1.1"

function remove_gaps(subs, sel)
    if #sel < 2 then
        aegisub.debug.out("请至少选择两行\n")
        return
    end

    -- 配置窗口
    local config = {
        {class="label", label="阈值(ms):", x=0, y=0},
        {class="intedit", name="threshold", value=500, x=1, y=0},

        {class="label", label="最大拉伸(ms):", x=0, y=1},
        {class="intedit", name="max_extend", value=5000, x=1, y=1},

        {class="label", label="模式:", x=0, y=2},
        {class="dropdown", name="mode", items={"头提前", "尾延后"}, value="头提前", x=1, y=2}
    }

    local btn, res = aegisub.dialog.display(config, {"执行", "取消"})
    if btn ~= "执行" then return end

    local threshold = res.threshold
    local max_extend = res.max_extend
    local mode = res.mode

    -- 收集选中行
    local lines = {}
    for _, li in ipairs(sel) do
        table.insert(lines, {index=li, line=subs[li]})
    end

    -- 按开始时间排序
    table.sort(lines, function(a, b)
        return a.line.start_time < b.line.start_time
    end)

    -- 处理
    for i = 1, #lines - 1 do
        local cur = lines[i].line
        local nxt = lines[i + 1].line

        local gap = nxt.start_time - cur.end_time

        -- ❗忽略 overlap（gap <= 0）
        if gap > 0 and gap <= threshold then

            if mode == "头提前" then
                local extend = gap  -- nxt.start 往前拉的量

                if extend <= max_extend then
                    nxt.start_time = cur.end_time
                    subs[lines[i + 1].index] = nxt
                end

            elseif mode == "尾延后" then
                local extend = gap  -- cur.end 往后拉的量

                if extend <= max_extend then
                    cur.end_time = nxt.start_time
                    subs[lines[i].index] = cur
                end
            end

        end
    end

    aegisub.set_undo_point("去除间隙拼接（增强）")
end

aegisub.register_macro(script_name, script_description, remove_gaps)