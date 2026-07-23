# 参与 TiPE 开发

感谢关注 TiPE。项目仍处于开发预览阶段，最有价值的贡献是可以重复、可以验证的真实输入问题。

## 提交问题前

先运行：

```bash
tipe-doctor
./scripts/build.sh
./scripts/smoke-test.sh
```

输入逻辑问题尽量用状态探针缩小：

```bash
./build/tipe-state-probe "" --script your-actions.txt
```

动作文件示例：

```text
type nihao
select 你
expect-preedit hao
```

请不要在 Issue 中粘贴：

- API Key。
- 密码框内容。
- 不希望公开的周边文字。
- 完整私人监督历史。

## Bug 报告需要什么

- 应用名称和版本。
- 桌面环境或 compositor。
- Wayland、DBus/GTK、XIM 或 Wine 前端。
- 显示缩放比例。
- 最短输入拼音和逐键步骤。
- 预期结果与实际结果。
- `tipe-doctor` 的相关输出。
- 可以复现时附上状态探针动作文件。

“偶尔会吞拼音”很难形成回归测试；“输入 A，选择 B，再按 C 后 preedit 从 D 变为空”可以直接验证和修复。

## 开发流程

1. 从 `main` 创建分支。
2. 保持改动集中在一个问题。
3. 为状态机或脚本行为补充回归测试。
4. 运行构建和 smoke-test。
5. 确认没有提交构建目录、缓存、模型文件和用户监督数据。
6. 提交 Pull Request，说明行为变化和验证结果。

## 代码约定

- C++ 使用 C++20。
- 延续现有模块边界，不在 UI 中复制状态机规则。
- 普通输入路径不能启动 Python、网络请求或常驻大模型。
- 新的监督字段必须有大小上限和持久化隐私策略。
- 模型输出必须经过结构化解析和本地校验。
- 安装脚本不得自动修改 fcitx5 profile、桌面快捷键或全局配置。
- 手工源码修改使用清晰、有限的提交，不顺带格式化无关文件。

## 验证命令

```bash
./scripts/build.sh
ctest --test-dir build --output-on-failure
./scripts/smoke-test.sh
```

HTTP 后端测试在禁止本机回环 socket 的沙箱中会跳过，可在普通终端单独运行：

```bash
python3 tests/model_backend_http_test.py scripts/model-adapter.sh
```

## UI 改动

候选窗口改动至少检查：

- 收起与展开布局。
- 长候选跨格。
- 数字标签只属于当前行。
- 方向键、Tab、翻页和鼠标点击。
- 屏幕四边约束。
- 1 倍与分数缩放。
- 原生 Wayland UI 和 GTK 后备 UI 的一致性。

学习窗口改动应保持普通用户只看到“学习、模型、支持”三个主要页面，协议字段和诊断信息放在高级详情。

## 隐私和安全问题

涉及密码字段、API Key 泄露、越界写入或模型命令注入的问题，请不要先公开私人样本。可以只描述受影响模块和复现前提，再通过仓库的安全联系入口协调详细信息。
