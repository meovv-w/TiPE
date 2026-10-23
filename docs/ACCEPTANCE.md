# TiPE 验收状态（2026-10-23）

以原始完整需求和后续反馈为范围，按用户确认的范围已完成交付。2026-10-23 用户明确回复“这两个不用管”，移除实体 niri 多屏和 Discord 登录聊天界面两项验收条件；这两项仍记为未验证，不改写为通过。可选云端功能仍在交付范围内，默认关闭，服务契约已测试，外部生产服务未配置实测。绿色自动测试只证明对应覆盖范围。历史清单与早期运行记录保存在 ACCEPTANCE-history-202609.md。本页以当前代码及 docs/evidence/2026-10-21、2026-10-22、2026-10-23 的直接证据为准。

最终启动复核补齐了重新登录后的模块加载路径：安装器保存可恢复的 Fcitx 用户服务 drop-in，不依赖临时 manager 环境。直接重启现有服务验证三个模块与之前测试版哈希完全相同；成功加入并选择 TiPE 后恢复用户原选拼音。启动、映射与安装回归3/3通过，其中安装事务用例现为11项。证据为 `evidence/2026-10-23/startup-service-results.json`、`ctest-startup-final.log`；不改 niri 或服务启动命令。

2026-10-23：修复超过64字节后无中文候选，新增65/70/100/260字符核心回归；
68字符跨窗口分段在实际安装GTK/Qt/浏览器/Wine/foot五条路径5/5通过。
中间编辑改为只同步变化区间；共享基础词库解决每上下文重复加载约30MB数据，
12上下文空闲RSS从395MB降至46MB，活动解码资源回归也已通过。
前一轮完整CTest36/36通过，73.06秒，见`evidence/2026-10-23/ctest-long-shared-final.log`。
安装后又通过全部98项真实编辑回归，GTK/Qt/Chromium各21、Wine19、foot16，
结果为同目录`editing-long-shared-results.json`，并核对新核心已被当前daemon映射。
该轮代表性长组合逐键p95低于5ms，部分中间编辑p95约6–9ms；首轮和最大值保留在
`diagnose-long-shared-tip.log`、`diagnose-long-shared-trained.log`，不声称所有操作均达标。

后续已优化空历史评分、固定2KiB标量缓存与展开前缀去重/复用。70组候选与
35,664次逐词浮点评分对比通过，模型发布/清空使展开缓存正确失效。
最新完整CTest36/36通过（82.36秒），见`evidence/2026-10-23/ctest-prefix-linear-final.log`。
不同长拼写导航p95约1.23ms，242字节中间编辑约4.07–4.14ms。所有首轮、最大值
和一次整体变慢的异常采样都保留，不能据此宣称任意负载下所有按键均低于5ms。

优化版实际安装后又通过43/43项分段回归，以及GTK/Qt两项真实缩写链自动学习。
当前运行模块哈希及inode见`evidence/2026-10-23/installed-runtime-decoder-navigation.json`。

已安装niri 26.04的独立会话再通过64/64项：GTK、Qt、原生foot、Chromium各16项。
同一活动组合从1×切到1×/2×/1.25×/1.5×，普通/长词的键盘与鼠标提交均正确；
截图和协议光标矩形确认候选完整可见及定位，展开宽度差为0，缩放前后最大差2个
逻辑像素。证据为`evidence/2026-10-23/niri-isolated-results.json`及对应runtime、
日志和截图。这是未修改的系统niri运行于私有无屏幕Sway中的单输出动态缩放，
不是实体niri双屏；当前用户daemon未重启，模块映射和选中TiPE再次核对通过。

Discord真实界面已尝试在空白配置的隔离niri中启动，18次观察均只有更新器，
无输入框；`discord.com`更新服务连接失败（curl退出7）。未复制用户登录数据、
替换DOM或发送消息。另保存隐藏私有niri启动提示后的实际更新器截图。
该结果证明当前验证条件不足，不计为聊天输入通过；详见同证据目录
`discord-native-discord-ui-probe.json`、`discord-native-connectivity.json`、
`discord-native-visible-discord-ui.png`。当前实体输出仍只有eDP-1。

