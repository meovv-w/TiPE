# 模型与学习

## 先说结论

TiPE 的“模型”不是每按一个键就让大模型生成一次文字。

正常输入时：

- C++ 状态机负责拼音、候选和提交。
- LibIME 在本机自动学习常用词序。
- TiPE 记录有上限的按键、删除、重打和最终选择证据。
- 已经验证过的个人规则在 C++ 内直接应用。

只有下面三种显式动作会启动额外模型工作：

1. 在学习窗口点击“更新 TiP”。
2. 在模型页点击“分析当前输入”。
3. 输入过程中按 `F9`。

因此，TiPE 可以理解输入习惯，但不需要常驻大模型占用 CPU 和内存。

## 三种学习不是一回事

### 日常词序学习

每次正常提交中文候选时，TiPE 更新自己的 LibIME 用户历史。它擅长：

- 常用词靠前。
- 已提交上下文后的自然续词。
- 普通词频和 bigram 调整。

这层自动运行，不需要点击，不运行 Python 或大模型。

### TiP 个人行为模型

TiP 是 TiPE 自带的轻量个人模型，模型名固定为 **TiP**。它处理日常词序之外的行为：

- 你明确选过但不在第一位的候选。
- 经常原样提交的英文标识符。
- 删除后重打形成的纠错对。
- 漏键、交换键、额外按键和替换键的位置模式。
- 长拼音分段选择形成的完整词链。

TiP 训练读取监督历史，生成版本化 JSON 模型。训练结束后，安全能力会蒸馏成运行时偏好行，让 C++ 引擎直接使用；普通按键不会重复启动训练脚本。

TiP 不会因为一个偶然样本就大范围改变候选。不同能力有独立阈值和验证门：

- 精确候选偏好达到证据阈值后生效。
- 原样英文需要重复确认。
- 精确纠错和通用漏键习惯使用不同阈值。
- 通用候选重排必须在隔离验证集上不劣于首选基线。

### 外部本地或云端模型

外部模型负责一次性分析当前受限请求：

- 在已有候选中重新排序。
- 判断一次输入是否可能漏键或错键。
- 确认当前拼音和候选偏好。
- 确认可以复用的分段链。

模型输出必须通过本地校验。它不能执行命令、写任意文件或返回候选列表以外的任意文字直接提交。

## TiPE 监督什么

TiPE 监督 fcitx5 交给当前输入上下文的按键，包括：

- 字母、数字和符号。
- 空格、回车、退格和 Delete。
- 方向键、Tab、翻页和候选选择。
- 拼音中间编辑。
- 原样英文提交。
- 选词、取消和分段提交。

它不会得到：

- 窗口管理器先消费的全局快捷键。
- 没有经过当前输入法的应用事件。
- 密码或敏感输入框中的内容。

按键释放事件只用于显式模型触发的去抖，不作为重复训练样本。

## 一个漏键习惯怎样形成

假设用户想输入 `nihao`，但多次出现：

```text
ihao
全部删除
nihao
最终选择“你好”
```

TiPE 会先保存：

1. 原输入 `ihao`。
2. 删除和重打轨迹。
3. 修正输入 `nihao`。
4. 最终真实选择。

精确纠错证据达到阈值后，`ihao -> nihao` 可以直接生效。多个不同词都出现相同位置漏 `n` 时，TiP 才可能发布更通用的“位置 0 漏 n”习惯。

已经存在明确中文选择证据的拼音会阻止过于宽泛的键盘习惯覆盖用户真实意图。

## 一个生僻词怎样学会

当系统词库没有完整词语时，用户可以分段选择：

```text
完整拼音
选择第一段
选择第二段
完成提交
```

TiPE 会保存完整链：

- 原始完整拼音。
- 每段消耗的拼音。
- 每段提交的文字。
- 每一步剩余拼音。
- 最终组合词。

第一次一致链路作为暂存证据；第二次一致确认后，非 ASCII 完整词可写入 TiPE 用户词典。之后无需启动模型即可直接候选。

## 模型页怎样使用

打开：

```bash
tipe-supervision-window
```

进入“模型”页后，先选择三类之一：

1. **TiP**：TiPE 自带的本地个人学习。
2. **本地大模型**：llama.cpp、Ollama 或自定义本地封装。
3. **云端大模型**：OpenAI 或兼容 API。

“检查填写内容”只做本地配置校验，不联系服务商，也不会产生模型费用。

“保存配置”写入：

```text
~/.config/tipe/model-env
```

直接填写的 API Key 单独写入：

```text
~/.config/tipe/model-api-key
```

该文件权限为 `0600`，配置文件只保存它的路径，不保存明文 Key。

学习窗口自己的下一次分析可以立刻使用新配置。正在运行的 fcitx5 要使用新模型入口，需要用户明确执行 `tipe-restart-fcitx5`。

## TiP

训练：

```bash
tipe-personal-model-train
```

查看模型：

```bash
tipe-personal-model inspect
```

