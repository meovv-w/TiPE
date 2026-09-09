# TiPE 验收清单

本清单依据用户粘贴的完整产品要求，不把“能编译、能显示几个候选”当作完成。检查基线为 2026-09-09 当前工作区；其他代理继续实现后，应依据新的运行证据更新状态。`[x]` 仅表示列出的窄范围证据已观察到，未勾选项包括未实现和未验证项目。

## 已观察到的证据

- [x] 项目包含 C++20 核心、fcitx5 插件、Qt 监督窗口、演示、诊断、候选网格布局模块、构建文件、脚本和中文文档。
- [x] 已配置构建和 CTest 目标，覆盖 LibIME 固定语料质量、控制中心支持页及 HiDPI、核心状态机、候选网格、Pango/Cairo 候选绘制、纠错规则、学习持久化、模型状态、LocalSmall 候选表、资源压力、独立 fcitx5 testfrontend 集成、X11 UI smoke、分页偏移、无显示 fallback、Wayland 公开连接能力诊断、协议画像和显式同意的 loopback HTTP 云端传输；当前工作区已重新运行并记录为 20/20 通过。
- [x] 集成测试在独立 fcitx5 实例中确认加载 TiPE，并实际收到 `nihao`→`你好`、`github` 原样提交，以及通过真实候选回调分段选择 `woc`→`我`→`操`；同时覆盖 key-up 事件。
- [x] `tipe-protocol-tests` 在独立 testfrontend 中按 GTK、Qt、终端能力标志分别创建输入上下文，设置 text rectangle，并验证 key down/up 与 `commitString`；`scripts/run-protocol-regression.sh` 记录运行时版本和可复现日志。这是协议回归，不等同真实窗口视觉测试。
- [x] 现有核心测试覆盖：长拼音部分选择、删除恢复、分页、Shift+Tab、标点、英文切换、敏感事件过滤和提交事件进入学习器。它没有覆盖本清单的其他验收项。
- [x] 核心与 Fcitx 回归还验证了 UTF-8 安全的 preedit 光标：左右/Home/End 移动、中间插入以及光标处 Backspace/Delete；这不等同真实应用的跨 chunk surrounding-text 编辑验收。
- [x] `tipe-diagnose` 使用 LibIME 对 1000 次整段 `nihao`（并记录 5000 个单键样本）输出 p50/p95/p99，同时输出 CPU、峰值 RSS、队列和写入计数。这是核心热路径样本，不等同所有真实应用的长期后台资源上限。
- [x] 已加入可重复的 GTK4/Qt6 真实 toolkit harness：正常窗口通过 GTK/Qt 输入法上下文记录 preedit、commit、surrounding-text、光标查询和文本矩形，并记录本机 GTK/Qt/foot 版本及当前输入法；普通脚本仍尊重当前会话（本机当前是 `keyboard-us`，所以那次记录只有 toolkit ready/矩形证据），新增 `run-isolated-real-app-harness.sh` 则在临时 XDG profile、独立 D-Bus 和嵌套 Xwayland 中加载 TiPE，已实测 GTK4 与 Qt6 都提交 `你好`。两种 harness 都不修改用户 profile、不重启现有桌面 fcitx，也不替代真实桌面视觉验收。
- [x] `run-real-app-edit-regression.sh` 在隔离 GTK4/Qt6 窗口中执行两段中文提交、客户端 Left+Backspace、英文 `github` 和再次中文提交；两种 toolkit 的最终 text 事件都观察到 `你好界github你好`，并保留了对应 cursor/query 记录。这是窄范围真实编辑证据，不替代其他前端或真实桌面回归。
- [x] 隔离真实应用脚本现在额外探测 Chromium 输入框和 foot 终端，并分别写入 `chromium-smoke.log`/`foot-smoke.log`；每次运行记录版本、窗口发现和 `commit-ok`/`commit-not-observed`/明确跳过原因。最近一次隔离运行中 GTK4/Qt6 提交 `你好`，Chromium 明确记录 `commit-not-observed`，foot 因嵌套 Xwayland 没有 Wayland socket 而记录 `no-Wayland-display-for-foot`，没有把缺失证据标为通过。`TIPE_REAL_APP_STRICT_APPS=1` 可将已找到窗口但未观察到提交设为失败。
- [x] 学习测试覆盖两次自动原子发布后回滚上一版，并确认内存快照与回滚文件一致。
- [x] 已存在 2048 上限的事件队列和后台线程，以及暂停、清空、导出接口；`tipe-learning-tests` 覆盖多个 Learner 实例的状态观察、并发控制、清空竞态和导出权限，并用独立 writer/control 子进程通过共享 `control.current` 验证暂停时的批次回滚、恢复后的继续学习；另有 writer 子进程突然退出后重新打开模型的恢复回归。该证据仍不等同断电或每个 fsync/rename 窗口的完整证明。
- [x] Engine 的提交恢复历史有硬上限：最多保留最近 1024 个完整 chunk，且 text/pinyin 总负载不超过 64 KiB；超过单 token 上限的提交不进入恢复历史。核心 stress 测试写入 1100 个 chunk 后只恢复最近 1024 个，并验证 70 KiB 单 token 不会留下陈旧历史。