| 要求 | 当前实现与证据 | 结论 |
|---|---|---|
| C++20、Fcitx5、协议/UI/学习/模型/监督窗口分层 | CMakeLists、include/tipe、src、integration | 已实现 |
| 成熟基础词库与整句排序 | LibIME sc.dict + zh_CN.lm；质量集与缺失/损坏回退测试 | 已验证 |
| 长拼音、部分选词、后缀保留、完整选择链 | 核心真实图边界；65/70/100/260字符组合；五类前端38项分段与新增5项68字符跨窗口检查；GTK/Qt三次woc完整链自动发布并影响后续输入 | 已验证所列分段、恢复和学习范围 |
| 删除恢复、混合文本、客户端光标编辑 | 守卫周围文本精确后缀；GTK/Qt/Chromium 各21项实际编辑检查；Wine19项、foot16项按协议能力验证 | 已通过所列98项；任意分段选择链和全部应用光标场景未全覆盖 |
| 全部键盘行为与时间间隔 | KeyDown/Up、编辑导航、候选开闭、选择/提交、模式切换、剩余拼音、应用低敏标识 | 自动测试、五类前端98项编辑与38项分段通过；不是所有应用任意序列 |
| 自动静默学习，无更新按钮 | 有界队列、低优先级 worker、去抖合并、原子快照、后台重载 | 已验证 |
| 最终保留结果 | 有周围文本能力时暂存自己的提交，下一次操作验证后缀后学习；立即删除/重编丢弃；不保存应用整段文本 | 已验证 GTK 真实按键：立即删除计数0→0，保留提交0→3并自动发布 |
| 泛化漏字、多字、替换、颠倒 | 同应用6次、3种样本、竞争置信度>=0.8；只在基础解码仅返回原文时介入；不自我强化 | 四种编辑均通过真实核心按键删除重输、异词泛化、重启及上下文隔离 |
| 英文、命令、网址、大小写 | 819正向/306拼音负向语料；显式重复原文习惯；中文可撤销该习惯 | 测试通过；Wine 已验证原文习惯可由显式中文选择撤销 |
| 中英切换、保留组合、Enter原文/Space选择 | 核心/Fcitx生命周期；GTK/Qt/Chromium/foot/Wine/Electron夹具实际按键 | GTK/Qt切窗6项通过；新增Chromium/foot/Wine切窗12项通过，包含返回首个候选、英文往返和第二轮切窗；修复终端组合丢失与XIM旧窗口残留；不是所有应用的任意组合 |
| 黑色、单行、展开原列数原宽度 | 共享 strip_layout/Pango painter；GTK4/Qt6截图和每列矩形断言 | 已验证 |
| 方向、数字、翻页、鼠标一致 | 共享布局；第二十项数字回调；GTK模块点击/hover测试 | GTK4/Qt6 两个位置真实指针与键盘选择一致；Wine 连续两次启动均通过鼠标/键盘一致性 |
| 跟随光标、屏幕边缘、切窗不漂移 | GTK/Qt协议popup；native classicui input-popup；XIM标准矩形 | GTK/Qt与原生Chromium/foot四角各16项通过；GTK/Qt混合缩放24项、foot原生混合缩放12项通过；Wine按应用overthespot光标横移与换行通过 |
| 不猜niri坐标，不轮询定位 | 产品代码依赖协议矩形；niri仅用于测试夹具焦点保护 | 已实现 |
| T图标、中文状态 | SVG 已渲染核对；安装文件与源文件逐字节一致；桌面文件及 Fcitx 的 T label、中文/英文 subMode 已核对 | 已验证交付图形与状态定义 |
| 三页中文监督窗口 | 学习状态、模型、支持；暂停/恢复、关闭、清空、导出、回滚；密钥文件位置 | 三页2×截图已核对；后台导出/取消、单实例唤起及崩溃恢复测试通过 |
| 支持页收款码 | 所有者提供的微信/支付宝原图嵌入程序，离线显示与查看大图；宽窄布局、HiDPI及控制中心4项回归通过 | 已验证；见 support-payments-results.json |
| TiP个人层 | 精确拼写/应用偏好与短选择链、置信纠错 | 已实现 |
| 真正本地小模型 | 后台构建字符二元概率模型；未见组合、真实候选排序、首选保护、应用隔离、重启/清空测试；默认不启用该排序 | 已实现；属于统计小模型，不是神经网络或大模型 |
| 可选云端大模型 | 默认关闭，显式环境授权、语义事件、TLS/取消/退避、受限响应缓存；配置文件位置 | 本地服务协议测试通过；外部生产服务未配置实测 |
| 隐私边界 | 敏感字段不记录；默认本地；不保存完整周围文本；文件0600/目录0700；导出/清空 | GTK/Qt/Chromium/Wine 四类真实密码框通过：字符完整、观察计数不变、测试内容不进模型；GTK/Qt 验证实际已安装模块映射。Chromium 增加字段往返、恢复中文/学习、文本选择保留与空闲无确认循环；XIM 因协议能力缺失默认禁用观察/学习/云端 |
| 原子发布、版本、校验和、回滚、失败重试 | learning测试与专用Debug SIGKILL注入fsync/rename边界 | 已通过；不等同物理断电 |
| 按键低延迟 | Release + LibIME增量前缀；5000单键 p95 0.716ms，整段nihao p95 2.088ms | 该采样达标；已发布本地小模型单键p95 0.832ms（5000次），非所有长句保证 |
| 持续后台资源 | 10分钟153.6万事件；新代码2分钟30.72万事件；队列<=2048、模型<=4096、RSS约20MB、状态约223KB | 新增7.68万事件已训练模型压力：340条转移、4096条目、RSS约23MB、磁盘约275KB、worker CPU约30%；过载会丢学习事件，不丢应用按键 |
| 训练/云端/导出可取消，暂停及时生效 | worker事务取消与共享控制测试；云端token；后台导出/取消按钮、唯一私有临时文件；trainer信号 | 自动测试覆盖 |
| 安装、卸载、启用、重启、回滚脚本 | 预演/当前用户安装、实际卸载和重启，持久事务恢复、文件备份与校验、在线移除 TiPE | 11 项隔离安装用例通过（新增持久启动配置）；历史10项已通过：失败回滚、进程中断、已修改文件保护、多组/默认输入法保留、旧 client UI TSV 清单、卸载后仍保留学习；本轮核心/classicui/waylandim/控制中心/GTK/Qt 构建一致，daemon模块 inode核对；真实卸载只在隔离HOME执行，当前桌面仅运行 dry-run |
| GTK4/Qt6 | 原生候选截图、Enter/Space、长候选；隔离真实控件编辑 | 已安装GTK/Qt切窗6项通过；更新用户Qt模块后重复验证，两个角落展开宽度598保持不变、鼠标/键盘选词一致 |
| Chromium150原生Wayland | 6项DOM及composition证据，真实物理键路径 | 6项输入路径通过；现可收到正常字段的周围文本并恢复拼音；密码/普通字段三项往返与隐私通过；完整任意光标编辑仍未覆盖 |
| foot1.27原生Wayland | 专用stdin夹具4项，实际读回文本 | 已验证 |
| Wine11标准Win32 EDIT | 独立prefix、直接文件读回、可恢复的按应用overthespot配置 | 同一daemon连续两次关闭重开各6项通过；按应用样式另通过3个真实caret位置、6项输入与19项编辑；配置事务5项通过；默认callbacks仍可能只给初始坐标 |
| Discord1.0.162/Electron42 | 隔离启动器renderer、本地contenteditable四项通过，无聊天发送；真实更新器诊断保留 | 登录聊天界面未验证；用户已移除此项验收条件 |
| 中文README、白话说明、最终交付 | README、USER_GUIDE.zh-CN.md、DELIVERY.zh-CN.md 已写；旧审计保留历史 | 实现、操作、证据与限制已记录 |

