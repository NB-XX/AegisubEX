# AegisubEX

基于 [TypesettingTools/Aegisub](https://github.com/TypesettingTools/Aegisub) 3.5.1 开发的扩展分支（独立版本序列 `v3.5.1-EX.x`），C++20 构建，保持 ASS 语法与原生渲染兼容。

---

## 功能模块

### 1. 视觉对帧 (Frame Match)

位置：视频工具栏（视觉对帧工具） / 视频菜单 / 快捷键 `Ctrl+Alt+D`

- **选区模式**：
  - 矩形框选：在选框内用 DBNet 提取紧致文字外接框。
  - 竖线锚点（拖拽宽度 `< 15px` 且高度 `>= 8px`）：以竖线中点与行高为基准向水平方向连通扩展，适配同样式不同字数行。
  - 选区坐标与模式标志（`Region Is Anchor`）持久化保存，拖拽单行、单键快对与批量对帧共用同一套裁剪逻辑。
- **检测引擎（3 种模式可切换）**：
  - 双模（默认）：优先通过 ONNX Runtime + DirectML（失败时回退 CPU）运行内建 `ch_PP-OCRv4_det.onnx` 定位文字区域，再进行极性掩模时序比对；模型不可用或特征不足时回退原生像素模式。
  - 纯深度学习检测：仅使用 DBNet 定位路径。
  - 原生双极性像素（Dice）：直接对区域内明暗极性掩模计算相似度。
- **前景极性判定与背景过滤**：
  - 对参考帧前后 `±6s` 采样剔除常驻静态像素（如固定字幕条）。
  - 某一极性面积占比 `> 50%` 且另一极性存在足够笔画密度时，自动锁定少数极性作为文字前景，避免白底黑字 / 黑底白字方框跟踪底板。
- **搜索策略**：
  - 多尺度金字塔步进（`500ms` 粗扫 -> `100ms` 中步 -> 单帧收敛），含单帧噪点容错。
- **单键快速对帧（`Ctrl+Alt+D`）**：
  - 复用已保存区域对当前选中行执行对帧，弹窗显示进度并在完成后自动关闭。
- **批量对帧**：
  - 选中行按原始时间排序后逐行处理，每行独立缓存并即时写回时间戳与进度，支持随时中止且保留已完成行。
  - 完成后按 `Style` 分组检查同样式相邻行重叠：单侧吞并时直接复用对方的准确内侧边界点，双向重合时按原交界对齐帧切分；不同样式的重叠不作干预。

---

### 2. 视频运动追踪 (Motion Tracking)

位置：视频工具栏（运动追踪工具） / 视频菜单

- **交互框选**：在视频画面拖拽定义目标 ROI 后自动打开追踪配置面板。
- **追踪算法**：
  - 粗细两阶段 NCC 模板匹配 + 亚像素抛物线拟合。
  - 初始模板与动态模板双模板融合，带模板更新置信度门禁，防止背景吸附与漂移。
  - 单帧位移搜索半径可调范围：`15–300px`。
- **位移应用与滤波**：
  - 支持相对位移（以指定基准帧的字幕位置为锚点叠加位移增量）与绝对跟随（字幕中心对齐追踪框中心）。
  - 支持移动加权平滑窗口滤波。
- **静止帧合并**：相邻帧位移 `<= 0.5px` 的连续帧合并为单条事件并延长时间区间。
- **列表折叠管理**：
  - 生成的逐帧字幕默认在网格中折叠为 1 行注释摘要（底层 `Comment=true`，不影响画面渲染），双击摘要行或点击行号列展开/收起。
  - 对折叠摘要行执行删除、复制、剪切时，自动级联作用于整组追踪帧。
- **关键帧导出**：按 Adobe After Effects 6.0 关键帧格式写入剪贴板，供 Mocha / Aegisub-Motion 等外部工具导入。

---

### 3. 审校模式与字幕网格 (Review Mode & Grid)

位置：主窗口审校开关 / 字幕表格列头 / 底部样式快切栏

- **变速不变调播放**：内建流式 WSOLA（`TimeStretchEngine`），支持 `0.5x` / `1x` / `2x` / `4x` 档位。
- **预览降采样（Smooth Preview）**：视频高于 720P 时输出 720P 预览帧以降低显存与拷贝开销，字幕脚本坐标分辨率保持原视频尺寸。
- **审校播放行为**：
  - `Ctrl+P` 切换审校播放/暂停，`Ctrl+Shift+P` 播放当前行 `±1s`。
  - 播放中切换选中行不中断播放；文本框获得焦点自动暂停，确认编辑后按配置续播或重播。
  - 表格高亮当前播放行与时间重叠行。
- **表格列头筛选与排序**：
  - 点击或右键 `Style` / `Actor` / `Effect` / `CPS` 列头弹出筛选菜单，按字段过滤显示行。
  - 支持按 CPS 与字符长度正/逆序排序。
- **样式快切面板（Quick Style）**：
  - 开启后数字键 `1`–`0` 直接切换当前行样式，支持拖拽调整前 10 个快捷槽位顺序。

---

### 4. 样式管理与文本编辑 (Styles & Editor)

位置：样式管理器 / 样式编辑器 / 字幕文本编辑框 / 字幕菜单

- **样式编辑器透明度微调**：主色、次色、边框色、阴影色各配独立 `0–255` Alpha 数值框，与拾色器按钮双向同步。
- **审校浮动格式条**：开启审校模式并在编辑框选中文本时弹出，提供加粗（B）、倾斜（I）、下划线（U）、字色、边色、重置局部代码及打开详细局部样式面板；对已含覆写标签的选区执行样式修改时合并覆写而非外层重复包裹。
- **多层描边与发光（Layer Styles）**：样式中支持配置最多 32 层描边、外发光与淡入淡出；保存与渲染时自动展开为标准 ASS 多行事件并写入折叠标记，重新打开时还原。
- **工程维护与体检**：
  - 未使用样式清理：扫描样式表及正文内 `{\r样式名}` 引用，移除未被引用的样式（支持 `Ctrl+Z` 撤销）。
  - 批量修改/清除选中行的覆写标签。
  - ASS 解析异常时弹出诊断列表，输出出错行号、原因与修改提示。

---

### 5. 音频人声切分与说话人分离 (Silero VAD v5)

位置：AI 助手 / 音频工具菜单（需存在 `silero_vad.onnx` 与 `onnxruntime.dll`）

- **语音端点检测**：基于 Silero VAD v5（ONNX Runtime / DirectML），`32ms` 流式分块推断；默认起始偏移 `0ms`，结尾延展 `200ms`。
- **非人声过滤**：提取 20 维 Mel 滤波器组、基频与自相关谐波度（共 24 维声学特征），按谐波度阈值过滤无周期结构的音效与背景噪声。
- **说话人分离（Diarization）**：
  - 已知角色匹配：从当前脚本已有 `Actor` 行提取 24 维特征质心，按余弦相似度归类并过滤低于阈值的片段。
  - 无监督聚类：无标注时使用 K-Means 聚类为 `Speaker 1, 2...`。
  - 支持追加至末尾或替换现有字幕，支持单步撤销。

---

### 6. 按字幕切分与合并视频 (Video Shuffler)

位置：视频菜单（需 `ffmpeg.exe` 位于程序目录或系统 `PATH`）

- **切分模式**：
  - 合并保留行：剔除空白间隔与注释行，提取所有对话行对应片段并拼接输出。
  - 按章节导出：识别形如 `Comment: # 章节名` 的注释行，按区间分段输出独立文件。
  - 仅导出选中行：提取当前选中字幕对应时间段。
- **输出编码**：
  - 流复制（`-c copy`）：不重编码。
  - 重编码：支持 `x264 + aac` 视频或 `wav` / `mp3` 纯音频。
- **字幕同步与边界保护**：
  - 同步生成时间轴重新对齐的 `_clip.ass`，可在完成后直接载入当前窗口。
  - 检测字幕越界（超出视频时长）并提示钳制；清洗输出文件名中的非法路径字符。

---

### 7. AI 辅助翻译与转写 (AI Assistant)

位置：AI 菜单 / 视频区域截图工具

- **区域截图 OCR 翻译**：在视频画面框选区域截屏，发送至视觉模型提取原文并翻译，可写回当前行。
- **选中行音频转写与翻译**：从内存 `AudioProvider` 直接切片送审，结果以表格展示序号、时间、原文、译文，支持多选写回、全量替换及 `Ctrl+C` 制表符格式（TSV）复制。
- **接口与网络配置**：兼容 OpenAI / 阿里云百炼 / Google Gemini 接口格式；支持多套配置切换、指定源语言、HTTP/SOCKS5 代理及超时退避重试。

---

## 快捷键

| 快捷键 | 功能 |
| :--- | :--- |
| `Ctrl + Alt + D` | 使用已保存区域对当前行快速对帧 |
| `Ctrl + P` | 审校播放 / 暂停（不改变选中行） |
| `Ctrl + Shift + P` | 单句预览（当前行 `±1s`） |
| `1` – `0` | 样式快切（需开启样式快切面板） |
| `↑` / `↓` | 切换上下行（暂停时联动跳转画面，播放中不中断） |
| 双击运动追踪摘要行 | 展开 / 折叠逐帧运动追踪字幕组 |
| `Delete` | 删除选中行（作用于折叠摘要行时级联删除整组帧） |
| `Ctrl + C` / `Ctrl + A` | 在 AI 音频转写结果表中复制（TSV） / 全选 |

---

## 从源码构建

### Windows

依赖：Visual Studio 2022（MSVC v143 + Windows SDK）、Python 3、`meson`、`ninja`（`pip install meson ninja`）、Git。

在 Visual Studio `x64 Native Tools Command Prompt` 中执行：

```powershell
git clone https://github.com/NB-XX/AegisubEX.git
cd AegisubEX
meson setup build-local -Ddefault_library=static --buildtype=release
python -m ninja -C build-local aegisub.exe
```

- 产物路径：`build-local\aegisub.exe`（或运行根目录 `打开本地新版Aegisub.cmd`）。
- 增量重编：`cmd /c build-local\rebuild.cmd`。
- 打包便携版压缩包：`powershell -File tools\package-release.ps1 -Version v3.5.1-EX.1`。

### macOS

```bash
brew install meson ninja cmake pkg-config libass boost zlib ffms2 fftw hunspell uchardet
export LDFLAGS="-L/usr/local/opt/icu4c/lib"
export CPPFLAGS="-I/usr/local/opt/icu4c/include"
export PKG_CONFIG_PATH="/usr/local/opt/icu4c/lib/pkgconfig"
# Apple Silicon 将 /usr/local 替换为 /opt/homebrew

meson setup build -Ddefault_library=static --buildtype=release
meson compile -C build
```

### Linux (Debian / Ubuntu)

```bash
sudo apt install build-essential pkg-config meson ninja-build gettext intltool \
  libfontconfig1-dev libass-dev libboost-all-dev zlib1g-dev wx3.2-headers libwxgtk3.2-dev \
  icu-devtools libicu-dev libpulse-dev libasound2-dev libopenal-dev libffms2-dev \
  libfftw3-dev libhunspell-dev libuchardet-dev libcurl4-gnutls-dev libgl1-mesa-dev

meson setup build --prefix=/usr/local --buildtype=release -Dsystem_luajit=false -Ddefault_library=static
meson compile -C build
sudo meson install -C build --skip-subprojects luajit
```

---

## 许可协议

- 源码基于 BSD 3-Clause 协议发布，见 [LICENCE](LICENCE)。
- 静态链接 `fftw3` 的预编译二进制发行版整体遵循 GPLv2。
- 上游项目：[TypesettingTools/Aegisub](https://github.com/TypesettingTools/Aegisub)。