查看可训练记录数：

```bash
tipe-training-export --stats
```

默认训练：

- 单次运行后退出。
- 使用 `nice 10`。
- 不包含应用名称。
- 不包含原始周边文字。
- 不包含长纠错轨迹和已知证据，除非显式启用。
- 读取 Fedora/Rime 拼音词典作为可用先验，但不上传词典内容。

点击“更新 TiP”只训练累计的终止样本，不分析当前正在输入的文字。“分析当前输入”是另一项独立操作。

## llama.cpp

配置一次性 GGUF：

```bash
tipe-model-config --write llama-cpp \
  --model "$HOME/.local/share/tipe/models/model.gguf" \
  --llama-command /usr/bin/llama-cli
```

默认行为：

- 每次点击分析或按 `F9` 才启动一个 `llama-cli`。
- 默认 CPU 模式、6 个线程、8192 token 上下文。
- 默认模型超时 30 秒。
- 请求结束后进程退出。

这种方式空闲占用为零，但每次请求都要重新加载 GGUF。

## Ollama

```bash
tipe-model-config --write ollama \
  --base-url http://127.0.0.1:11434/v1 \
  --model qwen2.5:0.5b
```

TiPE 把 Ollama 当作本机 OpenAI 兼容接口。`--test-dry-run` 可以验证生成的请求，不真正联系模型。

## OpenAI

通过标准输入保存 Key，避免出现在进程参数中：

```bash
printf '%s\n' "$OPENAI_API_KEY" |
  tipe-model-config --write openai \
    --model YOUR_MODEL_NAME \
    --api-key-stdin
```

TiPE 不固定一个具体云端模型名称，用户应填写自己账号当前可用的模型。

## 其他 OpenAI 兼容接口

```bash
printf '%s\n' "$PROVIDER_API_KEY" |
  tipe-model-config --write openai-compatible \
    --base-url https://api.example.com/v1 \
    --model YOUR_MODEL_NAME \
    --api-key-stdin
```

服务地址必须是 `http` 或 `https`，聊天路径必须以 `/` 开头。

## 云端到底发送什么

云端请求使用固定版本 `tipe.cloud-rerank.v1`。默认只包含：

- 当前拼音。
- 当前候选及其索引。
- 当前候选光标和展开状态。
- 可安全复用的当前请求元数据。

默认关闭的两个权限：

| 权限 | 开启后增加的数据 |
| --- | --- |
| 近期按键 | 受限的按键、编辑、历史和学习证据 |
| 光标附近文字 | 受限周边文字和应用名称 |

这两个开关相互独立，必须由用户主动打开。

云端回复只接受以下协议行：

```text
candidate<TAB>候选文字
correction<TAB>原拼音<TAB>修正拼音
preference<TAB>拼音<TAB>候选文字<TAB>次数
segment_chain<TAB>原拼音<TAB>已消耗拼音<TAB>已提交文字<TAB>剩余拼音<TAB>修正完整拼音<TAB>组合候选<TAB>次数
```

不在当前候选中的排序建议、非法拼音、过长字段、与已知证据简单回声以及无法验证的分段链都会被拒绝。

## 自定义模型封装

生成一个安全模板：

```bash
tipe-model-wrapper-new \
  --path "$HOME/.local/bin/my-tipe-model-wrapper"
```

检查协议：

```bash
tipe-model-wrapper-check \
  --command "$HOME/.local/bin/my-tipe-model-wrapper"
```

配置：

```bash
tipe-model-config --write custom \
  --command "$HOME/.local/bin/my-tipe-model-wrapper"
```

TiPE 直接执行封装程序，不经过 shell。需要管道、复杂引号或凭据逻辑时，应全部放入权限受控的封装脚本。

## 模型自测

当前配置：

```bash
tipe-model-current --show
tipe-model-self-test --current
```

只验证请求，不访问接口：

```bash
tipe-model-self-test --current --adapter-dry-run
```

导出一次请求：

```bash
TIPE_MODEL_DUMP_PATH=/tmp/tipe-model.tsv \
  tipe-model-dump < /tmp/request.tsv

tipe-model-explain /tmp/tipe-model.tsv
```

## 连续模式

`Shift+F9` 切换连续模式。连续模式只运行已经加载到进程内的轻量重排，不会每按一个键调用：

- llama.cpp
- Ollama
- OpenAI
- 自定义外部模型命令

因此它与“始终把大模型挂在后台”不是一回事。

## 目前的边界

- TiP 需要足够的真实、重复样本才能安全发布通用习惯。
- 模型只能改善排序、纠错、英文倾向和分段链，不能弥补底层词典完全没有候选元数据的所有情况。
- 应用没有把按键交给 fcitx5 时，TiPE 无法监督该按键。
- 云端质量取决于所选模型，但所有结果仍受本地协议限制。
- 训练结果保留旧模型能力，验证不通过时不会为了“看起来更新了”而替换更好的现有模型。