可重复桌面检查仅操作自有测试窗口，每次按键检查焦点；原生Wayland路径不用wtype冒充输入法抓键，使用有写权限的evdev键盘写入测试按键，不读取键盘内容。脚本入口：check-candidate-desktop.py、check-browser-desktop.py、check-foot-desktop.py、check-wine-desktop.py、check-discord-renderer.py。后者只验证Discord启动器渲染器，不是聊天界面。

以下保留各轮验证历史，曾经的后续验证要求以页首当前范围为准。上一轮完整 CTest 35/35 通过，69.95 秒，日志见 docs/evidence/2026-10-22/ctest-segmented.log。当时安装的核心、classicui、waylandim 和监督窗口与构建 SHA256 一致，D-Bus owner 对应已安装模块 inode，GTK/Qt 密码夹具也验证实际模块映射。已训练模型、真实保留学习、GTK边缘鼠标、Wine连续重开与监督窗口证据见 docs/evidence/2026-10-21。后续隔离双屏和 niri 动态缩放证据见下文，实体多屏现已由用户移除验收条件。

本轮主要证据：`password-all-installed-results.json`、`cross-protocol-focus-current-results.json`、`focus-installed-results.json`、`qt-installed-popup-results.json`、`user-installation-tests.log`、`installed-runtime-current.json`。原生 Wayland 修复来自固定源码的前端状态确认与未知用途隐私回退，不是按应用名称写特殊分支。

