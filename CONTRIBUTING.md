# 参与 TiPE 开发

请优先提供可重复的输入步骤：应用及版本、桌面/合成器、输入协议、缩放比例、逐键操作、预期结果和实际结果。不要提交密码、API Key、私人学习文件或无关周围文本。

## 本地开发

构建依赖见 [首页](README.md)。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/tipe-demo
./build/tipe-diagnose --trained-local --representative
```

需要按键级状态时：

```bash
./build/tipe-demo --json-lines --state-dir /tmp/tipe-demo-private
```

标准输入接受空格分隔的按键名，输出提交、剩余拼音、编辑光标与候选消耗边界。请使用独立状态目录，不要把复现数据写进日常个人学习。

## 改动约定

- 通过状态机、协议能力和生命周期修复通用根因，不按应用名复制特例。
- 输入热路径不进行网络请求、同步落盘或模型训练。新的后台工作必须有容量、取消、隐私与资源边界。
- 产品使用协议提供的光标矩形和原生 popup；不要轮询或猜测 niri 全局坐标。
- GTK、Qt 和 classicui 使用同一候选布局与绘制器。
- 真实桌面回归顺序执行，只操作自有夹具并检查焦点；不得读取实体键盘或发送聊天。
- 安装改动须覆盖预演、原子替换、已有文件保护、卸载恢复与失败回滚。

为会造成实际回归的行为添加测试，并运行相关检查。UI 改动应核对普通/长候选、展开宽度、鼠标命中、窄窗口和缩放；不要把夹具结果写成所有应用均通过。

更多入口见 [开发参考](docs/DEVELOPMENT.zh-CN.md) 与 [真实应用验证说明](docs/REAL-APP-HARNESS.zh-CN.md)。提交 PR 时说明行为变化、原因、验证结果和仍存在的限制。