## 基础解码、状态机与英文

- [x] 首选由系统 LibIME 词典、语言模型和词频生成；无 LibIME 时才使用受限内置回退词表。个人精确偏好单独置顶，不改写基础排序。
- [x] 覆盖长拼音整句、逐字和分段选择；核心与 Fcitx 回调测试确认选择前半段后保留后缀拼音，并验证展开候选刷新不会把前缀选择误提交成旧索引。
- [x] 最近一次选择或提交的汉字 chunk，在 surrounding text 精确匹配时可以恢复为可编辑拼音；Fcitx 集成回归还覆盖了两个连续中文 chunk 只恢复最后一段。不匹配时清除历史并让 Backspace 穿透，协议不足时也会穿透并清除历史，之后重新提供匹配 snapshot 仍 fail-closed；无 composition 的 Delete 也会穿透并清除恢复历史，随后匹配 snapshot 的 Backspace 仍 fail-closed。核心层同样在 Delete 和真实光标移动后清除恢复后缀。任意真实编辑光标和跨 chunk 删除仍缺真实应用证据。
- [x] 核心测试覆盖非词字母串/极长输入保持可编辑、候选为空、连续删除和模式切换；这只是有限状态机证据，不等于所有乱拼音、协议边界或真实应用长时间回归。
- [ ] Backspace/Delete、Enter/Esc、Tab/Shift+Tab、Home/End、数字、标点、方向键的完整产品行为仍未验收。核心状态机现在具备 UTF-8 边界安全的独立 preedit 光标，支持中间插入、前后删除和光标保留，且有核心/Fcitx 回归；Fcitx testfrontend 已覆盖 Delete 穿透后的恢复历史失效。新增 `build/isolated-edit-final-*` 在真实 GTK4/Qt6 窗口中观察到两段中文提交、Left+Backspace 删除、英文 `github` 和再次中文提交，但这仍不是所有按键、多应用和协议 fallback 的完整验收。
- [ ] 中文模式实现了英文/代码/网址/路径原文启发式；核心 smoke corpus 现在覆盖约 50 个常见英文/代码词、软件名、大小写、`https://`、带端口和数字路径、`www.`、域名及绝对/相对路径，并同时检查多组普通拼音不被误判，但这仍不是大规模误判率评估。
- [x] 英文模式直接输入不显示中文候选；Fcitx 集成测试验证候选列表清除和 key-up 穿透。
- [x] 拼音中途切英文又切回可恢复原组合；开始实际英文输入时会先提交挂起拼音，模式切换在第一次 Shift tap 即生效。
- [ ] 从其他输入法进入 TiPE 的模式与焦点生命周期稳定，适用于浏览器、终端、Discord、Wine 等协议路径。
- [ ] 已提交文本必须在真实插入光标处可安全编辑：不能只依赖“最后一个 chunk 紧邻光标”的特殊情况，也要覆盖多段中文/英文混合、光标移动后 Backspace/Delete 和 surrounding text 不可用的 fallback。隔离 GTK4/Qt6 edit scenario 已直接观察 `你好世界` 经光标移动删除为 `你好界`，再插入 `github`/`你好`；真实桌面其他前端与 surrounding text 不可用路径仍待验证。

## 候选窗口与光标