新增实际编辑矩阵98/98通过：GTK、Qt、Chromium各21项，Wine19项，foot16项。
输入通过真实物理按键进入自有夹具；GTK/Qt检查实际安装模块的路径和inode。
涵盖preedit中间插入/删除、Shift保留插入位置、展开收起、候选方向/翻页、数字
首选、长拼写、客户端选区替换/光标插入、提交恢复、连续中文提交只恢复最后一段、
取消后重新输入。Wine XIM无法提供周围文本时执行客户端删除；foot只验证组合编辑，
不模拟GUI选区。证据：`editing-gtk-qt-browser-results.json`、
`editing-wine-foot-results.json`及同名日志；入口`check-editing-desktop.py`。
这些结果不证明任意分段选择链、所有屏幕边缘和多显示器已经通过。

后续新增分段矩阵38/38通过：GTK、Qt、Chromium各8项，Wine、foot各7项。
检查逐次选词后的客户端文字和剩余拼音（Wine只检查已提交文字、foot只检查
最终行），包括数字选前缀、编辑/取消后缀、Shift往返、`woc`分选“我”“操”、
三个连续词组、删空后缀后恢复前字。GTK/Qt额外各一项真实自动学习链通过：
三次完整保留后，模型同时发布`wo'cao`和`woc`，后续空格直接得到“我操”。
GTK/Qt四角普通/长候选16/16通过：宽度598、710或718逻辑像素均在展开前后
保持一致，底部向上避让，鼠标与键盘结果一致。

本轮修复了两项根因：核心把LibIME枚举请求位置误当候选消耗边界，以及原生
Wayland组合缩短后客户端保留旧组合范围。后者通过同事务组合替换同步处理，
提交后等待新周围文本快照，避免使用旧坐标。浏览器矩阵还检查协议拒绝日志，
不能只凭最终文字判成功。当时完整CTest35/35通过，69.95秒，见
`ctest-segmented.log`。分段、学习和边缘证据见`current-results.json`关联文件。
最终Wayland模块再次通过浏览器8项分段（并检查协议拒绝日志为空）、浏览器/foot
37项编辑、浏览器密码与字段往返，以及12项跨协议切窗。空闲确认循环计数仍为0。
安装映射与新哈希见`installed-runtime-segmented.json`；5000次单键p95为0.700ms、
整段nihao p95为1.821ms，见`diagnose-segmented-trained.log`。

