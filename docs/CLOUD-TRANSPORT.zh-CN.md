# 云端增强传输边界

TiPE 的云端增强默认关闭。调用方必须同时把
`CloudTransportOptions::enabled` 和本次请求的 `consent` 设为真；否则
`send_cloud_request` 在创建 socket 前返回 `Disabled` 或 `ConsentRequired`。
请求只能由 `make_cloud_payload` 产生，字段固定为拼音、提交文本、FNV
context token、输入/删除间隔和请求编号。原始 key、preedit、surrounding
text、应用路径和 session 标识没有传输字段。敏感事件、KeyDown/KeyUp 和
超长字段会返回 `InvalidPayload`。

传输支持两种端点：

* `http://localhost/...`、`127.0.0.1` 或 `::1` 用于本地服务和测试；远程
  明文 HTTP 一律拒绝。
* `https://...` 使用 OpenSSL 客户端，最低 TLS 1.2，默认系统 CA（或
  `ca_certificate_path` 指定的 CA 文件），并验证 DNS 名称或 IP SAN。
  DNS、连接、TLS 握手、发送和接收共享一个总超时，响应大小和取消操作
  都有界。

API key 只有在 `include_api_key` 和显式同意同时开启时才读取。路径必须
指向当前用户拥有的普通文件，权限不可授予组或其他用户，内容最多 4 KiB
且只能是一行可打印 token；读取后通过 `Authorization: Bearer` 发送，
不写入日志。带 API key 的请求必须使用 HTTPS。

本地合同由 `tipe-cloud-transport-tests` 覆盖：

```sh
cmake --build build --target tipe-cloud-transport-tests
./build/tipe-cloud-transport-tests
```

测试验证显式同意门、请求脱敏、loopback HTTP、远程明文和 HTTPS URL、API
key 文件权限、预取消、读取超时及响应上限。输入热路径不会直接执行网络请求；
具体生产服务仍需由用户配置 endpoint、请求 schema 和服务端认证策略。

可选的 `CloudWorker` 已接入 `Learner`。只有同时设置
`TIPE_CLOUD_ENABLED=1`、`TIPE_CLOUD_CONSENT=1` 和
`TIPE_CLOUD_ENDPOINT=https://...` 才会创建可发送的后台队列；API key 和 CA
路径分别由 `TIPE_CLOUD_API_KEY`、`TIPE_CLOUD_CA` 提供。队列最多保留 64 条
去重后的语义事件，成功请求间隔至少 5 秒，失败最多指数退避重试 3 次。暂停、
关闭学习、清空和回滚都会取消待发送任务，状态写入 `cloud.state` 并由控制中心
显示。默认环境没有这些变量，因此仍完全关闭。

Worker 对 2xx 响应继续执行严格的候选响应校验：响应只能是带有
`candidates` 数组的 JSON 对象（也可使用 `{}` 表示仅确认），候选项固定为
`pinyin`、`text`、`score`，可选 `request_id` 且必须匹配当前请求。未知字段、
重复候选、非法 UTF-8、控制/双向覆盖字符、超过 32 项、过长文本或超出
0..1000 的分数都会被拒绝。通过校验的结果只放入内存中的短 TTL、有限容量、
context token 隔离缓存；取消、重新配置或清空时立即删除，永不写入基础词库或
个人模型。缓存查询 API 已接入候选生成：只有在显式环境门控和同意都存在时，Cloud
backend 才可选。经过 schema、UTF-8、请求号、数量、分数和 TTL/context 校验的结果
会作为低优先级候选追加；不会写入 LibIME 或 TiP，失败和取消会清空缓存并回退本地结果。