- [ ] 默认紧凑横向一行；方向键后展开多列、多行网格，而非仅改变纵向列表提示。`candidate_painter` 已完成 Pango/Cairo 字体测量、HiDPI 绘制、选中高亮、长文本省略和 hit-test，并有 `tipe-painter-tests`；`tipe-ui` 已接入可选 X11/XWayland override-redirect popup，但 Wayland 原生 popup 和真实桌面视觉仍未验收。
- [x] 展开后候选编号使用 1..9、0，超过 10 项可连续翻页和选择；核心测试覆盖第二页游标和 0 键映射。独立 `candidate_grid`/`candidate_painter` 已测试动态行列、实际字体宽度、命中、HiDPI 和边界导航；`tipe-ui-page-offset-smoke` 还验证第二页的公共 `BulkCursor` 全局选中项与 painter 的 `begin` 偏移保持一致，避免分页后高亮和鼠标命中落到第一页。标准 CommonCandidateList 仍是默认 fallback，启用 `TIPE_UI_X11=1` 时可选 `tipe-ui` addon 会绘制 X11/XWayland override-redirect popup，并已通过 DISPLAY=:0 的连接/加载/卸载 smoke，但尚无真实桌面视觉证据，Wayland popup 也未实现。
- [ ] 上下左右按行列移动；核心展开状态的 Up/Down 已接入 `candidate_grid` 行列导航，Left/Right 保留线性兼容路径；候选窗口真实行列命中和左键返回最左候选列仍需自绘 UI 验证。
- [ ] 长候选拥有合理宽度，短候选间距紧凑；无多余空白、异常宽度、模糊或低分辨率绘制。
- [ ] 使用 text rectangle / popup surface / compositor anchor 跟随真实插入光标；X11/XWayland 路径已实现 cursorRect 定位和屏幕边缘避让，并在 DISPLAY=:0 smoke 中通过加载/卸载；Wayland 原生 surface、真实应用光标跟随仍需验证。`tipe-ui` 会对公开 `wl_display` 做只读 registry 能力探测并记录 `wayland_global_probe` 与 input-method/input-panel/popup global 字段（包括 v2 manager），并写出 `wayland_popup_reason:public-api-no-popup-factory`；由于 Fcitx 公共 API 没有 popup-surface 工厂，仍明确回退标准 InputPanel，不把 global 存在误报为已实现的原生 popup。
- [ ] 首次应用切换不会短暂飘到错误位置；缺少协议能力时提供可诊断 fallback，不猜测或轮询 niri 窗口坐标。
- [ ] 在终端、浏览器、Discord、Wine 上验证视觉和行为，并记录各应用采用的协议与能力差异。
- [ ] 中英文切换有简洁中文状态标识；提供正式 T 字图标及必要的缩放尺寸。

## 完整行为记录与自动学习

- [x] 事件覆盖已扩展到 key down/up、字母/数字/标点、删除、提交、Esc、Tab/Shift+Tab、Home/End、PageUp/PageDown、方向键、候选展开/收起/移动/选择和模式变化；Fcitx 早退的修饰键、Shift tap/chord 与 surrounding-text Backspace 也会记录。完整真实应用事件序列仍需桌面回归。
- [x] CandidateSelect/Commit 事件关联候选前后剩余拼音、提交文本、低敏应用标识、输入/删除间隔、保留文本和 session；普通按键 telemetry 也携带 session/context/privacy。最终保留文本在真实应用光标场景仍需验证。
- [x] `ihao → 删除 → nihao` 的删除起点会进入受限漏键规则；重复三种以上同类证据后可对未见拼音生成候选。`woc → 我 → 操` 的完整选择链可以学习，不限于空格提交或完整拼音输入。
- [x] 个人词、生僻词/专名的明确选择、选择序列和同上下文漏键规则可自动积累；规则按场景隔离，英文启发式仍主要由原文识别覆盖。
- [x] 已实现有限范围的去重、短窗口批处理与增量处理：worker 使用 40ms 批次窗口并以低优先级运行；同一批次只合并连续、同 session/context/privacy 的相同 `CandidateMove`，不会跨越选词、提交、删除或纠错事件。学习回归用测试延迟证明重复导航被压缩且选词链完整；更广泛的全事件去抖以及长期资源上限仍未验收。
- [x] 精确个人词与泛化漏键规则分离；漏键规则要求至少 6 次支持和 3 种修正拼音，最多 128 条/场景且只在 LibIME 仅返回原文时介入，高置信基础结果优先。
- [x] 固定 LibIME 评估集包含“你再干什么”“我想知道”等正常整句；质量测试在个人精确学习后再次逐句检查首选未被污染。新词和受限漏键收益另有独立学习/纠错测试，尚未形成更大规模离线评估集。
- [x] 学习 worker 将事件处理、模型发布和启动加载拆分为独立内部阶段；发布通过原子快照替换，后续候选查询自动读取新版，不需要按钮或重启。
- [x] 后台写入失败会回滚内存批次、保留可用旧版本、把事件重新排队并在 200ms 后自动重试；`tipe-learning-tests` 的写入失败场景确认输入继续可用且不会重复计数，worker 状态路径会写入“下次自动重试”。监督窗口在连续失败期间的实时显示和不弹窗行为仍需真实 UI 观察。
- [x] `RuleStore` 已接入运行时候选生成、持久化、回滚和上下文隔离；`tipe-correction-flow-tests` 验证真实删除/重输事件、未见拼音、重启恢复、敏感模式和成熟 LibIME 首选不被覆盖。