原生Chromium/foot各通过四角×普通/长句8项，共16项、32次真实键盘/鼠标选择。
桌面截图检查完整候选窗边界、展开前后宽度和向上避让，客户端实际发送的
text-input-v3光标矩形检查锚点，最终文字均与固定预期一致；浏览器协议拒绝
日志为空。证据为`native-popup-results.json`及两份`native-popup-*.log`。
此项覆盖当前单屏2×缩放，不能推导为多屏或混合缩放通过。

隔离双屏检查现已通过24/24：实际已安装GTK/Qt模块各12项，固定1×屏与
1.25×、1.5×、2×屏之间保留活动组合双向移动，普通词/长句均检查紧凑、展开、
移回原屏再移出及键盘/鼠标提交。直接读取合成器framebuffer，对照客户端
xdg_popup锚点检查定位、完整边界与逻辑宽度。模块路径/inode/SHA256在
`mixed-scale-installed-runtime.json`，逐项证据在`mixed-scale-installed-results.json`。
测试发现并修复GTK4跨屏时popup沿用旧输出约束的问题：监听父surface的屏幕及
缩放通知，隐藏旧popup并合并重建，不清除组合、不猜全局坐标。失败证据保留在
`mixed-scale-gtk-before-fix.log`。该结果仅覆盖隔离Mutter双屏；实体niri多屏、
原生input-popup与XIM均已增加下述隔离双屏证据。
安装修复后再通过当前niri桌面的GTK/Qt切窗6项、GTK四角普通/长句8项；
证据见`focus-after-multiscale-results.json`和`gtk-popup-after-multiscale-results.json`。

原生input-popup另通过隔离Sway 1.11双屏12/12：实际安装core/classicui/waylandim，
foot以text-input-v3发送真实光标矩形，经input-method-v2提交。在1×与1.25×、
1.5×、2×之间保留活动组合双向移动，检查普通/长句紧凑和展开窗的完整可见
边界、逻辑宽度、光标锚点及最终键盘/鼠标选词。证据见`native-mixed-scale-*.json/log`。
此处使用私有Wayland seat，wtype实际经过输入法抓键并产生中文提交；与当前
niri的虚拟键盘路径不同。没有向实体桌面注入，也没有改变实体输出。
Sway此版本未把Chromium首次普通字段的延迟content_type转发给input-method；
TiPE因此保持未知用途保护，本组不包含Chromium，不放宽隐私边界来使测试通过。

Wine新增证据：`wine-app-overthespot-caret-results.json`、
`wine-app-overthespot-input-results.json`、`editing-wine-overthespot-results.json`、
`wine-configuration-results.json`。按应用启用和恢复入口见[Wine说明](WINE.zh-CN.md)。

Wine/XIM隔离双屏12/12也已通过，实际安装模块映射见
`xim-mixed-scale-installed-runtime.json`，逐项结果与日志见
`xim-mixed-scale-installed-results.json/log`。Win32标准EDIT在overthespot模式下，
1×与1.25×、1.5×、2×之间保留紧凑/展开组合双向移动；对照实际Win32 caret，
确认候选完整可见、逻辑宽度保持及键盘/鼠标提交一致。1.5×下曾出现X11子窗
内容完整但合成器未显示，失败drawable、截图及重绘对照保留在
`xim-mixed-scale-before-fix*`。现监听嵌入候选实际父窗口的ConfigureNotify后
重绘，不靠定时器、compositor坐标或追加按键恢复。固定源码构建脚本可重复
应用该补丁，安装后协议/生命周期7项通过；实体niri多屏仍未验证。
当前niri桌面又通过Chromium/foot/Wine跨协议切窗12项，包含第二轮切窗与
组合往返，未发现旧候选窗残留；证据见`focus-after-xim-parent-results.json`。
