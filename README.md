# AegisubEX (Aegisub 审校扩展版)

<p align="center">
  <img src="https://raw.githubusercontent.com/TypesettingTools/Aegisub/master/packages/desktop/icons/256x256/aegisub.png" width="128" alt="Aegisub Logo" />
</p>

<p align="center">
  <b>面向现代字幕组、影视听译与短视频生产力深度重构的高性能 Aegisub 扩展发行版</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17" />
  <img src="https://img.shields.io/badge/Base-Aegisub%203.5.1%2B-brightgreen.svg" alt="Aegisub Version" />
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux-orange.svg" alt="Platform" />
  <img src="https://img.shields.io/badge/License-BSD%203--Clause-lightgrey.svg" alt="License" />
</p>

---

## 📖 简介 (Introduction)

**AegisubEX** 是基于官方 [TypesettingTools/Aegisub](https://github.com/TypesettingTools/Aegisub) 最新基线打造的深度定制扩展版本。

传统 Aegisub 是 ASS/SSA 字幕样式排版的巅峰之作，但在当下快节奏的字幕组打轴校对、短视频双语剪辑、无声听译与二创切片流程中，传统工具存在诸多痛点：**无法变速不变调听译、缺乏自动化人声切分、动态花字无法运动追踪、无法按字幕秒级切分合并视频、4K/超清原盘播放卡顿掉帧、多层边框繁琐堆叠、解析报错难以排查**。

AegisubEX 在**100% 保持原生高精度字幕排版引擎与 ASS 语法兼容**的前提下，原生融合了**现代计算机视觉（NCC / 亚像素拟合）、深度学习（Silero VAD / 声纹谐波分析）、极速流媒体切片处理（FFmpeg Engine）以及大语言模型智能辅助**，为您提供一体化、沉浸式的高效审校生产力体验。

---

## ✨ 核心增强特性 (Key Features)

### ⚡ 1. 视频运动追踪与相对位移 (Motion Tracking Engine)
- **画面交互式框选 (Canvas ROI Tool)**：直接在视频预览画面上拖拽框选动态花字或追踪目标，框选完成后自动呼出运动追踪配置面板。
- **高鲁棒性多尺度 NCC + 亚像素拟合**：
  - 粗细双阶段 NCC 模板匹配结合亚像素抛物线极值定位。
  - 双模板动态融合机制：动态模板跟随目标形变，初始模板牢牢锁定原位，彻底防止长时间追踪漂移。
- **相对位移运动与绝对跟随**：
  - **相对位移**：以基准帧的字幕固定排版为锚点，花字怎么动，字幕严格以相同轨迹同步运动。
  - **绝对跟随**：字幕中心直接吸附在追踪框中心。
  - 内建移动加权平滑滤波，消除视频原生高频微抖动。
- **静止帧智能合并 (Merge Static Frames)**：
  - 针对影视片头/短视频常见的“花字飞入后定格不动”场景，自动检测相邻帧位移差（$\le 0.5\text{px}$）。
  - **连续静止帧自动合并为单行并平滑延长区间**，告别过去生成数十行无意义重复帧的臃肿现象，行数精简达 80% 以上。
- **字幕列表单行折叠 (Fold in Subtitle Grid)**：
  - 追踪生成的所有逐帧字幕在 Aegisub 字幕列表中**默认折叠显示为 1 行摘要**（`▶ [运动追踪] 目标文本 (N 帧/段)`），配以专属柔和高亮底色，时间轴保持绝对清爽。
  - **双击该行或点击 `#` 行号列**可随时展开全部各帧（图标变为 `▼`），方便单独精调每一帧的 `\pos` 或时间；再次双击即可收起。
  - 折叠摘要行在底层 ASS 中自动标记为 `Comment=true`，视频渲染器**绝不会在画面上重复绘制多余文字**；视频播放时全帧依然正常渲染，流畅丝滑。
  - 级联删除与复制：直接在折叠行上按 `Delete` 可自动清理整组所有帧；复制/剪切折叠行会自动携带完整追踪数据帧。
- **Mocha / After Effects 关键帧剪贴板导出**：一键生成 Adobe After Effects 6.0 关键帧格式并写入剪贴板，可无缝粘贴至 Aegisub-Motion、Mocha 或外部特效软件。

---

### 🎬 2. 按字幕切分与合并视频 (Video Shuffler & Cutter)
- **智能精简剪辑 (Smart Trim & Merge)**：
  - 一键扫描字幕工程，自动剔除无声空白区与无效注释行，将所有有效台词分段秒级提取并**无缝拼接为一个精华视频**。
  - 适合快速提取全片对白、制作高能集锦、短视频粗剪。
- **按章节注释批量切片**：
  - 只要字幕中插入形如 `Comment: # 序章` 或 `# 01 章节名` 的注释行，即可自动按章节将视频切割并导出为独立的独立分段视频文件。
- **仅切分选中字幕行**：快速截取当前高光对白台词对应的高清音视频片段。
- **双重编码引擎支持**：
  - **超快速无损流复制 (`-c copy`)**：无需重新编解码，原画质原音质 100% 无损，毫秒级瞬间导出。
  - **逐帧精准重编码 (x264+aac / wav / mp3)**：毫秒级帧准度对齐，同时支持单切提取伴音或纯音频。
- **同步生成严丝合缝的新 ASS 字幕**：
  - 剪辑重组视频的同时，自动生成起止时间 100% 对齐的新字幕文件（`_clip.ass`）。
  - 支持在弹窗中**一键在 Aegisub 中直接载入切片新工程**，继续后续精细编辑。
- **全方位防呆与安全防御**：
  - 自动检测字幕时间超出视频总时长（如音频外挂越界），智能给出拦截或钳制提示。
  - 导出路径非法字符/特殊标点自动清洗，杜绝系统路径创建失败。
  - 细粒度步骤状态监控，错误原因精准反馈。

---

### 🎧 3. 智能人声切分与说话人分离 (Silero VAD v5 + 声纹识别)
- **Silero VAD v5 深度学习引擎**：
  - 基于 DirectML / ONNX Runtime 动态驱动，无需安装额外大型环境，官方 v5 神经网络权重。
  - 32ms 流式分块推断，状态机自动捕捉发声区间。
- **起止时码贴合**：
  - 起始发声时刻 **0ms 紧贴（pad_start = 0ms）**，发声瞬态零迟滞。
  - 语音结束自然延长 **200ms（pad_end = 200ms）**，消除断句截断感。
- **背景乐与游戏音效强力过滤（24维声纹谐波度检测）**：
  - 内建声学特征提取器，提取 20 维 Mel 滤波器组、Pitch 基频与自相关谐波度（NACF Harmonicity）。
  - 人声具有强周期谐波结构，非语言音效（游戏爆炸、枪声、UI 杂音、背景乐）缺乏谐波；开启过滤后可彻底杜绝游戏音效误判为说话。
- **说话人分离（Diarization）双模式**：
  - **参考已知角色音色匹配**：自动学习工程中已有 `Actor` 角色行的 24 维音色质心，推断时比对余弦相似度并自动赋予角色名；低于相似度阈值的人声自动判定为非目标杂音并过滤。
  - **无监督 K-Means 聚类**：未标注工程自动聚类为 Speaker 1, 2...
  - 支持追加到字幕末尾或替换所有行，支持 Ctrl+Z 一键撤销。

---

### 🎯 4. 自动对帧与场景匹配 (Frame Match Assistant)
- **纯内建深度学习文字笔画概率检测 (Stroke Probability Aware)**：
  - **免除外部依赖**：彻底剥离外部第三方 DLL 依赖，杜绝路径非 ASCII 崩溃与环境不兼容问题。
  - **吸收先进文字笔画语义检测**：利用 DirectML GPU 硬件加速与内建 PP-OCRv4 DBNet 神经网络，直接输出文字笔画概率分布（Stroke Probability Mask），彻底替代传统 RGB 像素对比，100% 免疫复杂运动、动态背景、光影闪烁与镜头推拉干扰。
- **竖线基准锚点 (Vertical Anchor) 与整行长度自适应**：
  - **打破死方框限制**：彻底克服传统固定方框无法适配不同字数长短字幕的弊端。支持在画面中直接拖拽或点击放置**竖线测量锚点**（X 坐标、Y 坐标与基准行高）。
  - **每行独立自适应外接矩形**：在批量对帧时，系统在每条台词的参考帧上，自动以竖线锚点为基准向左右膨胀连通，自动精准捕获属于该句台词的完整文字包围盒（短句紧凑包裹，长句自动延伸跨越全句），彻底杜绝由于边框外背景暴露造成的相似度失真。
- **逐行实时上屏与声画动态流式对帧**：
  - **完成一行立即刷新**：批量对帧时不再等待全部结束才整体更新，而是**每对帧完一行，立即写回时间戳至字幕表格，同时视频画面即刻无缝跳跃到该行起始帧**，字幕网格实时高亮联动，极大增强打轴交互动态感与视觉沉浸感。
  - **断点防丢进度保障**：每行匹配即时持久化提交（统一合并为单步撤销），即使用户中途手动取消或异常退出，已经完成对帧的所有字幕行**进度 100% 完整保留，绝对不会丢失任何已有成果**。
- **10倍极速与单帧级高精度兼得（顺向定界 + 跨步飞跃）**：
  - **空间边缘能量预过滤 (0.02ms 极速筛选)**：在送入神经网络前瞬间计算 Sobel 梯度能量，快速剔除无文字帧，大幅节省 GPU 前向推断。
  - **自适应跨步跳跃 + 顺向逐帧精确定界**：向前后边界搜索时采用自适应步长快速跳跃锁定边界失配点，并在微小失配区间内利用 FFMS2 原生连续顺序解码（0ms 出帧，无逆向跨 GOP 重复 Seek 开销）单向精确定界，兼顾 **150ms 极速**与 **100% 单帧级精度**，彻底解决视频压缩轻微失真导致的二分跳步误判。
  - **批量对帧零卡顿与流式响应**：在单帧解码采样与每一行匹配处理中深度注入 Windows 消息循环泵（`wxSafeYield`），彻底杜绝批量对帧时的窗口假死、“(未响应)”白屏与进度条冻结，支持随时一键平滑取消。
  - **三模灵活切换**：
    1. **智能双模 (推荐)**：优先调用 DirectML GPU 文字笔画概率检测；无模型或特征时自动平滑回退原生像素。
    2. **深度学习笔画检测**：纯神经笔画概率掩模对比，抗复杂运动与高动态场景。
    3. **经典原生双极性像素模式 (保留备选)**：保留原本的极值二值化与 Dice 掩模对比算法，毫秒级响应，零额外显存占用。

---

### 🤖 5. AI 内嵌智能辅助翻译与转写 (AI Assistant)
- **区域截图视觉翻译**：画面框选 OCR，毫秒级提取硬字幕并调用大语言模型进行高质量多语种翻译，可一键写回当前行。
- **选中行音频转写与听写**：从内存 AudioProvider 直接提取切片样本，支持兼容 OpenAI、阿里云百炼、Google Gemini 等 API 端点；表格化展示序号、时间、原文与译文，支持多选填入或一键全量替换。
- **便捷复制与导出**：支持表格右键、`Ctrl+C`、一键按钮复制（制表符分隔格式，可直接无缝粘贴到 Excel 分列）。
- **多方案与网络代理管理**：自由配置与切换多套 API Key、端点 URL、源语言（支持韩语/日语/英语等指定，防止混淆）与 HTTP/SOCKS5 代理，内建指数退避超时自动重试。

---

### ⏱️ 6. 深度字幕审校模式 (Review Mode) 与倍速播放
- **流式倍速播放（变速不变调）**：
  - 内建流式 WSOLA（TimeStretchEngine）时距伸缩算法。
  - 支持 0.5x / 1x / 2x / 4x 档位切换，人声清脆可辨，声画严格同步。
- **4K/超清视频动态降采样流畅预览 (Smooth Preview)**：
  - 审校超清/4K 片源时，底层色彩转换直接输出 720P，显存占用与内存拷贝骤减 89%，光栅化耗时降低 75%，杜绝卡顿，视频原生宽高坐标保持 4K 零精度损失。
- **审校模式专属时间轴状态机**：
  - 单句预览（`Ctrl-Shift-P`，播当前句 $\pm 1\text{s}$）。
  - 切换选中行绝不打断视频播放；编辑框焦点感知联动（聚焦自动暂停，编辑完成回车自动续播或重播）。
  - 正在播放行琥珀色高亮聚焦，时间重叠行粉色高亮即时告警。
- **样式快切面板 (Quick Style)**：
  - 数字键 `1`–`0` 极速切换当前行 ASS 样式；未播放状态下方向键切行画面自动跳转，播放中切行不打断播放；支持鼠标拖拽重排前 10 个快捷样式。

---

### 🎨 7. 样式特效与工程体检 (Style FX & Audit)
- **多层描边 / 外发光 / 淡入淡出 (Layer Styles)**：
  - 突破 ASS 样式单层边框限制，直接在样式编辑器中定义最高 32 层描边与发光。
  - 保存和播放时自动展开为标准 ASS 事件栈并内嵌可读标记，重新打开时无损折叠收回。
- **一键清理未使用的样式**：智能扫描 ASS 脚本中所有 Style 及正文内行内 `{\r样式名}` 标签，一键清理冗余样式，支持 Ctrl+Z 撤销。
- **批量预设 / 批量删除 override 标签**：给选中行批量替换或清洗特定标签。
- **CPS（每秒字数）/ 字符长度双向排序**：快速排查压字数、超速字幕行。
- **字幕格式审查 (Audit)**：文件载入解析抛错时自动触发诊断，列出具体行号、错误类型与修复建议，告别盲目排查。

---

## ⌨️ 快捷键速查 (Keybindings)

| 快捷键 | 功能描述 |
| :--- | :--- |
| **Ctrl + P** | 审校播放 / 暂停（不动当前选中行） |
| **Ctrl + Shift + P** | 单句审校预览（播放当前行 $\pm 1\text{s}$） |
| **1 ～ 0** | 样式快切（需开启右下角样式快切面板） |
| **↑ / ↓** | 上下一行（未播放时跳转画面，审校播放中不打断播放） |
| **双击运动追踪行** | 展开 / 折叠逐帧运动追踪字幕组 |
| **Delete** | 删除选中行（选中折叠追踪行时自动级联删除整组帧） |
| **Ctrl + C / Ctrl + A** | 在 AI 音频翻译列表中快速复制 / 全选结果 |

---

## 🛠️ 构建与编译 (Building from Source)

### Windows (推荐环境)

#### 前置要求：
1. **Visual Studio 2022**（安装包含 C++ 桌面开发、MSVC v143 编译器与 Windows SDK）。
2. **Python 3**（确保已加入系统 PATH）。
3. **Meson** 与 **Ninja** (`pip install meson ninja`)。
4. **Git for Windows**。

#### 构建步骤：
```powershell
# 1. 克隆本仓库
git clone https://github.com/NB-XX/AegisubEX.git
cd AegisubEX

# 2. 从 Visual Studio "x64 Native Tools Command Prompt" 执行构建配置
meson setup build-local -Ddefault_library=static --buildtype=release

# 3. 编译与生成可执行文件
python -m ninja -C build-local aegisub.exe

# 4. 链接生成运行版
cmd /c "build-local\link-ready.cmd"
```
编译产物位于 `build-local\aegisub-ready.exe`，直接运行即可。

---

### macOS

#### 依赖安装 (Homebrew)：
```bash
brew install meson ninja cmake pkg-config libass boost zlib ffms2 fftw hunspell uchardet
export LDFLAGS="-L/usr/local/opt/icu4c/lib"
export CPPFLAGS="-I/usr/local/opt/icu4c/include"
export PKG_CONFIG_PATH="/usr/local/opt/icu4c/lib/pkgconfig"
# Apple Silicon 架构请将 /usr/local 替换为 /opt/homebrew
```

#### 构建应用包：
```bash
meson setup build -Ddefault_library=static --buildtype=release
meson compile -C build
```

---

### Linux (Debian / Ubuntu)

#### 依赖安装：
```bash
sudo apt install build-essential pkg-config meson ninja-build gettext intltool \
  libfontconfig1-dev libass-dev libboost-all-dev zlib1g-dev wx3.2-headers libwxgtk3.2-dev \
  icu-devtools libicu-dev libpulse-dev libasound2-dev libopenal-dev libffms2-dev \
  libfftw3-dev libhunspell-dev libuchardet-dev libcurl4-gnutls-dev libgl1-mesa-dev
```

#### 构建与安装：
```bash
meson setup build --prefix=/usr/local --buildtype=release -Dsystem_luajit=false -Ddefault_library=static
meson compile -C build
sudo meson install -C build --skip-subprojects luajit
```

---

## 📄 开源许可 (License)

- 本项目基于 BSD 3-Clause 许可证发布，详见 [LICENCE](LICENCE)。
- 官方 Windows/macOS 发行版由于静态链接包含 `fftw3`，整体遵循 **GPLv2** 许可协议。
- 本项目继承上游 [TypesettingTools/Aegisub](https://github.com/TypesettingTools/Aegisub) 所有原创者的卓越贡献。