## 持久化、隐私与数据管理

- [x] 个人数据启动时恢复；单 writer 锁、控制文件和只读控制中心共享同一版本化状态，不用独立内存覆盖文件。
- [x] 发布包含版本号、FNV 校验、原子替换、上一版保留与回滚；损坏、写入失败和连续清空测试通过。
- [x] 记录与模型有长度、容量和 2048 事件队列上限；关闭学习会排空待处理队列，暂停后不继续发布。
- [ ] 密码和敏感输入字段不进入学习；核心层和独立 Fcitx testfrontend 已覆盖 `PasswordOrSensitive` 能力并比较敏感提交前后的模型文件，且 context/privacy 切换会重置输入与删除间隔。真实 GTK4/Qt6 password harness 现在设置各 toolkit 的密码/敏感提示并隐藏回显，JSONL 只保留长度与 redacted；`build/isolated-sensitive-final` 已观察到两个真实密码字段收到输入、密码属性生效且私有模型 SHA-256 摘要前后相同。隔离脚本使用临时 HOME/XDG_STATE_HOME，未触碰用户状态；其他前端与模式切换仍需真实回归。导出使用 0600 临时文件、fsync 和原子 rename，已有测试验证权限和无残留临时文件。
- [x] 清空、导出、暂停/恢复和回滚通过共享控制协议作用于运行中的 writer；清空后的旧队列不会重新写回数据。
- [x] 默认没有云端原始键盘流；模型设置只保存 API Key 路径，不读取密钥，云端 transport 默认关闭。
- [x] 云端发送前契约和最小 loopback HTTP transport 已加入：`make_cloud_payload` 只接受显式同意的 CandidateSelect/Commit，拒绝敏感字段和键盘流，context 只发送 FNV token；`send_cloud_request` 在每次请求再次检查显式同意、轮询 `CloudCancellation`、限制超时和响应大小。`tipe-cloud-transport-tests` 用本地 HTTP 服务验证发送体脱敏、取消、超时和上限。
- [ ] 生产云端仍未启用：当前 transport 拒绝远程明文 HTTP 和 HTTPS（尚未接入经过审计的 TLS/密钥读取），也没有把请求接到后台 worker 或控制中心状态页。
- [ ] 长期资源与留存上限仍未全部验收：除 12000 条有界压力回归外，`tipe-resource-stress-tests` 现已持续分批推送并反复发布模型，运行期间采样队列、模型条目、目录大小、`/proc` VmRSS 和 CPU/wall；默认 CTest 120 轮以及 `scripts/run-resource-stress.sh` 的 600 轮运行均确认队列 `<=2048`、模型条目 `<=4096`、状态目录 regular-file 总量 `<8 MiB`、采样 RSS `<256 MiB`，并逐项核对 `disk_bytes()` 与实际文件大小。该回归不是断电证明，也不宣称长期 CPU 上限；断电恢复和真实桌面持续资源监控仍待验收。

## 监督窗口与模型选择

- [ ] 学习状态页仍需真实 Qt 观察者视觉回归；共享状态一致性已有自动证据：`tipe-learning-tests` 覆盖 writer 连续发布、控制实例读取、失败清空后的“下次自动重试”、成功清空和回滚失败，并验证观察次数、模型数和状态最终收敛。新增 `tipe-control-window-tests` 在 Qt offscreen 中真实点击暂停/恢复、启用/关闭、清空、导出和模型选择，并等待观察者定时器读取窗口打开后的新发布；这验证交互与反馈，不替代真实桌面视觉观察。
- [x] 状态页提供暂停/恢复、关闭/开启学习、清空、导出、回滚和数据管理，操作即时反馈；关闭学习会排空待处理队列并持久化启用状态。
- [x] 模型页说明 TiP、本地小模型和云端模型做什么、何时运行；TiP 默认启用，本地小模型可选择并从有界 `local-small.tsv`/内置表在内存中查询，显示文件位置和 API Key 配置位置；默认云端关闭。`tipe-local-small-tests` 验证显式选择后自定义候选置顶且 TiP 路径不受影响。
- [ ] TiP 个人层自动增量整理在低优先级 worker 中，可暂停和回滚；当前已支持批次内取消、暂停时回排、清空/禁用时丢弃和事务回滚，独立 writer/control 子进程回归已证明共享控制文件的跨进程暂停、批次回滚与恢复，另有子进程突然退出后重新打开模型的回归测试。仍未证明真实独立 TiP 训练器、SIGKILL/断电发生在每个 fsync/rename 窗口时的完整恢复协议或长期后台 TiP 调度。失败重试和回滚已有测试；本地小模型已实现为可选的有界本地候选表，云端后端提供脱敏 loopback transport 合同，生产 TLS、后台调度和网络服务仍待实现。
- [x] 支持页有简洁中文说明、微信/支付宝收款码预留；界面不堆术语或技术指标，不要求“更新模型”。`tipe-support-tests` 和 `tipe-support-hidpi-tests` 覆盖中文文案、两个占位框以及大字体布局；真实收款码图片仍待项目方提供。

## 性能与工程交付

- [x] 当前工作区已重新运行 LibIME 诊断：整段 `nihao` 最近两次 p50=2.74–2.87 ms、p95=2.94–3.06 ms、p99=3.84–4.32 ms；逐键 p50=0.42–0.44 ms、p95=1.06–1.11 ms、p99=1.15–1.19 ms。该工具仍是单进程基准，不能替代长期桌面监控或真实候选视觉测量。
- [x] 热路径没有网络、大模型或同步磁盘；按键只更新本地状态并入队，LibIME 解码在本地完成，学习发布在独立低优先级 worker。
- [x] `tipe-diagnose` 输出 CPU、峰值 RSS、队列深度、丢弃事件、发布次数和模型写入估计；该工具是单进程基准，不替代长期桌面监控。
- [x] 输入插件只按 InputContext 生命周期创建 Engine，控制中心使用只读 Learner，不启动竞争 worker；未使用 niri IPC 轮询。诊断输出队列、丢弃、发布和磁盘计数。
- [x] 预演、安装、卸载和重启脚本的窄范围路径已有记录；`enable-user.sh`/`disable-user.sh` 现在通过 `fcitx-profile.py` 解析实际分组和项目编号，保存精确备份状态，重复启用幂等，禁用时优先精确恢复，若用户之后编辑过则只移除 TiPE 并保留后续修改。已用临时 profile 做启用、重复启用、恢复和后续编辑场景验证。
- [x] 安装预演验证插件、输入法和图标路径，独立 fcitx5 testfrontend 实际加载 TiPE 并完成按键/候选/提交回归；真实桌面会话切换仍需人工观察。
- [ ] 自动测试覆盖状态机、长拼音、部分选词、候选导航、删除恢复、英文切换、网格布局、部分 surrounding text 协议、学习、重试、原子发布、回滚和隐私；没有覆盖真实编辑光标、真正自绘网格、长期性能/留存或真实窗口视觉，因此不能将“光标协议、性能和完整验收”写成已覆盖。
- [ ] 真实 GTK、Qt、浏览器、终端、Discord、Wine 回归记录应用版本、Wayland/XWayland 协议、步骤与结果；需要人工观察的项目集中给用户一次清楚步骤。
- [x] README 与白话说明同步描述实际能力、数据路径、暂停/清空/导出/回滚方法和限制，删除尚未实现能力的肯定性表述；两份文档明确注明个人精确词需重复三次才加入同一输入候选，并把当前受限漏键规则与尚未完成的完整置信度/泛化纠错系统区分开。
- [x] 本次交付说明覆盖架构、事件学习链、性能隔离、模型职责、质量保护、输入行为、光标协议、改动文件、测试证据和剩余限制。

## 当前实现中应优先复核的事实

当前实现已接入 LibIME 系统词典/语言模型，按键 down/up 进入有界队列，学习模块提供版本化原子发布、FNV 校验、加载、回滚、敏感过滤和低优先级 worker。仍未完成的项目以本清单未勾选项为准，尤其是完整真实应用交互回归、协议网格视觉、跨进程监督窗口和恢复已提交文本为可编辑拼音。

完成判断应逐项取得直接证据。某项测试通过只能证明其覆盖的行为，不能代替真实应用、性能或隐私验收。
