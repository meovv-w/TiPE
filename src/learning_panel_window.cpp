#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::string_view, 3> primaryPageTitles = {"学习", "模型", "支持"};
constexpr std::array<std::string_view, 2> supportImageNames = {"wechat", "alipay"};

struct LearningPanelRow {
    std::string section;
    std::string key;
    std::vector<std::string> values;
};

struct LearningPanelData {
    std::vector<LearningPanelRow> rows;
    bool selfTest = false;
    bool parseOnly = false;
    bool argumentError = false;
    std::string inputPath;
    std::string refreshCommand;
    std::string modelConfigCommand;
    std::string personalTrainCommand;
    std::string statusText;
    std::string windowTitle = "TiPE";
    GtkWidget *window = nullptr;
    GtkWidget *content = nullptr;
    GtkWidget *statusLabel = nullptr;
    GtkWidget *rowsBox = nullptr;
    GtkWidget *analyzeButton = nullptr;
    GtkWidget *personalTrainButton = nullptr;
};

struct ModelSettings {
    std::string mode = "heuristic";
    std::string model;
    std::string baseUrl;
    std::string chatPath;
    std::string apiKeyEnv;
    std::string apiKey;
    bool apiKeyStored = false;
    std::string apiKeyRuntime;
    bool clearApiKey = false;
    std::string customCommand;
    std::string personalModelPath;
    std::string dumpPath;
    std::string llamaCommand;
    std::string llamaThreads;
    std::string llamaContext;
    std::string timeout;
    std::string httpTimeout;
    std::string temperature;
    std::string maxTokens;
    bool continuous = false;
    bool trainingContext = false;
    bool trainingSurrounding = false;
    bool cloudRecentInput = false;
    bool cloudSurrounding = false;
};

struct ModelSettingsWindow {
    LearningPanelData *panel = nullptr;
    GtkWidget *window = nullptr;
    GtkWidget *mode = nullptr;
    GtkWidget *modeHint = nullptr;
    GtkWidget *modelLabel = nullptr;
    GtkWidget *model = nullptr;
    GtkWidget *baseUrlLabel = nullptr;
    GtkWidget *baseUrl = nullptr;
    GtkWidget *chatPathLabel = nullptr;
    GtkWidget *chatPath = nullptr;
    GtkWidget *apiKeyEnvLabel = nullptr;
    GtkWidget *apiKeyEnv = nullptr;
    GtkWidget *apiKeyLabel = nullptr;
    GtkWidget *apiKey = nullptr;
    GtkWidget *clearApiKey = nullptr;
    GtkWidget *customCommandLabel = nullptr;
    GtkWidget *customCommand = nullptr;
    GtkWidget *personalModelPathLabel = nullptr;
    GtkWidget *personalModelPath = nullptr;
    GtkWidget *dumpPathLabel = nullptr;
    GtkWidget *dumpPath = nullptr;
    GtkWidget *llamaCommandLabel = nullptr;
    GtkWidget *llamaCommand = nullptr;
    GtkWidget *llamaThreadsLabel = nullptr;
    GtkWidget *llamaThreads = nullptr;
    GtkWidget *llamaContextLabel = nullptr;
    GtkWidget *llamaContext = nullptr;
    GtkWidget *timeoutLabel = nullptr;
    GtkWidget *timeout = nullptr;
    GtkWidget *httpTimeoutLabel = nullptr;
    GtkWidget *httpTimeout = nullptr;
    GtkWidget *temperatureLabel = nullptr;
    GtkWidget *temperature = nullptr;
    GtkWidget *maxTokensLabel = nullptr;
    GtkWidget *maxTokens = nullptr;
    GtkWidget *continuous = nullptr;
    GtkWidget *trainingContext = nullptr;
    GtkWidget *trainingSurrounding = nullptr;
    GtkWidget *cloudPrivacy = nullptr;
    GtkWidget *cloudRecentInput = nullptr;
    GtkWidget *cloudSurrounding = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *check = nullptr;
    GtkWidget *save = nullptr;
};

bool refreshModelConfigRows(LearningPanelData &data);
void startPersonalTraining(LearningPanelData &data);
void refreshClicked(GtkButton *, gpointer userData);
void analyzeClicked(GtkButton *, gpointer userData);
void personalTrainClicked(GtkButton *, gpointer userData);
void modelSettingsClicked(GtkButton *, gpointer userData);

constexpr std::string_view modelModes[] = {"personal", "ollama", "llama-cpp", "openai", "openai-compatible",
                                           "heuristic", "custom", "dump", "off"};

std::string rowValue(const LearningPanelData &data, std::string_view section, std::string_view key,
                     std::string_view fallback = "") {
    for (const auto &row : data.rows) {
        if (row.section == section && row.key == key && !row.values.empty()) {
            return row.values[0];
        }
    }
    return std::string(fallback);
}

ModelSettings modelSettingsFrom(const LearningPanelData &data) {
    ModelSettings settings;
    settings.mode = rowValue(data, "model-config", "configured-mode", "heuristic");
    settings.model = rowValue(data, "model-config", "model");
    settings.baseUrl = rowValue(data, "model-config", "base-url");
    settings.chatPath = rowValue(data, "model-config", "chat-path");
    settings.apiKeyEnv = rowValue(data, "model-config", "api-key-env");
    settings.apiKeyStored = rowValue(data, "model-config", "api-key-source") == "stored-file";
    settings.apiKeyRuntime = rowValue(data, "model-config", "api-key-runtime");
    settings.customCommand = rowValue(data, "model-config", "custom-command");
    settings.personalModelPath = rowValue(data, "model-config", "personal-model");
    settings.dumpPath = rowValue(data, "model-config", "dump-path");
    settings.llamaCommand = rowValue(data, "model-config", "llama-command");
    settings.llamaThreads = rowValue(data, "model-config", "llama-threads");
    settings.llamaContext = rowValue(data, "model-config", "llama-context");
    settings.timeout = rowValue(data, "model-config", "timeout");
    settings.httpTimeout = rowValue(data, "model-config", "http-timeout");
    settings.temperature = rowValue(data, "model-config", "temperature");
    settings.maxTokens = rowValue(data, "model-config", "max-tokens");
    settings.continuous = rowValue(data, "model-config", "continuous-default") == "1";
    settings.trainingContext = rowValue(data, "model-config", "training-context") == "1";
    settings.trainingSurrounding = rowValue(data, "model-config", "training-surrounding") == "1";
    settings.cloudRecentInput = rowValue(data, "model-config", "send-recent-input") == "1";
    settings.cloudSurrounding = rowValue(data, "model-config", "send-surrounding") == "1";
    return settings;
}

std::vector<std::string> modelConfigArguments(const ModelSettings &settings) {
    std::vector<std::string> args = {"--write", settings.mode};
    const bool endpointMode = settings.mode == "ollama" || settings.mode == "openai" ||
                              settings.mode == "openai-compatible";
    const bool cloudMode = settings.mode == "openai" || settings.mode == "openai-compatible";
    const bool namedModelMode = endpointMode || settings.mode == "llama-cpp";
    if (endpointMode && settings.mode != "openai" && !settings.baseUrl.empty()) {
        args.insert(args.end(), {"--base-url", settings.baseUrl});
    }
    if (namedModelMode && !settings.model.empty()) {
        args.insert(args.end(), {"--model", settings.model});
    }
    if (endpointMode && settings.mode != "openai" && !settings.chatPath.empty()) {
        args.insert(args.end(), {"--chat-path", settings.chatPath});
    }
    if (cloudMode && !settings.apiKey.empty()) {
        args.push_back("--api-key-stdin");
    } else if (cloudMode && settings.clearApiKey) {
        args.push_back("--clear-api-key");
    } else if (cloudMode && !settings.apiKeyEnv.empty()) {
        args.insert(args.end(), {"--api-key-env", settings.apiKeyEnv});
    }
    if (settings.mode == "custom" && !settings.customCommand.empty()) {
        args.insert(args.end(), {"--command", settings.customCommand});
    }
    if (settings.mode == "personal" && !settings.personalModelPath.empty()) {
        args.insert(args.end(), {"--personal-model", settings.personalModelPath});
    }
    if (settings.mode == "dump" && !settings.dumpPath.empty()) {
        args.insert(args.end(), {"--dump-path", settings.dumpPath});
    }
    if (settings.mode == "llama-cpp") {
        if (!settings.llamaCommand.empty()) {
            args.insert(args.end(), {"--llama-command", settings.llamaCommand});
        }
        if (!settings.llamaThreads.empty()) {
            args.insert(args.end(), {"--llama-threads", settings.llamaThreads});
        }
        if (!settings.llamaContext.empty()) {
            args.insert(args.end(), {"--llama-context", settings.llamaContext});
        }
    }
    if (settings.mode != "off" && !settings.timeout.empty()) {
        args.insert(args.end(), {"--timeout", settings.timeout});
    }
    if (endpointMode) {
        if (!settings.httpTimeout.empty()) {
            args.insert(args.end(), {"--http-timeout", settings.httpTimeout});
        }
        if (!settings.temperature.empty()) {
            args.insert(args.end(), {"--temperature", settings.temperature});
        }
        if (!settings.maxTokens.empty()) {
            args.insert(args.end(), {"--max-tokens", settings.maxTokens});
        }
    }
    if (cloudMode) {
        args.insert(args.end(), {"--send-recent-input", settings.cloudRecentInput ? "on" : "off"});
        args.insert(args.end(), {"--send-surrounding", settings.cloudSurrounding ? "on" : "off"});
    }
    args.insert(args.end(), {"--continuous", settings.continuous ? "on" : "off"});
    args.insert(args.end(), {"--training-context", settings.trainingContext ? "on" : "off"});
    args.insert(args.end(), {"--training-surrounding", settings.trainingSurrounding ? "on" : "off"});
    return args;
}

std::vector<std::string> personalTrainingArguments(const LearningPanelData &data) {
    std::vector<std::string> args = {data.personalTrainCommand};
    const auto settings = modelSettingsFrom(data);
    if (!settings.personalModelPath.empty()) {
        args.insert(args.end(), {"--output", settings.personalModelPath});
    }
    if (settings.trainingContext) {
        args.push_back("--include-context");
    }
    if (settings.trainingSurrounding) {
        args.push_back("--include-surrounding");
    }
    return args;
}

std::vector<std::string> split(std::string_view input, char delimiter) {
    std::vector<std::string> result;
    std::string current;
    for (const char ch : input) {
        if (ch == delimiter) {
            result.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    result.push_back(current);
    return result;
}

bool parsePanelLine(LearningPanelData &data, std::string_view line) {
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
    }
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return true;
    }
    const auto fields = split(line, '\t');
    if (fields.size() < 3 || fields[0] != "panel") {
        return false;
    }
    LearningPanelRow row;
    row.section = fields[1];
    row.key = fields[2];
    row.values.assign(fields.begin() + 3, fields.end());
    data.rows.push_back(std::move(row));
    return true;
}

bool loadPanelStream(LearningPanelData &data, std::istream &input) {
    std::string line;
    while (std::getline(input, line)) {
        if (!parsePanelLine(data, line)) {
            return false;
        }
    }
    return true;
}

bool loadPanelData(LearningPanelData &data) {
    data.rows.clear();
    if (data.inputPath.empty() || data.inputPath == "-") {
        return loadPanelStream(data, std::cin);
    }
    std::ifstream input(data.inputPath);
    return input && loadPanelStream(data, input);
}

std::string joinValues(const std::vector<std::string> &values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            result += '\t';
        }
        result += values[index];
    }
    return result;
}

std::string segmentSuffixCandidate(std::string_view committedText, std::string_view combinedCandidate) {
    if (committedText.empty() || combinedCandidate.size() <= committedText.size()) {
        return std::string(combinedCandidate);
    }
    if (combinedCandidate.rfind(committedText, 0) != 0) {
        return std::string(combinedCandidate);
    }
    return std::string(combinedCandidate.substr(committedText.size()));
}

const LearningPanelRow *findRow(const LearningPanelData &data, std::string_view section, std::string_view key) {
    for (const auto &row : data.rows) {
        if (row.section == std::string(section) && row.key == std::string(key)) {
            return &row;
        }
    }
    return nullptr;
}

std::vector<const LearningPanelRow *> findRows(const LearningPanelData &data, std::string_view section,
                                               std::string_view key) {
    std::vector<const LearningPanelRow *> rows;
    for (const auto &row : data.rows) {
        if (row.section == std::string(section) && row.key == std::string(key)) {
            rows.push_back(&row);
        }
    }
    return rows;
}

std::string modelKindText(std::string_view value) {
    if (value == "offline-heuristic") {
        return "离线启发式";
    }
    if (value == "custom-wrapper") {
        return "自定义模型脚本";
    }
    if (value == "personal-reranker") {
        return "TiP";
    }
    if (value == "local-llama-cpp") {
        return "本地 llama.cpp";
    }
    if (value == "request-dump") {
        return "请求转储";
    }
    if (value == "disabled") {
        return "已关闭";
    }
    if (value.rfind("local-http:", 0) == 0) {
        return "本地 HTTP：" + std::string(value.substr(11));
    }
    if (value.rfind("official-openai:", 0) == 0) {
        return "官方 OpenAI：" + std::string(value.substr(16));
    }
    if (value.rfind("openai-compatible:", 0) == 0) {
        return "OpenAI 兼容：" + std::string(value.substr(18));
    }
    return std::string(value);
}

std::string modelConfigKeyText(std::string_view key) {
    if (key == "configured-mode") {
        return "配置模式";
    }
    if (key == "backend") {
        return "后端";
    }
    if (key == "kind") {
        return "类型";
    }
    if (key == "timeout") {
        return "分析超时（秒）";
    }
    if (key == "http-timeout") {
        return "网络请求超时（秒）";
    }
    if (key == "temperature") {
        return "模型温度";
    }
    if (key == "max-tokens") {
        return "最大输出 token";
    }
    if (key == "invocation") {
        return "调用方式";
    }
    if (key == "model") {
        return "模型";
    }
    if (key == "base-url") {
        return "接口地址";
    }
    if (key == "chat-path") {
        return "接口路径";
    }
    if (key == "api-key-env") {
        return "API 密钥变量";
    }
    if (key == "api-key-runtime") {
        return "密钥运行时状态";
    }
    if (key == "custom-command") {
        return "自定义命令";
    }
    if (key == "personal-model") {
        return "TiP 模型文件";
    }
    if (key == "personal-model-name") {
        return "模型名称";
    }
    if (key == "personal-model-status") {
        return "TiP 状态";
    }
    if (key == "personal-model-architecture") {
        return "TiP 结构";
    }
    if (key == "personal-model-feature-version") {
        return "TiP 特征版本";
    }
    if (key == "personal-model-pinyin-prior-entries") {
        return "拼音先验条目";
    }
    if (key == "personal-model-training-pinyin-prior-sources") {
        return "拼音先验来源数";
    }
    if (key == "personal-model-training-samples") {
        return "TiP 训练样本";
    }
    if (key == "personal-model-training-ranking-samples") {
        return "TiP 排序样本";
    }
    if (key == "personal-model-training-chinese-ranking-samples") {
        return "中文排序样本";
    }
    if (key == "personal-model-training-correction-only-samples") {
        return "纠错专用样本";
    }
    if (key == "personal-model-training-non-leading-samples") {
        return "非首选训练样本";
    }
    if (key == "personal-model-pair-evidence") {
        return "已记录选择对";
    }
    if (key == "personal-model-active-pair-evidence") {
        return "重复选择证据";
    }
    if (key == "personal-model-raw-token-evidence" ||
        key == "personal-model-training-raw-token-evidence-entries") {
        return "已记忆英文词";
    }
    if (key == "personal-model-active-raw-token-evidence" ||
        key == "personal-model-training-active-raw-token-evidence") {
        return "生效英文词记忆";
    }
    if (key == "personal-model-active-correction-patterns") {
        return "生效纠错模式";
    }
    if (key == "personal-model-active-key-habits") {
        return "生效全局按键习惯";
    }
    if (key == "personal-model-training-validation-strategy") {
        return "验证集策略";
    }
    if (key == "personal-model-training-validation-accuracy") {
        return "留出集准确率";
    }
    if (key == "personal-model-training-validation-baseline-accuracy") {
        return "首项基线准确率";
    }
    if (key == "personal-model-training-validation-gain") {
        return "验证收益";
    }
    if (key == "personal-model-training-validation-non-leading-samples") {
        return "验证非首选样本";
    }
    if (key == "personal-model-training-validation-non-leading-correct") {
        return "验证非首选命中";
    }
    if (key == "personal-model-training-validation-non-leading-accuracy") {
        return "验证非首选准确率";
    }
    if (key == "personal-model-training-validation-leading-samples") {
        return "验证原首选样本";
    }
    if (key == "personal-model-training-validation-leading-correct") {
        return "验证原首选保留";
    }
    if (key == "personal-model-training-validation-generic-non-leading-samples") {
        return "真实泛化验证样本";
    }
    if (key == "personal-model-training-validation-generic-non-leading-correct") {
        return "真实泛化验证命中";
    }
    if (key == "personal-model-training-validation-generic-non-leading-accuracy") {
        return "真实泛化验证准确率";
    }
    if (key == "personal-model-training-validation-generic-excluded-direct-evidence") {
        return "排除已有证据样本";
    }
    if (key == "personal-model-training-validation-generic-excluded-seen-preedit") {
        return "排除已见拼音样本";
    }
    if (key == "personal-model-training-validation-generic-excluded-raw-candidate") {
        return "排除英文原样样本";
    }
    if (key == "personal-model-training-validation-generic-excluded-derived-prefix") {
        return "排除派生前缀样本";
    }
    if (key == "personal-model-training-raw-profile-samples") {
        return "英文原样监督样本";
    }
    if (key == "personal-model-training-raw-profile-accepted-samples") {
        return "英文原样接受样本";
    }
    if (key == "personal-model-training-raw-profile-rejected-samples") {
        return "英文原样拒绝样本";
    }
    if (key == "personal-model-training-raw-profile-auxiliary-positive-samples") {
        return "英文模式辅助正样本";
    }
    if (key == "personal-model-training-raw-profile-validation-samples") {
        return "英文原样验证样本";
    }
    if (key == "personal-model-training-raw-profile-validation-correct") {
        return "英文原样验证命中";
    }
    if (key == "personal-model-training-raw-profile-validation-false-promotions") {
        return "英文原样错误提前";
    }
    if (key == "personal-model-training-raw-profile-validation-accuracy") {
        return "英文原样验证准确率";
    }
    if (key == "personal-model-training-raw-profile-recommendation") {
        return "英文原样能力建议";
    }
    if (key == "personal-model-keyboard-correction-safe") {
        return "键盘习惯纠错已解锁";
    }
    if (key == "personal-model-raw-profile-safe") {
        return "英文原样泛化已解锁";
    }
    if (key == "personal-model-generic-ranking-safe") {
        return "泛化改序已解锁";
    }
    if (key == "personal-model-component-update-safe") {
        return "安全组件可升级";
    }
    if (key == "personal-model-evidence-merge-strategy") {
        return "长期证据合并策略";
    }
    if (key == "personal-model-training-recommendation") {
        return "TiP 建议";
    }
    if (key == "personal-model-promotion-margin") {
        return "改序置信余量";
    }
    if (key == "llama-command") {
        return "llama.cpp 命令";
    }
    if (key == "llama-command-valid") {
        return "llama.cpp 可执行";
    }
    if (key == "llama-model-readable") {
        return "GGUF 模型可读";
    }
    if (key == "llama-threads") {
        return "本地模型线程数";
    }
    if (key == "llama-context") {
        return "本地模型上下文";
    }
    if (key == "dump-path") {
        return "转储路径";
    }
    if (key == "continuous-default") {
        return "连续模式默认值";
    }
    if (key == "training-context") {
        return "TiP 训练使用近期提交上下文";
    }
    if (key == "training-surrounding") {
        return "TiP 训练使用光标周围上下文";
    }
    if (key == "send-recent-input") {
        return "云端接收近期按键和修改记录";
    }
    if (key == "send-surrounding") {
        return "云端接收光标附近文字和应用名称";
    }
    if (key == "configured-command") {
        return "配置文件模型命令";
    }
    if (key == "configured-command-valid") {
        return "配置文件模型入口有效";
    }
    if (key == "process-command") {
        return "当前进程模型命令";
    }
    if (key == "process-command-scope") {
        return "命令环境范围";
    }
    if (key == "process-command-active-scope") {
        return "模型入口状态范围";
    }
    if (key == "runtime-verification") {
        return "运行时核验";
    }
    if (key == "process-command-active") {
        return "当前命令环境使用模型入口";
    }
    if (key == "active-command") {
        return "当前进程模型命令";
    }
    if (key == "config-active") {
        return "当前命令环境使用模型入口";
    }
    if (key == "activation-hint") {
        return "启用提示";
    }
    if (key == "analyze-window") {
        return "分析窗口";
    }
    if (key == "supervision-window") {
        return "监督窗口";
    }
    if (key == "analyze-learn") {
        return "学习分析命令";
    }
    if (key == "self-test-command") {
        return "模型自测";
    }
    if (key == "dry-run-test-command") {
        return "无网络自测";
    }
    if (key == "dry-run-test-supported") {
        return "支持无网络自测";
    }
    return std::string(key);
}

std::string metricText(std::string_view key) {
    if (key == "rows") {
        return "行";
    }
    if (key == "legacy-preferences") {
        return "旧偏好";
    }
    if (key == "candidates") {
        return "候选";
    }
    if (key == "corrections") {
        return "纠错";
    }
    if (key == "correction-patterns") {
        return "纠错习惯";
    }
    if (key == "preferences") {
        return "偏好";
    }
    if (key == "segment-chains") {
        return "分段链";
    }
    if (key == "pending-segments") {
        return "待确认分段";
    }
    if (key == "path") {
        return "路径";
    }
    if (key == "learn-output") {
        return "写入学习";
    }
    if (key == "trigger") {
        return "触发";
    }
    if (key == "visible") {
        return "当前显示";
    }
    if (key == "numbered") {
        return "带数字";
    }
    if (key == "correction-events") {
        return "纠错事件";
    }
    if (key == "context") {
        return "上下文";
    }
    return std::string(key);
}

std::string valueText(std::string_view value) {
    if (value == "none") {
        return "无";
    }
    if (value == "on-demand-single-process") {
        return "按需启动一次，完成后退出";
    }
    if (value ==
        "hashed-pairwise-ranker+personal-edit-channel+raw-token-memory+raw-offer-profile+pinyin-prior") {
        return "候选成对排序 + 个人按键错误通道 + 英文词记忆 + 英文原样分类 + 拼音先验";
    }
    if (value == "hashed-pairwise-ranker+personal-edit-channel+raw-offer-profile+pinyin-prior") {
        return "候选成对排序 + 个人按键错误通道 + 英文原样分类 + 拼音先验";
    }
    if (value == "hashed-pairwise-ranker+personal-edit-channel+pinyin-prior") {
        return "候选成对排序 + 个人按键错误通道 + 拼音先验";
    }
    if (value == "legacy-hashed-pairwise-ranker") {
        return "旧版候选成对排序";
    }
    if (value == "full-delete-retype") {
        return "全部删除后重打";
    }
    if (value == "partial-rewrite") {
        return "部分重写";
    }
    if (value == "middle-edit") {
        return "中间编辑";
    }
    if (value == "ui-and-short-action-order") {
        return "界面和短操作顺序";
    }
    if (value == "delete-retype-and-middle-edit-learning") {
        return "删除重打和中间编辑学习";
    }
    if (value == "waiting-for-current-input") {
        return "等待当前输入";
    }
    if (value == "live-supervision") {
        return "当前实时输入";
    }
    if (value == "last-supervision") {
        return "上一次输入快照";
    }
    if (value == "history-supervision") {
        return "近期历史输入";
    }
    if (value == "request-file") {
        return "请求文件";
    }
    if (value == "waiting-for-live-supervision") {
        return "等待当前实时输入";
    }
    if (value == "showing-last-supervision") {
        return "上一次输入快照";
    }
    if (value == "showing-history-supervision") {
        return "近期历史输入";
    }
    if (value == "active-preedit") {
        return "活动拼音";
    }
    if (value == "active") {
        return "已生效";
    }
    if (value == "inactive-evidence") {
        return "尚未生效";
    }
    if (value == "pass-through-only") {
        return "仅按键监督";
    }
    if (value == "keyboard-context-only") {
        return "仅键盘上下文";
    }
    if (value == "ready-to-learn") {
        return "可学习";
    }
    if (value == "awaiting-suffix-confirmation") {
        return "等待后缀确认";
    }
    if (value == "selected-candidate-already-top") {
        return "选中候选已在首位";
    }
    if (value == "selected_candidate_prefix") {
        return "前缀候选选择";
    }
    if (value == "selected_candidate") {
        return "候选选择";
    }
    if (value == "history_correction") {
        return "历史纠错";
    }
    if (value == "possible_correction") {
        return "疑似纠错";
    }
    if (value == "known_correction") {
        return "已知纠错";
    }
    if (value == "current_typo") {
        return "当前像输错项";
    }
    if (value == "current_corrected_preedit") {
        return "当前是纠正目标";
    }
    if (value == "supervised_evidence") {
        return "监督证据";
    }
    if (value == "recent_supervised") {
        return "近期监督";
    }
    if (value == "prefix-only-no-preference") {
        return "前缀候选不学习整串";
    }
    if (value == "rank-only") {
        return "仅排序参考";
    }
    if (value == "no-action") {
        return "无动作";
    }
    if (value == "wait-for-active-preedit") {
        return "等待活动拼音";
    }
    if (value == "prefer-suggested-protocols") {
        return "优先使用建议学习行";
    }
    if (value == "wait-for-selected-suffix") {
        return "等待选中后缀";
    }
    if (value == "no-persistent-preference-needed") {
        return "不需要持久偏好";
    }
    if (value == "rerank-only-with-stronger-evidence") {
        return "只有更强证据才重排";
    }
    if (value == "do-not-emit-output") {
        return "不要输出";
    }
    if (value == "already-known-or-no-new-safe-row") {
        return "已存在或没有新的安全学习行";
    }
    if (value == "empty-output") {
        return "模型没有输出";
    }
    if (value == "current-shell-environment") {
        return "当前 shell 环境";
    }
    if (value == "current-shell-only-not-fcitx5-runtime") {
        return "仅当前 shell，未进入 fcitx5 运行时";
    }
    if (value == "runtime section below is authoritative when available") {
        return "以运行时检查结果为准";
    }
    if (value == "unset") {
        return "未设置";
    }
    if (value == "offline-heuristic") {
        return "离线启发式";
    }
    if (value == "custom-wrapper") {
        return "自定义包装器";
    }
    if (value == "personal-reranker" || value == "personal") {
        return "TiP";
    }
    if (value == "untrained") {
        return "尚未训练";
    }
    if (value == "invalid") {
        return "模型文件无效";
    }
    if (value == "unavailable") {
        return "检查工具不可用";
    }
    if (value == "keep-heuristic") {
        return "继续使用离线启发式";
    }
    if (value == "collect-more-data") {
        return "继续收集样本";
    }
    if (value == "stratified-temporal-v1") {
        return "按首选结果分层并优先保留较新样本";
    }
    if (value == "evidence-isolated-temporal-v2") {
        return "分层保留较新样本，并隔离已有证据与已见拼音";
    }
    if (value == "capability-isolated-temporal-v3") {
        return "按能力分层验证，并隔离英文原样与已有证据";
    }
    if (value == "capability-isolated-temporal-v4") {
        return "按能力分层验证，并隔离派生前缀、英文原样与已有证据";
    }
    if (value == "ready") {
        return "可用";
    }
    if (value == "manual") {
        return "手动";
    }
    if (value == "Analyze") {
        return "分析";
    }
    if (value == "applied") {
        return "应用";
    }
    if (value == "guarded") {
        return "护栏拦截";
    }
    if (value == "skipped") {
        return "跳过";
    }
    if (value == "ok") {
        return "可用";
    }
    if (value == "long-preedit") {
        return "长句交给点击分析";
    }
    if (value == "known-raw-english") {
        return "已知英文直出";
    }
    if (value == "known-english-token") {
        return "已知英文词";
    }
    if (value == "learned-raw-preference") {
        return "已学习英文直出";
    }
    if (value == "english-identifier") {
        return "像英文标识符";
    }
    if (value == "weak-pattern") {
        return "次数不足";
    }
    if (value == "not-applicable") {
        return "不适用";
    }
    if (value == "out-of-range") {
        return "位置越界";
    }
    if (value == "already-present") {
        return "目标已存在";
    }
    if (value == "not-present") {
        return "目标不存在";
    }
    if (value == "typo") {
        return "输错项";
    }
    if (value == "corrected") {
        return "纠正目标";
    }
    if (value == "invalid-pattern") {
        return "模式无效";
    }
    if (value == "implausible") {
        return "纠错不可信";
    }
    if (value == "1") {
        return "是";
    }
    if (value == "0") {
        return "否";
    }
    return std::string(value);
}

std::string formatUnixTime(std::string_view value) {
    try {
        std::time_t timestamp = static_cast<std::time_t>(std::stoll(std::string(value)));
        std::tm localTime{};
        if (!localtime_r(&timestamp, &localTime)) {
            return std::string(value);
        }
        char buffer[32]{};
        if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime) == 0) {
            return std::string(value);
        }
        return buffer;
    } catch (...) {
        return std::string(value);
    }
}

std::string eventKindText(std::string_view key) {
    if (key == "letter") {
        return "字母";
    }
    if (key == "digit") {
        return "数字";
    }
    if (key == "symbol") {
        return "符号";
    }
    if (key == "space") {
        return "空格";
    }
    if (key == "backspace") {
        return "退格";
    }
    if (key == "delete") {
        return "删除";
    }
    if (key == "cursor-move") {
        return "光标移动";
    }
    if (key == "candidate-selected") {
        return "选择候选";
    }
    if (key == "observed") {
        return "观察";
    }
    if (key == "rerank-requested") {
        return "请求分析";
    }
    if (key == "enter") {
        return "回车";
    }
    if (key == "escape") {
        return "取消";
    }
    return std::string(key);
}

std::string keyDisplayText(std::string_view key) {
    if (key.empty()) {
        return {};
    }
    if (key == "Down" || key == "KP_Down") {
        return "下方向键";
    }
    if (key == "Up" || key == "KP_Up") {
        return "上方向键";
    }
    if (key == "Left" || key == "KP_Left") {
        return "左方向键";
    }
    if (key == "Right" || key == "KP_Right") {
        return "右方向键";
    }
    if (key == "PageDown" || key == "Page_Down" || key == "Next" || key == "KP_Page_Down" ||
        key == "KP_Next") {
        return "下一页";
    }
    if (key == "PageUp" || key == "Page_Up" || key == "Prior" || key == "KP_Page_Up" ||
        key == "KP_Prior") {
        return "上一页";
    }
    if (key == "Home" || key == "KP_Home") {
        return "回到开头";
    }
    if (key == "End" || key == "KP_End") {
        return "跳到末尾";
    }
    if (key == "Tab") {
        return "Tab";
    }
    if (key == "ShiftTab" || key == "Shift+Tab" || key == "ISO_Left_Tab") {
        return "Shift+Tab";
    }
    if (key == "BackSpace" || key == "Backspace") {
        return "退格";
    }
    if (key == "Delete") {
        return "删除";
    }
    if (key == "space" || key == "Space") {
        return "空格";
    }
    if (key == "Return" || key == "Enter" || key == "KP_Enter") {
        return "回车";
    }
    if (key == "Escape" || key == "Esc") {
        return "取消";
    }
    if (key == "WindowSwitch") {
        return "切换窗口";
    }
    if (key == "InputMethodSwitch") {
        return "切换输入法";
    }
    if (key == "start") {
        return "开始监督";
    }
    return std::string(key);
}

std::string eventCountSummary(const LearningPanelData &data, std::string_view key, std::string_view label) {
    const auto rows = findRows(data, "supervision", key);
    if (rows.empty()) {
        return {};
    }
    std::string result(label);
    result += "：";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto *row = rows[index];
        if (!row || row->values.size() < 2) {
            continue;
        }
        if (index > 0) {
            result += "，";
        }
        result += eventKindText(row->values[0]) + " " + row->values[1];
    }
    return result == std::string(label) + "：" ? std::string{} : result;
}

std::string metricValue(const LearningPanelRow &row, std::string_view key) {
    for (std::size_t index = 0; index + 1 < row.values.size(); index += 2) {
        if (row.values[index] == std::string(key)) {
            return row.values[index + 1];
        }
    }
    return {};
}

bool analyzeLearns(const LearningPanelData &data) {
    if (const auto *row = findRow(data, "model-replay", "mode")) {
        for (std::size_t index = 1; index + 1 < row->values.size(); index += 2) {
            if (row->values[index] == "learn-output") {
                return row->values[index + 1] == "1";
            }
        }
    }
    return false;
}

std::string analyzeButtonText(const LearningPanelData &data) {
    return analyzeLearns(data) ? "分析当前输入" : "预览当前输入";
}

std::string refreshHintText(const LearningPanelData &data) {
    if (data.refreshCommand.empty()) {
        return {};
    }
    return analyzeLearns(data) ? "；“分析当前输入”会运行一次模型，安全结果写入学习"
                               : "；“预览当前输入”只运行一次模型";
}

std::string analysisStatusText(const LearningPanelData &data) {
    const auto *status = findRow(data, "model-replay", "status");
    if (!status) {
        return {};
    }
    if (status->values.empty() || status->values[0] != "ok") {
        return "分析失败或模型输出不可用";
    }

    const auto *summary = findRow(data, "model-output", "summary");
    if (!summary) {
        return "分析完成：没有模型输出";
    }
    const auto rows = metricValue(*summary, "rows");
    const auto candidates = metricValue(*summary, "candidates");
    const auto corrections = metricValue(*summary, "corrections");
    const auto preferences = metricValue(*summary, "preferences");
    const auto segmentChains = metricValue(*summary, "segment-chains");
    std::string result = "分析完成：";
    result += "输出 " + (rows.empty() ? "0" : rows);
    result += "，候选 " + (candidates.empty() ? "0" : candidates);
    result += "，纠错 " + (corrections.empty() ? "0" : corrections);
    result += "，偏好 " + (preferences.empty() ? "0" : preferences);
    result += "，分段链 " + (segmentChains.empty() ? "0" : segmentChains);

    if (const auto *learned = findRow(data, "model-output", "learned")) {
        const auto learnedPreferences = metricValue(*learned, "preferences");
        const auto learnedCorrections = metricValue(*learned, "corrections");
        const auto learnedSegmentChains = metricValue(*learned, "segment-chains");
        result += "；已写入学习：偏好 " + (learnedPreferences.empty() ? "0" : learnedPreferences);
        result += "，纠错 " + (learnedCorrections.empty() ? "0" : learnedCorrections);
        result += "，分段链 " + (learnedSegmentChains.empty() ? "0" : learnedSegmentChains);
    } else if (analyzeLearns(data)) {
        result += "；没有写入学习";
    }
    return result;
}

std::string correctionPatternKindText(std::string_view key) {
    if (key == "missing") {
        return "漏字";
    }
    if (key == "extra") {
        return "多字";
    }
    if (key == "replace") {
        return "替换";
    }
    return std::string(key);
}

std::string learningEffectText(const LearningPanelRow &row, bool includeLineNumber) {
    if (row.values.empty()) {
        return {};
    }
    const auto &effect = row.values[0];
    std::string lineSuffix;
    if (includeLineNumber && row.values.size() >= 2) {
        lineSuffix = "，行 " + row.values[1];
    }
    if (effect == "rank-preference" && row.values.size() >= 5) {
        return "下次排序：" + row.values[2] + " 会优先 " + row.values[3] + "（" + row.values[4] +
               " 次" + lineSuffix + "）";
    }
    if (effect == "raw-preference" && row.values.size() >= 5) {
        return "下次英文直出：" + row.values[2] + " 会优先保留原文（" + row.values[4] + " 次" +
               lineSuffix + "）";
    }
    if (effect == "legacy-raw-preference" && row.values.size() >= 4) {
        return "下次旧式原文偏好：" + row.values[2] + "（" + row.values[3] + " 次" + lineSuffix + "）";
    }
    if (effect == "supervised-raw-token" && row.values.size() >= 4) {
        return "下次英文直出：" + row.values[2] + " 已由英文模式确认（" + row.values[3] + " 次" +
               lineSuffix + "）";
    }
    if (effect == "correction-borrow" && row.values.size() >= 5) {
        return "下次纠错：" + row.values[2] + " 会借用 " + row.values[3] + " 的候选（" +
               row.values[4] + " 次" + lineSuffix + "）";
    }
    if (effect == "correction-target" && row.values.size() >= 5) {
        return "纠错目标：" + row.values[3] + " 已被 " + row.values[2] + " 借用（" + row.values[4] +
               " 次" + lineSuffix + "）";
    }
    if ((effect == "segment-chain-full" || effect == "segment-chain-suffix" ||
         effect == "segment-chain-corrected-full") &&
        row.values.size() >= 9) {
        std::string text;
        if (effect == "segment-chain-suffix") {
            text = "下次后缀续选：" + row.values[5] + " 可接在 " + row.values[4] + " 后，合并 " +
                   row.values[7];
        } else if (effect == "segment-chain-corrected-full") {
            text = "下次纠错分段：" + row.values[2] + " -> " + row.values[6] + "，合并 " +
                   row.values[7];
        } else {
            text = "下次分段整句：" + row.values[2] + " 可按 " + row.values[3] + " + " +
                   row.values[5] + " 续成 " + row.values[7];
        }
        return text + "（" + row.values[8] + " 次" + lineSuffix + "）";
    }
    return "学习效果：" + joinValues(row.values);
}

std::vector<std::string> summaryLinesFor(const LearningPanelData &data) {
    std::vector<std::string> lines;
    const auto *statusRow = findRow(data, "state", "status");
    const bool waitingForLive = statusRow && !statusRow->values.empty() &&
                                statusRow->values[0] == "waiting-for-live-supervision";
    const bool showingLast = statusRow && !statusRow->values.empty() &&
                             statusRow->values[0] == "showing-last-supervision";
    const bool showingHistory = statusRow && !statusRow->values.empty() &&
                                statusRow->values[0] == "showing-history-supervision";
    if (waitingForLive) {
        lines.push_back("等待当前 TiPE 输入");
        lines.push_back("开始输入拼音后，这个窗口会自动刷新。");
    } else if (showingLast) {
        lines.push_back("正在显示上一次 TiPE 输入");
        lines.push_back("开始输入拼音后，会切换到当前实时输入。");
    } else if (showingHistory) {
        lines.push_back("正在显示近期历史里的最后一次 TiPE 输入");
        lines.push_back("开始输入拼音后，会切换到当前实时输入。");
    }
    if (const auto *row = findRow(data, "state", "request-source"); row && row->values.size() >= 2) {
        lines.push_back("快照来源：" + valueText(row->values[0]) + "（" + row->values[1] + "）");
    }
    if (const auto *row = findRow(data, "state", "request-mtime"); row && !row->values.empty()) {
        lines.push_back("快照时间：" + formatUnixTime(row->values[0]));
    }
    if (const auto *row = findRow(data, "state", "preedit"); row && !row->values.empty()) {
        lines.push_back(row->values[0].empty() ? "正在输入：无活动拼音" : "正在输入：" + row->values[0]);
    } else {
        lines.push_back("正在输入：无活动拼音");
    }
    if (const auto *row = findRow(data, "state", "surrounding-before"); row && !row->values.empty() &&
                                                                    !row->values[0].empty()) {
        lines.push_back("光标前文：" + row->values[0]);
    }
    if (const auto *row = findRow(data, "state", "surrounding-after"); row && !row->values.empty() &&
                                                                   !row->values[0].empty()) {
        lines.push_back("光标后文：" + row->values[0]);
    }

    if (const auto *row = findRow(data, "candidates", "total"); row && row->values.size() >= 5) {
        lines.push_back("候选：" + row->values[0] + " 个，总计；当前显示 " + row->values[2] + " 个；带数字 " +
                        row->values[4] + " 个");
    }
    if (const auto *row = findRow(data, "candidates", "first"); row && row->values.size() >= 2) {
        lines.push_back("首选候选：" + row->values[1]);
    }
    if (const auto *row = findRow(data, "candidates", "selected"); row && row->values.size() >= 2) {
        lines.push_back("当前选中：" + row->values[1]);
    }
    if (const auto *row = findRow(data, "learning", "selected-candidate-signal");
        row && row->values.size() >= 4) {
        if (row->values[3] == "already-top") {
            lines.push_back("选中候选已经是首选，不需要学习排序");
        } else if (row->values[3] == "would-learn-preference") {
            lines.push_back("分析可学习排序：" + row->values[0] + " -> " + row->values[2]);
        } else if (row->values[3] == "prefix-only-no-preference") {
            lines.push_back("选中候选只提交前缀，等待后续分段链确认，不学习整串排序");
        }
    }
    if (const auto *row = findRow(data, "learning", "status"); row && row->values.size() >= 3) {
        lines.push_back("学习状态：" + valueText(row->values[0]) + "，下一步：" + valueText(row->values[2]));
    }
    if (const auto *row = findRow(data, "learning", "status-suggested-protocol"); row && row->values.size() >= 2) {
        lines.push_back("建议学习行：" + joinValues(std::vector<std::string>(row->values.begin() + 1, row->values.end())));
    }
    if (const auto *row = findRow(data, "learning", "status-awaiting-suffix"); row && row->values.size() >= 5) {
        lines.push_back("等待后缀确认：" + row->values[1] + " 已先选 " + row->values[3] +
                        "，当前剩余 " + row->values[4]);
    }
    if (const auto *row = findRow(data, "learning", "segment-chain-signal"); row && row->values.size() >= 8) {
        std::string line = "分段链证据：" + row->values[1] + " 先选 " + row->values[3] +
                           "，剩余 " + row->values[4] + "，合并 " + row->values[6];
        if (row->values[7] == "suffix-continuation" || row->values[7] == "suffix-correction-chain") {
            line = "当前剩余拼音可借用分段链：" + row->values[3] + " + " + row->values[4] +
                   " -> " + row->values[6];
        }
        if (row->values[7] == "correction-chain" || row->values[7] == "suffix-correction-chain") {
            line += "，并提示纠错到 " + row->values[5];
        }
        lines.push_back(line);
    }
    if (const auto *row = findRow(data, "learning", "pending-segment-signal"); row && row->values.size() >= 6) {
        if (row->values[5] == "confirmed-suffix" && row->values.size() >= 9) {
            lines.push_back("分段链可学习：" + row->values[1] + " 已先选 " + row->values[3] +
                            "，当前后缀 " + row->values[4] + " 选 " + row->values[6] +
                            "，合并 " + row->values[8]);
        } else {
            lines.push_back("分段链等待确认：" + row->values[1] + " 已先选 " + row->values[3] +
                            "，当前剩余 " + row->values[4]);
        }
    }
    if (const auto *row = findRow(data, "learning", "correction-signal"); row && row->values.size() >= 4) {
        lines.push_back("纠错可学习：" + row->values[2] + " -> " + row->values[3] + "（" +
                        valueText(row->values[1]) + "）");
    }
    if (const auto *row = findRow(data, "learning", "evidence-summary"); row && row->values.size() >= 9) {
        lines.push_back("当前拼音已学证据：" + row->values[0] + "，排序 " + row->values[2] +
                        "，纠错 " + row->values[6] + "，分段链 " + row->values[8]);
    }
    if (const auto *row = findRow(data, "learning", "evidence-supervised-raw-token");
        row && row->values.size() >= 4) {
        lines.push_back("英文模式已确认：" + row->values[1] + "（" + row->values[2] + " 次）");
    }
    if (const auto *row = findRow(data, "learning", "evidence-preference"); row && row->values.size() >= 4) {
        lines.push_back("已学排序：" + row->values[1] + " -> " + row->values[2] + "（" +
                        row->values[3] + " 次）");
    }
    if (const auto *row = findRow(data, "learning", "evidence-correction"); row && row->values.size() >= 5) {
        lines.push_back("已学纠错：" + row->values[2] + " -> " + row->values[3] + "（" +
                        row->values[4] + " 次）");
    }
    if (const auto *row = findRow(data, "learning", "evidence-segment-chain"); row && row->values.size() >= 8) {
        lines.push_back("已学分段链：" + row->values[1] + " 先选 " + row->values[3] +
                        "，剩余 " + row->values[4] + "，合并 " + row->values[6] +
                        "（" + row->values[7] + " 次）");
    }
    for (const auto *row : findRows(data, "learning", "evidence-effect")) {
        if (auto text = learningEffectText(*row, false); !text.empty()) {
            lines.push_back(text);
        }
    }

    if (const auto *row = findRow(data, "supervision", "recent-events"); row && row->values.size() >= 6) {
        lines.push_back("已监督：" + row->values[0] + " 个近期按键，" + row->values[2] +
                        " 个纠错记忆事件，" + row->values[4] + " 个上下文提交");
    }
    if (const auto *row = findRow(data, "supervision", "mode"); row && !row->values.empty()) {
        const auto *inputMode = findRow(data, "supervision", "input-mode");
        const bool englishPassThrough = inputMode && !inputMode->values.empty() &&
                                        inputMode->values[0] == "english";
        lines.push_back(row->values[0] == "pass-through-only"
                            ? (englishPassThrough
                                   ? "监督模式：英文直通；按键原样输出，同时记录有界词元和纠错操作"
                                   : "监督模式：仅按键行为；正在观察按键习惯，开始拼音后才分析候选")
                            : "监督模式：活动拼音和候选");
    }
    if (const auto *row = findRow(data, "supervision", "state"); row && row->values.size() >= 6) {
        lines.push_back("监督状态：" + valueText(row->values[1]) + "，近期事件 " + row->values[3] +
                        "，纠错轨迹 " + row->values[5]);
    }
    if (auto recentCountLine = eventCountSummary(data, "event-count", "按键类型"); !recentCountLine.empty()) {
        lines.push_back(recentCountLine);
    }
    if (auto correctionCountLine = eventCountSummary(data, "correction-event-count", "纠错轨迹类型");
        !correctionCountLine.empty()) {
        lines.push_back(correctionCountLine);
    }
    if (const auto *row = findRow(data, "supervision", "runtime-state"); row && row->values.size() >= 2 &&
                                                                       row->values[0] == "continuous") {
        lines.push_back(row->values[1] == "1" ? "连续监督：开启" : "连续监督：关闭");
    }
    if (const auto *row = findRow(data, "history", "summary"); row && row->values.size() >= 10) {
        const auto records = metricValue(*row, "records");
        const auto active = metricValue(*row, "active");
        const auto passThrough = metricValue(*row, "pass-through");
        const auto segmentChains = metricValue(*row, "segment-chains");
        lines.push_back("近期历史：记录 " + (records.empty() ? "0" : records) +
                        "，活动拼音 " + (active.empty() ? "0" : active) +
                        "，纯按键 " + (passThrough.empty() ? "0" : passThrough) +
                        "，分段链 " + (segmentChains.empty() ? "0" : segmentChains));
    }
    if (const auto *row = findRow(data, "history", "preedit"); row && row->values.size() >= 3) {
        lines.push_back("近期常见输入：" + row->values[1] + "（" + row->values[2] + " 次）");
    }
    if (const auto *row = findRow(data, "history", "selected-candidate"); row && row->values.size() >= 3) {
        lines.push_back("近期常选候选：" + row->values[1] + "（" + row->values[2] + " 次）");
    }
    if (const auto *row = findRow(data, "history", "preedit-selected"); row && row->values.size() >= 4) {
        lines.push_back("近期输入选择：" + row->values[1] + " -> " + row->values[2] + "（" +
                        row->values[3] + " 次）");
    }
    if (const auto *row = findRow(data, "history", "correction"); row && row->values.size() >= 4) {
        lines.push_back("近期重复纠错：" + row->values[1] + " -> " + row->values[2] + "（" +
                        row->values[3] + " 次）");
    }

    if (const auto *kind = findRow(data, "model-config", "kind"); kind && !kind->values.empty()) {
        std::string modelLine = "分析后端：" + modelKindText(kind->values[0]);
        if (const auto *model = findRow(data, "model-config", "model"); model && !model->values.empty() &&
                                                                  !model->values[0].empty()) {
            modelLine += " / " + model->values[0];
        }
        lines.push_back(modelLine);
    }
    const auto *active = findRow(data, "model-config", "process-command-active");
    if (!active) {
        active = findRow(data, "model-config", "config-active");
    }
    if (active && !active->values.empty() && active->values[0] == "0") {
        if (const auto *hint = findRow(data, "model-config", "activation-hint"); hint && !hint->values.empty()) {
            lines.push_back("当前窗口进程未携带模型入口环境；fcitx5 运行状态以 tipe-doctor runtime 为准");
        }
    }
    const auto *dryRunSupported = findRow(data, "model-config", "dry-run-test-supported");
    const bool canDryRun = dryRunSupported && !dryRunSupported->values.empty() && dryRunSupported->values[0] == "1";
    if (canDryRun) {
        if (const auto *row = findRow(data, "model-config", "dry-run-test-command"); row && !row->values.empty()) {
            lines.push_back("模型可无网络自测：" + row->values[0]);
        }
    } else if (const auto *row = findRow(data, "model-config", "self-test-command"); row && !row->values.empty()) {
        lines.push_back("模型可自测：" + row->values[0]);
    } else if (const auto *row = findRow(data, "model-config", "dry-run-test-command"); row && !row->values.empty()) {
        lines.push_back("模型可无网络自测：" + row->values[0]);
    }

    if (const auto *row = findRow(data, "behavior", "possible-correction"); row && row->values.size() >= 4) {
        lines.push_back("可能的输错模式：" + row->values[2] + " -> " + row->values[3] + "（" +
                        valueText(row->values[1]) + "）");
    } else if (const auto *row = findRow(data, "behavior", "correction-pattern"); row && row->values.size() >= 5) {
        lines.push_back("最强纠错习惯：" + correctionPatternKindText(row->values[1]) + " " + row->values[2] +
                        "（位置 " + row->values[3] + "，次数 " + row->values[4] + "）");
    } else if (const auto *row = findRow(data, "top-correction-pattern", "1"); row && row->values.size() >= 4) {
        lines.push_back("最强已学习习惯：" + correctionPatternKindText(row->values[1]) + " " + row->values[2] +
                        "（位置 " + row->values[3] + "，次数 " + row->values[0] + "）");
    } else if (const auto *row = findRow(data, "top-correction", "1"); row && row->values.size() >= 3) {
        lines.push_back("最强已学习纠错：" + row->values[1] + " -> " + row->values[2] + "（次数 " +
                        row->values[0] + "）");
    }
    if (const auto *row = findRow(data, "behavior", "realtime-correction"); row && row->values.size() >= 8) {
        std::string line = "实时纠错规则：" + valueText(row->values[1]) + " " +
                           correctionPatternKindText(row->values[3]) + " " + row->values[4] +
                           "（" + valueText(row->values[2]) + "）";
        if (!row->values[7].empty()) {
            line += " -> " + row->values[7];
        }
        lines.push_back(line);
    }

    if (const auto *row = findRow(data, "top-preference", "1"); row && row->values.size() >= 3) {
        lines.push_back("最强已学习选择：" + row->values[1] + " -> " + row->values[2] + "（次数 " +
                        row->values[0] + "）");
    }

    if (const auto *row = findRow(data, "model-output", "summary"); row && row->values.size() >= 10) {
        lines.push_back("本次分析接受：候选 " + metricValue(*row, "candidates") +
                        "，纠错 " + metricValue(*row, "corrections") +
                        "，偏好 " + metricValue(*row, "preferences") +
                        "，分段链 " + metricValue(*row, "segment-chains"));
    }
    if (const auto *row = findRow(data, "model-output", "learned"); row && row->values.size() >= 8) {
        const auto path = metricValue(*row, "path");
        if (!path.empty()) {
            lines.push_back("写入学习文件：" + path);
        }
    }
    for (const auto *row : findRows(data, "model-output", "accepted-preference")) {
        if (row && row->values.size() >= 3) {
            lines.push_back("下次排序会优先：" + row->values[1] + " -> " + row->values[2]);
        }
    }
    for (const auto *row : findRows(data, "model-output", "accepted-segment-chain")) {
        if (row && row->values.size() >= 7) {
            const auto suffixCandidate = segmentSuffixCandidate(row->values[3], row->values[6]);
            lines.push_back("下次分段会借用：" + row->values[1] + " 先选 " + row->values[3] +
                            "，剩余 " + row->values[4] + " 会优先 " + suffixCandidate);
        } else if (row && row->values.size() >= 3) {
            lines.push_back("下次分段会借用：" + row->values[1] + " -> " + row->values[2]);
        }
    }

    if (const auto *row = findRow(data, "model-output", "learned-top-preference"); row && row->values.size() >= 4) {
        lines.push_back("模型学到选择：" + row->values[1] + " -> " + row->values[2] + "（次数 " +
                        row->values[3] + "）");
    }
    if (const auto *row = findRow(data, "model-output", "learned-top-correction"); row && row->values.size() >= 4) {
        lines.push_back("模型学到纠错：" + row->values[1] + " -> " + row->values[2] + "（次数 " +
                        row->values[3] + "）");
    }
    if (const auto *row = findRow(data, "model-output", "learned-top-correction-pattern");
        row && row->values.size() >= 5) {
        lines.push_back("模型学到习惯：" + correctionPatternKindText(row->values[2]) + " " + row->values[3] +
                        "（位置 " + row->values[4] + "，次数 " + row->values[1] + "）");
    }
    if (const auto *row = findRow(data, "model-output", "learned-top-segment-chain"); row) {
        if (row->values.size() >= 8) {
            const auto suffixCandidate = segmentSuffixCandidate(row->values[3], row->values[6]);
            lines.push_back("模型学到分段短语：" + row->values[1] + " 先选 " + row->values[3] +
                            "，剩余 " + row->values[4] + " 优先 " + suffixCandidate +
                            "（次数 " + row->values[7] + "）");
        } else if (row->values.size() >= 4) {
            lines.push_back("模型学到分段短语：" + row->values[1] + " -> " + row->values[2] +
                            "（次数 " + row->values[3] + "）");
        }
    }
    std::string learnedPreferenceTotal;
    std::string learnedCorrectionTotal;
    std::string learnedSegmentChainTotal;
    for (const auto *row : findRows(data, "model-output", "preferences-summary")) {
        if (!row || row->values.size() < 3) {
            continue;
        }
        if (row->values[0] == "preferences") {
            learnedPreferenceTotal = row->values[2];
        } else if (row->values[0] == "corrections") {
            learnedCorrectionTotal = row->values[2];
        } else if (row->values[0] == "segment-chains") {
            learnedSegmentChainTotal = row->values[2];
        }
    }
    if (!learnedPreferenceTotal.empty() || !learnedCorrectionTotal.empty() || !learnedSegmentChainTotal.empty()) {
        lines.push_back("学习文件：偏好 " + (learnedPreferenceTotal.empty() ? "0" : learnedPreferenceTotal) +
                        "，纠错 " + (learnedCorrectionTotal.empty() ? "0" : learnedCorrectionTotal) +
                        "，分段链 " + (learnedSegmentChainTotal.empty() ? "0" : learnedSegmentChainTotal));
    }
    if (const auto *row = findRow(data, "model-output", "note"); row && !row->values.empty()) {
        if (row->values[0] == "selected-candidate-learned" && row->values.size() >= 4) {
            lines.push_back("分析已学习本次选择：" + row->values[1] + " -> " + row->values[3]);
        } else if (row->values[0] == "selected-candidate-already-top" && row->values.size() >= 4) {
            lines.push_back("分析发现你的选择已经排第一：" + row->values[1] + " -> " +
                            row->values[3]);
        } else if (row->values[0] == "no-safe-learning-signal") {
            lines.push_back("分析没有发现可安全学习的选择或纠错");
        } else if (row->values[0] == "no-new-learning" && row->values.size() >= 3) {
            lines.push_back("分析输出已通过检查，但没有新增学习：" + row->values[1] +
                            "（" + valueText(row->values[2]) + "）");
        }
    }

    if (!findRow(data, "model-replay", "status")) {
        if (findRow(data, "model-replay", "mode")) {
            lines.push_back(analyzeLearns(data)
                                ? "模型：尚未运行；“分析当前输入”会运行一次，安全结果写入学习"
                                : "模型：尚未运行；“预览当前输入”只运行一次");
        } else {
            lines.push_back("模型：当前只读视图未运行模型");
        }
    } else if (const auto *row = findRow(data, "model-replay", "status"); row && !row->values.empty() &&
                                                               row->values[0] == "ok") {
        if (const auto *summary = findRow(data, "model-output", "summary"); summary && summary->values.size() >= 2 &&
                                                                  summary->values[0] == "rows" &&
                                                                  summary->values[1] == "0") {
            lines.push_back("模型：分析完成，没有建议改变排序或学习内容");
        } else {
            lines.push_back("模型：分析完成");
        }
    }
    return lines;
}

std::string displayTextFor(const LearningPanelRow &row) {
    if (row.section == "candidates" && row.key == "total" && row.values.size() >= 4) {
        return "总数 " + row.values[0] + " / " + metricText(row.values[1]) + " " + row.values[2] + " / " +
               metricText(row.values[3]) + " " + (row.values.size() >= 5 ? row.values[4] : "");
    }
    if (row.section == "state" && row.key == "request-source" && row.values.size() >= 2) {
        return "快照来源：" + valueText(row.values[0]) + " / " + row.values[1];
    }
    if (row.section == "state" && row.key == "request-mtime" && !row.values.empty()) {
        return "快照时间：" + formatUnixTime(row.values[0]);
    }
    if (row.section == "state" && row.key == "preedit" && !row.values.empty()) {
        return row.values[0].empty() ? "正在输入：无活动拼音" : "正在输入：" + row.values[0];
    }
    if (row.section == "state" && row.key == "status" && !row.values.empty()) {
        return "状态：" + valueText(row.values[0]);
    }
    if (row.section == "state" && row.key == "surrounding-before" && !row.values.empty()) {
        return "光标前文：" + (row.values[0].empty() ? "无" : row.values[0]);
    }
    if (row.section == "state" && row.key == "surrounding-after" && !row.values.empty()) {
        return "光标后文：" + (row.values[0].empty() ? "无" : row.values[0]);
    }
    if (row.section == "candidates" && row.key == "first" && row.values.size() >= 2) {
        return "首选 #" + row.values[0] + "：" + row.values[1];
    }
    if (row.section == "candidates" && row.key == "selected" && row.values.size() >= 2) {
        return "当前选中 #" + row.values[0] + "：" + row.values[1];
    }
    if (row.section == "candidates" && row.key == "visible" && row.values.size() >= 3) {
        return "显示位 #" + row.values[0] + " / 候选 #" + row.values[1] + "：" + row.values[2];
    }
    if (row.section == "candidates" && row.key == "numbered" && row.values.size() >= 4) {
        return "数字 " + row.values[1] + " / 候选 #" + row.values[2] + "：" + row.values[3];
    }
    if (row.section == "supervision" && row.key == "recent-events" && row.values.size() >= 6) {
        std::string result = "近期事件 " + row.values[0] + " / " + metricText(row.values[1]) + " " +
                             row.values[2] + " / " + metricText(row.values[3]) + " " + row.values[4] +
                             " / " + metricText(row.values[5]) + " " +
                             (row.values.size() >= 7 ? row.values[6] : "");
        if (row.values.size() >= 9) {
            result += " / " + metricText(row.values[7]) + " " + row.values[8];
        }
        return result;
    }
    if (row.section == "supervision" && row.key == "model-input" && row.values.size() >= 12) {
        std::string result = "模型看到：拼音 " + row.values[1] + " / 候选 " + row.values[3] + " / 显示 " +
                             row.values[5] + " / 带数字 " + row.values[7] + " / 上下文 " + row.values[9] +
                             " / 分段链 " + row.values[11];
        if (row.values.size() >= 14) {
            result += " / 待确认分段 " + row.values[13];
        }
        return result;
    }
    if (row.section == "supervision" && row.key == "mode" && !row.values.empty()) {
        return row.values[0] == "pass-through-only" ? "监督模式：仅按键习惯"
                                                     : "监督模式：" + valueText(row.values[0]);
    }
    if (row.section == "supervision" && row.key == "state" && row.values.size() >= 6) {
        return "监督状态：" + valueText(row.values[1]) + " / 近期事件 " + row.values[3] +
               " / 纠错轨迹 " + row.values[5];
    }
    if (row.section == "supervision" && row.key == "runtime-state" && row.values.size() >= 2 &&
        row.values[0] == "continuous") {
        return row.values[1] == "1" ? "连续监督：开启" : "连续监督：关闭";
    }
    if (row.section == "supervision" && row.key == "input-mode" && !row.values.empty()) {
        return row.values[0] == "english" ? "输入模式：英文直通监督" : "输入模式：中文候选";
    }
    if (row.section == "supervision" && row.key == "event-trail" && row.values.size() >= 6) {
        return "按键轨迹 " + row.values[1] + " 个事件 / 上限 " + row.values[3] + " / " + valueText(row.values[5]);
    }
    if (row.section == "supervision" && row.key == "correction-trail" && row.values.size() >= 6) {
        return "纠错轨迹 " + row.values[1] + " 个事件 / 上限 " + row.values[3] + " / " + valueText(row.values[5]);
    }
    if (row.section == "supervision" && row.key == "event-count" && row.values.size() >= 2) {
        return "监督统计：" + eventKindText(row.values[0]) + " " + row.values[1];
    }
    if (row.section == "supervision" && row.key == "correction-event-count" && row.values.size() >= 2) {
        return "纠错统计：" + eventKindText(row.values[0]) + " " + row.values[1];
    }
    if (row.section == "supervision" && row.key == "event-item" && row.values.size() >= 3) {
        return "近期按键 #" + row.values[0] + "：" + eventKindText(row.values[1]) +
               (row.values[2].empty() ? "" : " " + keyDisplayText(row.values[2]));
    }
    if (row.section == "supervision" && row.key == "correction-event-item" && row.values.size() >= 3) {
        return "纠错轨迹 #" + row.values[0] + "：" + eventKindText(row.values[1]) +
               (row.values[2].empty() ? "" : " " + keyDisplayText(row.values[2]));
    }
    if (row.section == "history" && row.key == "summary") {
        const auto records = metricValue(row, "records");
        const auto active = metricValue(row, "active");
        const auto passThrough = metricValue(row, "pass-through");
        const auto segmentChains = metricValue(row, "segment-chains");
        const auto pendingSegments = metricValue(row, "pending-segments");
        return "近期历史：记录 " + (records.empty() ? "0" : records) +
               " / 活动拼音 " + (active.empty() ? "0" : active) +
               " / 纯按键 " + (passThrough.empty() ? "0" : passThrough) +
               " / 分段链 " + (segmentChains.empty() ? "0" : segmentChains) +
               " / 待确认 " + (pendingSegments.empty() ? "0" : pendingSegments);
    }
    if (row.section == "history" && row.key == "preedit" && row.values.size() >= 3) {
        return "近期输入 #" + row.values[0] + "：" + row.values[1] + "（" + row.values[2] + " 次）";
    }
    if (row.section == "history" && row.key == "selected-candidate" && row.values.size() >= 3) {
        return "近期候选 #" + row.values[0] + "：" + row.values[1] + "（" + row.values[2] + " 次）";
    }
    if (row.section == "history" && row.key == "preedit-selected" && row.values.size() >= 4) {
        return "近期选择 #" + row.values[0] + "：" + row.values[1] + " -> " + row.values[2] +
               "（" + row.values[3] + " 次）";
    }
    if (row.section == "history" && row.key == "learnable-preference" && row.values.size() >= 8) {
        return "可由历史学习排序 #" + row.values[0] + "：" + row.values[1] + " -> " + row.values[2] +
               "（" + row.values[3] + " 次，建议 " +
               joinValues(std::vector<std::string>(row.values.begin() + 4, row.values.end())) + "）";
    }
    if (row.section == "history" && row.key == "learnable-correction" && row.values.size() >= 7) {
        return "可由历史学习纠错 #" + row.values[0] + "：" + row.values[1] + " -> " + row.values[2] +
               "（" + row.values[3] + " 次，建议 " +
               joinValues(std::vector<std::string>(row.values.begin() + 4, row.values.end())) + "）";
    }
    if (row.section == "history" && row.key == "correction" && row.values.size() >= 4) {
        return "近期纠错 #" + row.values[0] + "：" + row.values[1] + " -> " + row.values[2] +
               "（" + row.values[3] + " 次）";
    }
    if (row.section == "history" && row.key == "application" && row.values.size() >= 3) {
        return "近期程序 #" + row.values[0] + "：" + row.values[1] + "（" + row.values[2] + " 次）";
    }
    if (row.section == "history" && row.key == "event-count" && row.values.size() >= 3) {
        return "近期按键 #" + row.values[0] + "：" + eventKindText(row.values[1]) + " " + row.values[2];
    }
    if (row.section == "history" && row.key == "active-event-count" && row.values.size() >= 3) {
        return "近期组合按键 #" + row.values[0] + "：" + eventKindText(row.values[1]) + " " + row.values[2];
    }
    if (row.section == "history" && row.key == "pass-through-event-count" && row.values.size() >= 3) {
        return "近期透传按键 #" + row.values[0] + "：" + eventKindText(row.values[1]) + " " + row.values[2];
    }
    if (row.section == "history" && row.key == "correction-event-count" && row.values.size() >= 3) {
        return "近期纠错 #" + row.values[0] + "：" + eventKindText(row.values[1]) + " " + row.values[2];
    }
    if (row.section == "segment-chain" && row.values.size() >= 6) {
        return "#" + row.key + " " + row.values[0] + " -> " + row.values[2] + " / 剩余 " + row.values[3] +
               " / 合并 " + row.values[5];
    }
    if (row.section == "learning" && row.key == "preferences" && row.values.size() >= 7) {
        return "选择偏好 " + row.values[0] + "（总计 " + row.values[2] + "）/ 纠错 " + row.values[4] +
               "（总计 " + row.values[6] + "）";
    }
    if (row.section == "learning" && row.key == "selected-candidate-signal" && row.values.size() >= 4) {
        if (row.values[3] == "already-top") {
            return "选中候选已经排第一：" + row.values[0] + " -> " + row.values[2];
        }
        if (row.values[3] == "would-learn-preference") {
            return "可学习排序：" + row.values[0] + " -> " + row.values[2] + "（候选 #" + row.values[1] + "）";
        }
        if (row.values[3] == "prefix-only-no-preference") {
            return "前缀候选不学习整串排序：" + row.values[0] + " -> " + row.values[2];
        }
        return "选中候选信号：" + joinValues(row.values);
    }
    if (row.section == "learning" && row.key == "segment-chain-signal" && row.values.size() >= 8) {
        std::string result = "分段链证据 #" + row.values[0] + "：" + row.values[1] + " 先选 " + row.values[3] +
                             "（" + row.values[2] + "），保留 " + row.values[4] + "，合并 " +
                             row.values[6];
        if (row.values[7] == "suffix-continuation" || row.values[7] == "suffix-correction-chain") {
            result = "分段链证据 #" + row.values[0] + "：当前剩余 " + row.values[4] +
                     " 可接在 " + row.values[3] + " 后，合并 " + row.values[6];
        }
        if (row.values[7] == "correction-chain" || row.values[7] == "suffix-correction-chain") {
            result += "；纠错目标 " + row.values[5];
        }
        return result;
    }
    if (row.section == "learning" && row.key == "pending-segment-signal" && row.values.size() >= 6) {
        if (row.values[5] == "confirmed-suffix" && row.values.size() >= 9) {
            return "可学习分段 #" + row.values[0] + "：" + row.values[1] + " 已先选 " + row.values[3] +
                   "（" + row.values[2] + "），当前后缀 " + row.values[4] + " 选 " + row.values[6] +
                   "，合并 " + row.values[8];
        }
        return "待确认分段 #" + row.values[0] + "：" + row.values[1] + " 已先选 " + row.values[3] +
               "（" + row.values[2] + "），当前剩余 " + row.values[4];
    }
    if (row.section == "learning" && row.key == "correction-signal" && row.values.size() >= 4) {
        return "可学习纠错 #" + row.values[0] + "：" + row.values[2] + " -> " + row.values[3] +
               "（" + valueText(row.values[1]) + "）";
    }
    if (row.section == "learning" && row.key == "evidence-status" && row.values.size() >= 3) {
        return "当前拼音学习证据读取失败：" + row.values[1] + "（状态 " + row.values[2] + "）";
    }
    if (row.section == "learning" && row.key == "evidence-summary" && row.values.size() >= 9) {
        return "当前拼音证据：" + row.values[0] + " / 排序 " + row.values[2] + " / 旧偏好 " +
               row.values[4] + " / 纠错 " + row.values[6] + " / 分段链 " + row.values[8];
    }
    if (row.section == "learning" && row.key == "evidence-preference" && row.values.size() >= 4) {
        return "已学排序：" + row.values[1] + " -> " + row.values[2] + "（" + row.values[3] +
               " 次，行 " + row.values[0] + "）";
    }
    if (row.section == "learning" && row.key == "evidence-legacy-preference" && row.values.size() >= 3) {
        return "已学旧偏好：" + row.values[1] + "（" + row.values[2] + " 次，行 " + row.values[0] +
               "）";
    }
    if (row.section == "learning" && row.key == "evidence-supervised-raw-token" && row.values.size() >= 4) {
        return "英文模式已确认：" + row.values[1] + "（" + row.values[2] + " 次，" +
               valueText(row.values[3]) + "，行 " + row.values[0] + "）";
    }
    if (row.section == "learning" && row.key == "evidence-correction" && row.values.size() >= 5) {
        return "已学纠错：" + row.values[2] + " -> " + row.values[3] + "（" + row.values[4] +
               " 次，" + valueText(row.values[1]) + "，行 " + row.values[0] + "）";
    }
    if (row.section == "learning" && row.key == "evidence-segment-chain" && row.values.size() >= 8) {
        return "已学分段链：" + row.values[1] + " 先选 " + row.values[3] + "（" + row.values[2] +
               "），剩余 " + row.values[4] + "，合并 " + row.values[6] + "（" + row.values[7] +
               " 次，行 " + row.values[0] + "）";
    }
    if (row.section == "learning" && row.key == "evidence-effect") {
        return learningEffectText(row, true);
    }
    if (row.section == "learning" && row.key == "status" && row.values.size() >= 3) {
        return "学习状态：" + valueText(row.values[0]) + " / 下一步 " + valueText(row.values[2]);
    }
    if (row.section == "learning" && row.key == "status-suggested-protocol" && row.values.size() >= 2) {
        return "建议学习行 #" + row.values[0] + "：" +
               joinValues(std::vector<std::string>(row.values.begin() + 1, row.values.end()));
    }
    if (row.section == "learning" && row.key == "status-awaiting-suffix" && row.values.size() >= 5) {
        return "等待后缀 #" + row.values[0] + "：" + row.values[1] + " 先选 " + row.values[3] +
               "，剩余 " + row.values[4];
    }
    if (row.section == "learning" && row.key == "status-signal-count" && row.values.size() >= 2) {
        return "学习信号：" + valueText(row.values[0]) + " " + row.values[1];
    }
    if ((row.section == "top-preference" || row.section == "top-correction") && row.values.size() >= 3) {
        const auto arrow = row.section == "top-correction" ? " => " : " -> ";
        return "#" + row.key + " 次数 " + row.values[0] + "：" + row.values[1] + arrow + row.values[2];
    }
    if (row.section == "top-correction-pattern" && row.values.size() >= 4) {
        return "#" + row.key + " 次数 " + row.values[0] + "：" + correctionPatternKindText(row.values[1]) + " " +
               row.values[2] + " / 位置 " + row.values[3];
    }
    if (row.section == "behavior" && row.key == "raw-english-hint" && row.values.size() >= 3) {
        std::string result = "英文直出提示 " + row.values[0] + "（" + valueText(row.values[2]) + "）";
        if (row.values.size() >= 5 && row.values[3] == "count") {
            result += " / 次数 " + row.values[4];
        }
        return result;
    }
    if (row.section == "behavior" && row.key == "preedit-leading-context") {
        const auto active = metricValue(row, "active");
        const auto events = metricValue(row, "events");
        if (active == "1") {
            return "拼音前按键：" + (events.empty() ? "0" : events) + " 个";
        }
        return "拼音前按键：无";
    }
    if (row.section == "behavior" && row.key == "preedit-leading-event" && row.values.size() >= 3) {
        std::string result = "拼音前按键 #" + row.values[0] + "：" + eventKindText(row.values[1]);
        if (!row.values[2].empty()) {
            result += " " + keyDisplayText(row.values[2]);
        }
        return result;
    }
    if (row.section == "behavior" && row.key == "edit-summary" && row.values.size() >= 12) {
        std::string result = "编辑摘要：当前 " + (row.values[1].empty() ? "空" : row.values[1]) +
                             " / 光标 " + row.values[3];
        if (!row.values[5].empty()) {
            result += " / 最近输入 " + row.values[5];
        }
        if (!row.values[7].empty()) {
            result += " / 刚删空 " + row.values[7];
        }
        if (!row.values[9].empty()) {
            result += " / 刚重写 " + row.values[9];
        }
        if (!row.values[11].empty()) {
            result += " / 中间编辑 " + row.values[11];
        }
        return result;
    }
    if (row.section == "behavior" && (row.key == "recent-event" || row.key == "correction-event") &&
        row.values.size() >= 2) {
        return (row.key == "recent-event" ? "近期事件 " : "纠错事件 ") + eventKindText(row.values[0]) + "：" +
               keyDisplayText(row.values[1]);
    }
    if (row.section == "behavior" && row.key == "possible-correction" && row.values.size() >= 4) {
        return "#" + row.values[0] + " " + valueText(row.values[1]) + "：" + row.values[2] + " -> " +
               row.values[3];
    }
    if (row.section == "behavior" && row.key == "correction-pattern" && row.values.size() >= 5) {
        return "纠错模式 #" + row.values[0] + "：" + correctionPatternKindText(row.values[1]) + " " +
               row.values[2] + " / 位置 " + row.values[3] + " / 次数 " + row.values[4];
    }
    if (row.section == "behavior" && row.key == "realtime-correction" && row.values.size() >= 8) {
        std::string result = "实时纠错 #" + row.values[0] + "：" + valueText(row.values[1]) + " / " +
                             correctionPatternKindText(row.values[3]) + " " + row.values[4] +
                             " / 位置 " + row.values[5] + " / 次数 " + row.values[6] +
                             " / 原因 " + valueText(row.values[2]);
        if (!row.values[7].empty()) {
            result += " / 修正 " + row.values[7];
        }
        return result;
    }
    if (row.section == "model-replay" && row.key == "status" && !row.values.empty()) {
        if (row.values[0] == "ok") {
            return "回放成功";
        }
        return "回放错误 " + joinValues(std::vector<std::string>(row.values.begin() + 1, row.values.end()));
    }
    if (row.section == "model-replay" && row.key == "wrapper" && row.values.size() >= 3) {
        return "模型命令 " + row.values[0] + " / " + metricText(row.values[1]) + " " + row.values[2];
    }
    if (row.section == "model-replay" && row.key == "check" && !row.values.empty()) {
        return "检查 " + joinValues(row.values);
    }
    if (row.section == "model-replay" && row.key == "mode" && row.values.size() >= 5) {
        return "模式 " + valueText(row.values[0]) + " / " + metricText(row.values[1]) + " " +
               valueText(row.values[2]) + " / " + metricText(row.values[3]) + " " + valueText(row.values[4]);
    }
    if (row.section == "model-config" && !row.values.empty()) {
        if (row.key == "kind") {
            return modelConfigKeyText(row.key) + "：" + modelKindText(row.values[0]);
        }
        if (row.key == "personal-model-training-samples" ||
            row.key == "personal-model-training-chinese-ranking-samples" ||
            row.key == "personal-model-training-non-leading-samples" ||
            row.key == "personal-model-pair-evidence" ||
            row.key == "personal-model-active-pair-evidence" ||
            row.key == "personal-model-raw-token-evidence" ||
            row.key == "personal-model-active-raw-token-evidence" ||
            row.key == "personal-model-training-raw-token-evidence-entries" ||
            row.key == "personal-model-training-active-raw-token-evidence" ||
            row.key == "personal-model-active-correction-patterns" ||
            row.key == "personal-model-active-key-habits" ||
            row.key == "personal-model-training-validation-accuracy" ||
            row.key == "personal-model-training-validation-baseline-accuracy" ||
            row.key == "personal-model-training-validation-gain" ||
            row.key == "personal-model-training-validation-non-leading-samples" ||
            row.key == "personal-model-training-validation-non-leading-correct" ||
            row.key == "personal-model-training-validation-non-leading-accuracy" ||
            row.key == "personal-model-training-validation-leading-samples" ||
            row.key == "personal-model-training-validation-leading-correct" ||
            row.key == "personal-model-training-validation-generic-non-leading-samples" ||
            row.key == "personal-model-training-validation-generic-non-leading-correct" ||
            row.key == "personal-model-training-validation-generic-non-leading-accuracy" ||
            row.key == "personal-model-training-validation-generic-excluded-direct-evidence" ||
            row.key == "personal-model-training-validation-generic-excluded-seen-preedit" ||
            row.key == "personal-model-training-validation-generic-excluded-raw-candidate" ||
            row.key == "personal-model-training-validation-generic-excluded-derived-prefix" ||
            row.key == "personal-model-training-raw-profile-samples" ||
            row.key == "personal-model-training-raw-profile-accepted-samples" ||
            row.key == "personal-model-training-raw-profile-rejected-samples" ||
            row.key == "personal-model-training-raw-profile-validation-samples" ||
            row.key == "personal-model-training-raw-profile-validation-correct" ||
            row.key == "personal-model-training-raw-profile-validation-false-promotions" ||
            row.key == "personal-model-training-raw-profile-validation-accuracy" ||
            row.key == "personal-model-promotion-margin") {
            return modelConfigKeyText(row.key) + "：" + joinValues(row.values);
        }
        if (row.key == "config-active" || row.key == "process-command-active" ||
            row.key == "configured-command-valid" || row.key == "dry-run-test-supported" ||
            row.key == "training-context" || row.key == "training-surrounding" ||
            row.key == "send-recent-input" || row.key == "send-surrounding" ||
            row.key == "personal-model-keyboard-correction-safe" ||
            row.key == "personal-model-raw-profile-safe" ||
            row.key == "personal-model-generic-ranking-safe" ||
            row.key == "personal-model-component-update-safe" || row.key == "llama-command-valid" ||
            row.key == "llama-model-readable") {
            return modelConfigKeyText(row.key) + "：" + (row.values[0] == "1" ? "是" : "否");
        }
        if (row.key == "activation-hint") {
            return modelConfigKeyText(row.key) + "：如需让 fcitx5 加载模型入口，运行 " + row.values[0];
        }
        if (row.key == "process-command-scope" || row.key == "process-command-active-scope" ||
            row.key == "runtime-verification" || row.key == "process-command") {
            return modelConfigKeyText(row.key) + "：" + valueText(joinValues(row.values));
        }
        return modelConfigKeyText(row.key) + "：" + valueText(joinValues(row.values));
    }
    if (row.section == "model-output" && row.key == "accepted-candidate" && row.values.size() >= 2) {
        return "已接受候选 #" + row.values[0] + "：" + row.values[1];
    }
    if (row.section == "model-output" && row.key == "accepted-correction" && row.values.size() >= 3) {
        return "已接受纠错 #" + row.values[0] + "：" + row.values[1] + " => " + row.values[2];
    }
    if (row.section == "model-output" && row.key == "accepted-preference" && row.values.size() >= 3) {
        return "已接受偏好 #" + row.values[0] + "：" + row.values[1] + " -> " + row.values[2];
    }
    if (row.section == "model-output" && row.key == "accepted-segment-chain" && row.values.size() >= 7) {
        const auto suffixCandidate = segmentSuffixCandidate(row.values[3], row.values[6]);
        std::string result = "已接受分段链 #" + row.values[0] + "：" + row.values[1] + " 先选 " +
                             row.values[3] + "，剩余 " + row.values[4] + " => " + suffixCandidate;
        if (row.values[5] != row.values[1]) {
            result += "，纠错到 " + row.values[5];
        }
        return result;
    }
    if (row.section == "model-output" && row.key == "accepted-segment-chain" && row.values.size() >= 3) {
        return "已接受分段链 #" + row.values[0] + "：" + row.values[1] + " => " + row.values[2];
    }
    if (row.section == "model-output" && row.key == "accepted" && row.values.size() >= 4) {
        if (row.values[0] == "preference") {
            return "已接受偏好 #" + row.values[1] + "：" + row.values[2] + " -> " + row.values[3];
        }
        if (row.values[0] == "segment_chain") {
            return "已接受分段链 #" + row.values[1] + "：" + row.values[2] + " => " + row.values[3];
        }
    }
    if (row.section == "model-output" && row.key == "summary" && row.values.size() >= 6) {
        if (row.values.size() >= 10) {
            return "汇总 " + metricText(row.values[0]) + " " + row.values[1] + " / " + metricText(row.values[2]) +
                   " " + row.values[3] + " / " + metricText(row.values[4]) + " " + row.values[5] + " / " +
                   metricText(row.values[6]) + " " + row.values[7] + " / " + metricText(row.values[8]) + " " +
                   row.values[9];
        }
        return "汇总 " + metricText(row.values[0]) + " " + row.values[1] + " / " + metricText(row.values[2]) +
               " " + row.values[3] + " / " + metricText(row.values[4]) + " " + row.values[5];
    }
    if (row.section == "model-output" && row.key == "learned" && row.values.size() >= 6) {
        if (row.values.size() >= 8) {
            return "已学习 " + metricText(row.values[0]) + " " + row.values[1] + " / " +
                   metricText(row.values[2]) + " " + row.values[3] + " / " + metricText(row.values[4]) + " " +
                   row.values[5] + " / " + metricText(row.values[6]) + " " + row.values[7];
        }
        return "已学习 " + metricText(row.values[0]) + " " + row.values[1] + " / " +
               metricText(row.values[2]) + " " + row.values[3] + " / " + metricText(row.values[4]) + " " +
               row.values[5];
    }
    if (row.section == "model-output" && row.key == "learned-top-preference" && row.values.size() >= 4) {
        return "已学习偏好 #" + row.values[0] + "：" + row.values[1] + " -> " + row.values[2] +
               "（次数 " + row.values[3] + "）";
    }
    if (row.section == "model-output" && row.key == "learned-top-correction" && row.values.size() >= 4) {
        return "已学习纠错 #" + row.values[0] + "：" + row.values[1] + " => " + row.values[2] +
               "（次数 " + row.values[3] + "）";
    }
    if (row.section == "model-output" && row.key == "learned-top-correction-pattern" &&
        row.values.size() >= 5) {
        return "已学习习惯 #" + row.values[0] + "：次数 " + row.values[1] + " / " +
               correctionPatternKindText(row.values[2]) + " " + row.values[3] + " / 位置 " + row.values[4];
    }
    if (row.section == "model-output" && row.key == "learned-top-segment-chain" && row.values.size() >= 8) {
        const auto suffixCandidate = segmentSuffixCandidate(row.values[3], row.values[6]);
        std::string result = "已学习分段链 #" + row.values[0] + "：" + row.values[1] + " 先选 " +
                             row.values[3] + "，剩余 " + row.values[4] + " => " + suffixCandidate +
                             "（次数 " + row.values[7] + "）";
        if (row.values[5] != row.values[1]) {
            result += "，纠错到 " + row.values[5];
        }
        return result;
    }
    if (row.section == "model-output" && row.key == "learned-top-segment-chain" && row.values.size() >= 4) {
        return "已学习分段链 #" + row.values[0] + "：" + row.values[1] + " => " + row.values[2] +
               "（次数 " + row.values[3] + "）";
    }
    if (row.section == "model-output" && row.key == "preferences-summary" && row.values.size() >= 2) {
        if (row.values.size() >= 3) {
            return "学习文件汇总：" + metricText(row.values[0]) + " " + row.values[1] +
                   "（总计 " + row.values[2] + "）";
        }
        return "学习文件汇总：" + metricText(row.values[0]) + " " + row.values[1];
    }
    if (row.section == "model-output" && row.key == "preferences-top-preference" && row.values.size() >= 4) {
        return "学习文件偏好 #" + row.values[0] + "：次数 " + row.values[1] + " / " +
               row.values[2] + " -> " + row.values[3];
    }
    if (row.section == "model-output" && row.key == "preferences-top-legacy-preference" &&
        row.values.size() >= 3) {
        return "学习文件旧偏好 #" + row.values[0] + "：次数 " + row.values[1] + " / " + row.values[2];
    }
    if (row.section == "model-output" && row.key == "preferences-top-correction" && row.values.size() >= 4) {
        return "学习文件纠错 #" + row.values[0] + "：次数 " + row.values[1] + " / " +
               row.values[2] + " => " + row.values[3];
    }
    if (row.section == "model-output" && row.key == "preferences-top-correction-pattern" &&
        row.values.size() >= 5) {
        return "学习文件习惯 #" + row.values[0] + "：次数 " + row.values[1] + " / " +
               correctionPatternKindText(row.values[2]) + " " + row.values[3] + " / 位置 " + row.values[4];
    }
    if (row.section == "model-output" && row.key == "preferences-top-segment-chain" && row.values.size() >= 4) {
        return "学习文件分段链 #" + row.values[0] + "：次数 " + row.values[1] + " / " +
               row.values[2] + " => " + row.values[3];
    }
    if (row.section == "model-output" && row.key == "note" && !row.values.empty()) {
        if (row.values[0] == "selected-candidate-learned" && row.values.size() >= 4) {
            return "说明：已学习选中候选 " + row.values[1] + " -> " + row.values[3];
        }
        if (row.values[0] == "selected-candidate-already-top" && row.values.size() >= 4) {
            return "说明：选中候选已经排第一 " + row.values[1] + " -> " + row.values[3];
        }
        if (row.values[0] == "no-safe-learning-signal") {
            return "说明：没有安全学习信号";
        }
        if (row.values[0] == "no-new-learning" && row.values.size() >= 3) {
            return "说明：没有新增学习 " + row.values[1] + "（" + valueText(row.values[2]) + "）";
        }
        return "说明 " + joinValues(row.values);
    }
    if (row.section == "model-output" && row.key == "rejected-summary" && row.values.size() >= 4) {
        return "模型输出被拒绝：行 " + row.values[1] + " / 检查状态 " + row.values[3];
    }
    if (row.section == "model-output" && row.key == "rejected-row" && row.values.size() >= 2) {
        if (row.values[1] == "candidate" && row.values.size() >= 3) {
            return "被拒绝输出 #" + row.values[0] + " 候选 " + row.values[2];
        }
        if (row.values[1] == "correction" && row.values.size() >= 4) {
            return "被拒绝输出 #" + row.values[0] + " 纠错 " + row.values[2] + " => " + row.values[3];
        }
        if (row.values[1] == "preference" && row.values.size() >= 4) {
            return "被拒绝输出 #" + row.values[0] + " 偏好 " + row.values[2] + " -> " + row.values[3];
        }
        if (row.values[1] == "segment_chain" && row.values.size() >= 5) {
            return "被拒绝输出 #" + row.values[0] + " 分段链 " + row.values[2] + " => " + row.values[4];
        }
        return "被拒绝输出 #" + row.values[0] + " " + joinValues(row.values);
    }
    if (row.section == "model-output" && row.key == "row-candidate" && row.values.size() >= 2) {
        return "模型输出 #" + row.values[0] + " 候选 " + row.values[1];
    }
    if (row.section == "model-output" && row.key == "row-correction" && row.values.size() >= 3) {
        return "模型输出 #" + row.values[0] + " 纠错 " + row.values[1] + " => " + row.values[2];
    }
    if (row.section == "model-output" && row.key == "row" && row.values.size() >= 2) {
        return "模型输出 #" + row.values[0] + " " + row.values[1];
    }
    if (row.values.empty()) {
        return row.key;
    }
    return row.key + "\t" + joinValues(row.values);
}

bool printParsedPanel(LearningPanelData &data) {
    if (!loadPanelData(data)) {
        std::cerr << "无法读取面板数据\n";
        return false;
    }
    for (const auto &row : data.rows) {
        std::cout << row.section << '\t' << row.key;
        if (!row.values.empty()) {
            std::cout << '\t' << joinValues(row.values);
        }
        std::cout << '\n';
    }
    return true;
}

LearningPanelData parseArgs(int argc, char **argv) {
    LearningPanelData data;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--self-test") {
            data.selfTest = true;
        } else if (arg == "--parse-panel") {
            data.parseOnly = true;
            if (index + 1 < argc && std::string_view(argv[index + 1]).rfind("--", 0) != 0) {
                data.inputPath = argv[++index];
            }
        } else if (arg == "--refresh-command") {
            if (index + 1 >= argc) {
                std::cerr << "--refresh-command 需要一个值\n";
                data.argumentError = true;
            } else {
                data.refreshCommand = argv[++index];
            }
        } else if (arg == "--model-config-command") {
            if (index + 1 >= argc) {
                std::cerr << "--model-config-command 需要一个值\n";
                data.argumentError = true;
            } else {
                data.modelConfigCommand = argv[++index];
            }
        } else if (arg == "--personal-train-command") {
            if (index + 1 >= argc) {
                std::cerr << "--personal-train-command 需要一个值\n";
                data.argumentError = true;
            } else {
                data.personalTrainCommand = argv[++index];
            }
        } else if (arg == "--title") {
            if (index + 1 >= argc) {
                std::cerr << "--title 需要一个值\n";
                data.argumentError = true;
            } else {
                data.windowTitle = argv[++index];
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "用法：tipe-learning-panel-window [--self-test] [--parse-panel [PATH|-]] "
                         "[--refresh-command PATH] [--model-config-command PATH] "
                         "[--personal-train-command PATH] [--title TEXT] [PATH|-]\n";
            data.argumentError = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "未知参数：" << arg << '\n';
            data.argumentError = true;
        } else if (data.inputPath.empty()) {
            data.inputPath = argv[index];
        } else {
            std::cerr << "输入路径过多\n";
            data.argumentError = true;
        }
    }
    return data;
}

std::vector<char *> gtkArgs(char **argv) {
    return {argv[0]};
}

GtkWidget *makeLabel(const std::string &text, const char *cssClass) {
    auto *label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, cssClass);
    return label;
}

void clearBox(GtkWidget *box) {
    while (auto *child = gtk_widget_get_first_child(box)) {
        gtk_box_remove(GTK_BOX(box), child);
    }
}

GtkWidget *makeSection(const std::string &title) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box, "section");
    gtk_box_append(GTK_BOX(box), makeLabel(title, "section-title"));
    return box;
}

std::string pairedRowValue(const LearningPanelData &data, std::string_view section, std::string_view key,
                           std::string_view field, std::string_view fallback = "0") {
    const auto *row = findRow(data, section, key);
    if (!row) {
        return std::string(fallback);
    }
    for (std::size_t index = 0; index + 1 < row->values.size(); index += 2) {
        if (row->values[index] == field) {
            return row->values[index + 1];
        }
    }
    return std::string(fallback);
}

std::size_t numericValue(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const auto result = std::stoull(std::string(value), &consumed);
        return consumed == value.size() ? static_cast<std::size_t>(result) : 0;
    } catch (...) {
        return 0;
    }
}

struct LearningOverview {
    std::size_t records = 0;
    std::size_t learnable = 0;
    std::size_t choices = 0;
    std::size_t observations = 0;
    std::size_t selectionRules = 0;
    std::size_t englishTokens = 0;
    std::size_t corrections = 0;
    bool modelReady = false;
    std::string dictionaryHistoryStatus = "unknown";
    std::size_t dictionaryHistoryBytes = 0;
    std::string dictionaryHistoryPath;
};

LearningOverview learningOverviewFrom(const LearningPanelData &data) {
    LearningOverview overview;
    overview.records = numericValue(pairedRowValue(
        data, "training", "summary", "records", rowValue(data, "model-config", "personal-model-training-samples", "0")));
    overview.learnable = numericValue(pairedRowValue(data, "training", "summary", "learnable"));
    overview.choices = numericValue(pairedRowValue(data, "training", "summary", "choices"));
    overview.observations = numericValue(pairedRowValue(data, "training", "summary", "observations"));
    overview.selectionRules =
        numericValue(rowValue(data, "model-config", "personal-model-active-pair-evidence"));
    overview.englishTokens =
        numericValue(rowValue(data, "model-config", "personal-model-active-raw-token-evidence"));
    overview.corrections =
        numericValue(rowValue(data, "model-config", "personal-model-active-correction-patterns")) +
        numericValue(rowValue(data, "model-config", "personal-model-active-key-habits"));
    overview.modelReady = rowValue(data, "model-config", "personal-model-status") == "ready";
    overview.dictionaryHistoryStatus = pairedRowValue(data, "learning", "dictionary-history", "status", "unknown");
    overview.dictionaryHistoryBytes =
        numericValue(pairedRowValue(data, "learning", "dictionary-history", "bytes"));
    overview.dictionaryHistoryPath = pairedRowValue(data, "learning", "dictionary-history", "path", "");
    return overview;
}

std::string dictionaryHistoryStatusText(const LearningOverview &overview) {
    if (overview.dictionaryHistoryStatus == "ready") {
        return "正在工作";
    }
    if (overview.dictionaryHistoryStatus == "waiting") {
        return "等待第一次中文选词";
    }
    if (overview.dictionaryHistoryStatus == "disabled") {
        return "已关闭";
    }
    if (overview.dictionaryHistoryStatus == "error") {
        return "历史文件不可用";
    }
    return "状态未知";
}

std::string learningHeadline(const LearningOverview &overview) {
    if (overview.records == 0) {
        return "TiPE 还在等待你的输入";
    }
    if (overview.selectionRules + overview.englishTokens + overview.corrections == 0) {
        return "TiPE 已开始记录你的输入习惯";
    }
    return "TiPE 正在使用学到的输入习惯";
}

std::string learningSummaryText(const LearningOverview &overview) {
    if (overview.records == 0) {
        return "正常使用输入法即可。记录只保存在这台电脑上。";
    }
    return "已经记录 " + std::to_string(overview.records) + " 次完整输入，其中 " +
           std::to_string(overview.learnable) + " 条可以用来更新 TiP。";
}

std::string modelModeDescription(std::string_view mode) {
    if (mode == "personal") {
        return "在这台电脑上学习你的选词、英文词和按键习惯，不联网。";
    }
    if (mode == "ollama" || mode == "llama-cpp") {
        return "大模型在这台电脑上运行，输入内容不会发送到云端。";
    }
    if (mode == "openai" || mode == "openai-compatible") {
        return "只在你主动分析时联网一次，可能产生服务商费用。";
    }
    if (mode == "heuristic") {
        return "只使用 TiPE 内置规则，不运行大模型。";
    }
    if (mode == "off") {
        return "智能分析已关闭，输入法基本功能仍可使用。";
    }
    if (mode == "custom") {
        return "使用你提供的模型脚本。";
    }
    if (mode == "dump") {
        return "只保存一次分析请求，供开发和排查使用。";
    }
    return "";
}

std::string modelSetupHint(std::string_view mode) {
    if (mode == "personal") {
        return "不需要填写任何账号。回到“学习”页点击“更新 TiP”即可。";
    }
    if (mode == "ollama") {
        return "需要先在电脑上安装并启动 Ollama，再填写已经下载的模型名称。";
    }
    if (mode == "llama-cpp") {
        return "需要选择电脑上的 GGUF 模型文件；每次分析时临时运行一次。";
    }
    if (mode == "openai") {
        return "需要 OpenAI API 平台的 API Key 和模型名称。ChatGPT 订阅不能代替 API Key。";
    }
    if (mode == "openai-compatible") {
        return "从服务商后台复制 API 地址、模型名称和 API Key。服务必须兼容 Chat Completions。";
    }
    return modelModeDescription(mode);
}

std::string cloudDataSummary(const ModelSettings &settings) {
    std::string result = "当前拼音和候选";
    if (settings.cloudRecentInput) {
        result += "、近期按键和修改记录";
    }
    if (settings.cloudSurrounding) {
        result += "、光标附近文字和应用名称";
    }
    return result;
}

GtkWidget *makeMetric(const std::string &value, const std::string &label, const std::string &detail) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "metric");
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_size_request(box, 150, 86);
    gtk_box_append(GTK_BOX(box), makeLabel(value, "metric-value"));
    gtk_box_append(GTK_BOX(box), makeLabel(label, "metric-label"));
    gtk_box_append(GTK_BOX(box), makeLabel(detail, "metric-detail"));
    return box;
}

GtkWidget *makeModelChoice(const std::string &title, const std::string &detail, bool current) {
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "model-choice");
    if (current) {
        gtk_widget_add_css_class(row, "model-choice-current");
    }
    auto *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(text), makeLabel(title, "model-choice-title"));
    gtk_box_append(GTK_BOX(text), makeLabel(detail, "model-choice-text"));
    gtk_box_append(GTK_BOX(row), text);
    if (current) {
        gtk_box_append(GTK_BOX(row), makeLabel("正在使用", "current-badge"));
    }
    return row;
}

GtkWidget *makeInfoRow(const std::string &label, const std::string &value) {
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "info-row");
    auto *key = makeLabel(label, "info-key");
    gtk_widget_set_size_request(key, 112, -1);
    auto *text = makeLabel(value, "info-value");
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(row), key);
    gtk_box_append(GTK_BOX(row), text);
    return row;
}

std::string modelModeTitle(std::string_view mode) {
    if (mode == "personal") {
        return "TiP 本地模型";
    }
    if (mode == "heuristic") {
        return "TiPE 基础规则";
    }
    if (mode == "llama-cpp") {
        return "GGUF 本地大模型";
    }
    if (mode == "ollama") {
        return "Ollama 本地大模型";
    }
    if (mode == "openai") {
        return "OpenAI 云端模型";
    }
    if (mode == "openai-compatible") {
        return "其他兼容云端服务";
    }
    if (mode == "custom") {
        return "自定义模型脚本";
    }
    if (mode == "off") {
        return "已关闭";
    }
    return std::string(mode);
}

std::filesystem::path supportImagePath(std::string_view name) {
    std::vector<std::filesystem::path> roots;
    if (const char *supportDir = std::getenv("TIPE_SUPPORT_DIR"); supportDir && *supportDir) {
        roots.emplace_back(supportDir);
    }
    if (const char *dataHome = std::getenv("XDG_DATA_HOME"); dataHome && *dataHome) {
        roots.emplace_back(std::filesystem::path(dataHome) / "tipe" / "support");
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        roots.emplace_back(std::filesystem::path(home) / ".local" / "share" / "tipe" / "support");
    }
#ifdef TIPE_SUPPORT_DATA_DIR
    roots.emplace_back(TIPE_SUPPORT_DATA_DIR);
#endif
    for (const auto &root : roots) {
        const auto candidate = root / (std::string(name) + ".png");
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

GtkWidget *makeSupportSlot(const std::string &title, const std::filesystem::path &path) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "support-slot");
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_size_request(box, 248, -1);
    gtk_box_append(GTK_BOX(box), makeLabel(title, "support-title"));
    if (!path.empty() && std::filesystem::is_regular_file(path)) {
        auto *picture = gtk_picture_new_for_filename(path.c_str());
        gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
        gtk_widget_set_size_request(picture, 220, 220);
        gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), picture);
    } else {
        auto *placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(placeholder, "support-placeholder");
        gtk_widget_set_size_request(placeholder, 220, 220);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        auto *icon = gtk_image_new_from_icon_name("image-missing-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
        gtk_box_append(GTK_BOX(placeholder), icon);
        gtk_box_append(GTK_BOX(placeholder), makeLabel("收款码待添加", "support-empty"));
        gtk_box_append(GTK_BOX(box), placeholder);
    }
    return box;
}

void installCss() {
    auto *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, R"CSS(
        window {
            background: #151518;
        }
        .root {
            background: #151518;
            padding: 16px;
        }
        .title {
            color: #f4f4f5;
            font: 700 18px "Sans";
            margin-bottom: 8px;
        }
        .brand-header {
            margin-bottom: 10px;
        }
        .brand-title {
            color: #f7f7f8;
            font: 800 20px "Sans";
        }
        .brand-subtitle {
            color: #8f98a5;
            font: 500 12px "Sans";
        }
        .tipe-toolbar {
            margin-bottom: 6px;
        }
        .toolbar-status {
            margin-bottom: 10px;
        }
        notebook > header {
            background: #151518;
            border-bottom: 1px solid rgba(255, 255, 255, 0.10);
        }
        notebook > header tab {
            color: #aeb6c2;
            background: transparent;
            padding: 8px 12px;
        }
        notebook > header tab:checked {
            color: #f4f4f5;
            background: rgba(25, 183, 165, 0.14);
            border-bottom: 2px solid #19b7a5;
        }
        .page {
            background: #151518;
            padding: 14px 2px 2px 2px;
        }
        .status {
            color: #aeb6c2;
            font: 500 12px "Sans";
        }
        .settings-root {
            background: #151518;
            padding: 16px;
        }
        .settings-grid {
            margin-top: 10px;
            margin-bottom: 12px;
        }
        .advanced-settings {
            color: #d7d9df;
            margin-bottom: 10px;
        }
        .field-label {
            color: #c9cbd1;
            font: 600 13px "Sans";
        }
        entry, dropdown {
            color: #f4f4f5;
            background: #242529;
            border: 1px solid rgba(255, 255, 255, 0.14);
            border-radius: 5px;
            min-height: 34px;
        }
        checkbutton {
            color: #d7d9df;
        }
        checkbutton:disabled {
            color: #777b84;
        }
        button {
            color: #f4f4f5;
            background: #2b2d31;
            border: 1px solid rgba(255, 255, 255, 0.14);
            border-radius: 6px;
            padding: 6px 10px;
        }
        .section {
            background: transparent;
            border-bottom: 1px solid rgba(255, 255, 255, 0.10);
            border-radius: 0;
            padding: 8px 2px 12px 2px;
            margin-bottom: 10px;
        }
        .section-title {
            color: #f0f1f3;
            font: 700 14px "Sans";
            margin-bottom: 6px;
        }
        .row {
            color: #f4f4f5;
            font: 500 13px "Sans";
        }
        .metric-strip {
            margin-bottom: 14px;
        }
        .metric {
            background: #202226;
            border: 1px solid rgba(255, 255, 255, 0.10);
            border-radius: 7px;
            padding: 12px;
        }
        .metric-value {
            color: #f7f7f8;
            font: 800 26px "Sans";
        }
        .metric-label {
            color: #d7d9df;
            font: 700 13px "Sans";
        }
        .metric-detail {
            color: #8f98a5;
            font: 500 11px "Sans";
        }
        .overview-callout {
            background: #202226;
            border-left: 3px solid #19b7a5;
            padding: 14px 16px;
            margin-bottom: 14px;
        }
        .overview-title {
            color: #f4f4f5;
            font: 750 17px "Sans";
        }
        .overview-text {
            color: #b9c1ca;
            font: 500 13px "Sans";
        }
        .info-row {
            padding: 5px 0;
        }
        .info-key {
            color: #8f98a5;
            font: 600 12px "Sans";
        }
        .info-value {
            color: #e8e9ec;
            font: 500 13px "Sans";
        }
        .page-actions {
            margin-top: 8px;
            margin-bottom: 4px;
        }
        .model-callout {
            background: rgba(25, 183, 165, 0.09);
            border-left: 3px solid #19b7a5;
            padding: 10px 12px;
            margin-bottom: 10px;
        }
        .model-callout-title {
            color: #f4f4f5;
            font: 700 14px "Sans";
        }
        .model-callout-text {
            color: #b9c1ca;
            font: 500 12px "Sans";
        }
        .model-choice {
            background: #202226;
            border: 1px solid rgba(255, 255, 255, 0.10);
            border-radius: 7px;
            padding: 11px 12px;
            margin-bottom: 6px;
        }
        .model-choice-current {
            border-color: rgba(25, 183, 165, 0.65);
            background: rgba(25, 183, 165, 0.08);
        }
        .model-choice-title {
            color: #f0f1f3;
            font: 700 13px "Sans";
        }
        .model-choice-text {
            color: #9da6b1;
            font: 500 12px "Sans";
        }
        .current-badge {
            color: #63d8c8;
            font: 700 11px "Sans";
        }
        .privacy-box {
            background: rgba(25, 183, 165, 0.07);
            border-left: 3px solid #19b7a5;
            padding: 10px 12px;
            margin: 4px 0 8px 0;
        }
        .settings-hint {
            color: #aeb6c2;
            font: 500 12px "Sans";
            margin-bottom: 6px;
        }
        .advanced-details {
            color: #c9cbd1;
            margin-top: 6px;
        }
        .support-row {
            margin-top: 8px;
        }
        .support-slot {
            background: #202226;
            border: 1px solid rgba(255, 255, 255, 0.10);
            border-radius: 7px;
            padding: 14px;
        }
        .support-title {
            color: #f4f4f5;
            font: 700 15px "Sans";
        }
        .support-placeholder {
            color: #737b87;
            border: 1px dashed rgba(255, 255, 255, 0.20);
            border-radius: 6px;
            padding: 12px;
        }
        .support-empty {
            color: #8f98a5;
            font: 500 12px "Sans";
        }
    )CSS");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void renderRowsInto(GtkWidget *container, LearningPanelData &data) {
    const auto overview = learningOverviewFrom(data);
    const auto preedit = rowValue(data, "state", "preedit");
    const auto settings = modelSettingsFrom(data);
    const auto mode = settings.mode;

    auto *learningPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *modelPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *supportPage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (auto *page : {learningPage, modelPage, supportPage}) {
        gtk_widget_add_css_class(page, "page");
    }

    auto *learningCallout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(learningCallout, "overview-callout");
    gtk_box_append(GTK_BOX(learningCallout), makeLabel(learningHeadline(overview), "overview-title"));
    gtk_box_append(GTK_BOX(learningCallout), makeLabel(learningSummaryText(overview), "overview-text"));
    gtk_box_append(GTK_BOX(learningPage), learningCallout);

    auto *automaticLearning = makeSection("日常词序学习");
    gtk_box_append(GTK_BOX(automaticLearning),
                   makeInfoRow("状态", dictionaryHistoryStatusText(overview)));
    gtk_box_append(GTK_BOX(automaticLearning),
                   makeLabel("每次正常提交中文候选都会在本机更新常用词和上下文顺序。这个过程自动完成，不需要点击“更新 TiP”，也不会运行 AI。",
                             "model-callout-text"));
    gtk_box_append(GTK_BOX(learningPage), automaticLearning);

    gtk_box_append(GTK_BOX(learningPage), makeLabel("TiP 模型已经记住", "section-title"));
    auto *metrics = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(metrics, "metric-strip");
    gtk_box_append(GTK_BOX(metrics),
                   makeMetric(std::to_string(overview.selectionRules), "常用选词", "会优先放到前面"));
    gtk_box_append(GTK_BOX(metrics),
                   makeMetric(std::to_string(overview.englishTokens), "常用英文", "会优先直接显示"));
    gtk_box_append(GTK_BOX(metrics),
                   makeMetric(std::to_string(overview.corrections), "按键纠正", "会尝试补漏打和错打"));
    gtk_box_append(GTK_BOX(learningPage), metrics);

    auto *learnAction = makeSection("更新 TiP");
    gtk_box_append(GTK_BOX(learnAction),
                   makeLabel(overview.records == 0
                                 ? "先正常使用一段时间，TiPE 有记录后才能更新。"
                                 : "点击一次，用监督记录更新漏键纠错、英文直出和候选重排。它和上面的自动词序学习互不依赖，更新时不会联网。",
                             "model-callout-text"));
    auto *learningActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(learningActions, "page-actions");
    if (!data.personalTrainCommand.empty()) {
        data.personalTrainButton = gtk_button_new_with_label("更新 TiP");
        gtk_widget_add_css_class(data.personalTrainButton, "suggested-action");
        gtk_widget_set_sensitive(data.personalTrainButton, overview.records > 0);
        g_object_add_weak_pointer(G_OBJECT(data.personalTrainButton),
                                  reinterpret_cast<gpointer *>(&data.personalTrainButton));
        g_signal_connect(data.personalTrainButton, "clicked", G_CALLBACK(personalTrainClicked), &data);
        gtk_box_append(GTK_BOX(learningActions), data.personalTrainButton);
    }
    gtk_box_append(GTK_BOX(learnAction), learningActions);
    gtk_box_append(GTK_BOX(learningPage), learnAction);

    auto *learningAdvancedBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(learningAdvancedBox),
                   makeInfoRow("完整输入记录", std::to_string(overview.records)));
    gtk_box_append(GTK_BOX(learningAdvancedBox),
                   makeInfoRow("可用于更新", std::to_string(overview.learnable)));
    gtk_box_append(GTK_BOX(learningAdvancedBox),
                   makeInfoRow("明确选词", std::to_string(overview.choices)));
    gtk_box_append(GTK_BOX(learningAdvancedBox),
                   makeInfoRow("其他按键记录", std::to_string(overview.observations)));
    gtk_box_append(GTK_BOX(learningAdvancedBox),
                   makeInfoRow("词序历史大小", std::to_string(overview.dictionaryHistoryBytes) + " 字节"));
    if (!overview.dictionaryHistoryPath.empty()) {
        gtk_box_append(GTK_BOX(learningAdvancedBox),
                       makeInfoRow("词序历史文件", overview.dictionaryHistoryPath));
    }
    gtk_box_append(GTK_BOX(learningAdvancedBox),
                   makeInfoRow("当前拼音", preedit.empty() ? "无" : preedit));
    const auto analysis = analysisStatusText(data);
    if (!analysis.empty()) {
        gtk_box_append(GTK_BOX(learningAdvancedBox), makeInfoRow("最近一次分析", analysis));
    }
    auto *refresh = gtk_button_new_with_label("刷新数据");
    gtk_widget_set_sensitive(refresh, !data.inputPath.empty() && data.inputPath != "-");
    gtk_widget_set_halign(refresh, GTK_ALIGN_START);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refreshClicked), &data);
    gtk_box_append(GTK_BOX(learningAdvancedBox), refresh);
    auto *learningAdvanced = gtk_expander_new("高级详情");
    gtk_widget_add_css_class(learningAdvanced, "advanced-details");
    gtk_expander_set_child(GTK_EXPANDER(learningAdvanced), learningAdvancedBox);
    gtk_box_append(GTK_BOX(learningPage), learningAdvanced);

    auto *modelCallout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(modelCallout, "model-callout");
    gtk_box_append(GTK_BOX(modelCallout), makeLabel("正在使用 " + modelModeTitle(mode), "model-callout-title"));
    gtk_box_append(GTK_BOX(modelCallout), makeLabel(modelModeDescription(mode), "model-callout-text"));
    gtk_box_append(GTK_BOX(modelPage), modelCallout);

    auto *provider = makeSection("选择适合你的方式");
    gtk_box_append(GTK_BOX(provider),
                   makeModelChoice("TiP 本地学习（推荐）", "免费、不联网，学习你的实际输入习惯。",
                                   mode == "personal"));
    gtk_box_append(GTK_BOX(provider),
                   makeModelChoice("本地大模型", "输入不离开电脑，但需要自己安装 Ollama 或 GGUF 模型。",
                                   mode == "ollama" || mode == "llama-cpp"));
    gtk_box_append(GTK_BOX(provider),
                   makeModelChoice("云端大模型", "需要 API Key，主动分析时按次联网，可能收费。",
                                   mode == "openai" || mode == "openai-compatible"));
    if (!data.modelConfigCommand.empty()) {
        auto *configure = gtk_button_new_with_label("选择和配置模型");
        g_signal_connect(configure, "clicked", G_CALLBACK(modelSettingsClicked), &data);
        gtk_widget_set_halign(configure, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(provider), configure);
    }
    gtk_box_append(GTK_BOX(modelPage), provider);

    auto *modelActions = makeSection("单次分析");
    gtk_box_append(GTK_BOX(modelActions),
                   makeLabel("只分析你正在输入的这一段内容。完成后模型会退出，不会在后台持续运行。",
                             "model-callout-text"));
    if (!data.refreshCommand.empty()) {
        data.analyzeButton = gtk_button_new_with_label(analyzeButtonText(data).c_str());
        gtk_widget_add_css_class(data.analyzeButton, "suggested-action");
        gtk_widget_set_halign(data.analyzeButton, GTK_ALIGN_START);
        gtk_widget_set_sensitive(data.analyzeButton, !data.inputPath.empty() && data.inputPath != "-");
        g_object_add_weak_pointer(G_OBJECT(data.analyzeButton), reinterpret_cast<gpointer *>(&data.analyzeButton));
        g_signal_connect(data.analyzeButton, "clicked", G_CALLBACK(analyzeClicked), &data);
        gtk_box_append(GTK_BOX(modelActions), data.analyzeButton);
    }
    gtk_box_append(GTK_BOX(modelPage), modelActions);

    auto *cloudFlow = makeSection("云端模型会拿到什么");
    gtk_box_append(GTK_BOX(cloudFlow), makeInfoRow("默认", "只发送当前拼音和候选"));
    gtk_box_append(GTK_BOX(cloudFlow),
                   makeInfoRow("由你决定", "可另外允许发送近期按键，或光标附近文字"));
    gtk_box_append(GTK_BOX(cloudFlow),
                   makeInfoRow("收到结果后", "TiPE 先在本机检查，只接受安全且能用的建议"));
    if (mode == "openai" || mode == "openai-compatible") {
        gtk_box_append(GTK_BOX(cloudFlow), makeInfoRow("当前允许发送", cloudDataSummary(settings)));
    }
    gtk_box_append(GTK_BOX(modelPage), cloudFlow);

    auto *modelAdvancedBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(modelAdvancedBox), makeInfoRow("内部模式", mode));
    const auto modelName = rowValue(data, "model-config", "model");
    if (!modelName.empty()) {
        gtk_box_append(GTK_BOX(modelAdvancedBox), makeInfoRow("模型名称", modelName));
    }
    const auto baseUrl = rowValue(data, "model-config", "base-url");
    if (!baseUrl.empty()) {
        gtk_box_append(GTK_BOX(modelAdvancedBox), makeInfoRow("服务地址", baseUrl));
    }
    const auto apiKeyRuntime = rowValue(data, "model-config", "api-key-runtime");
    if (!apiKeyRuntime.empty()) {
        const auto apiKeySource = rowValue(data, "model-config", "api-key-source");
        const auto keyStatus = apiKeyRuntime == "set"
                                   ? (apiKeySource == "stored-file" ? "已安全保存" : "已通过环境变量提供")
                                   : (apiKeyRuntime == "invalid" ? "保存文件不可用" : "尚未提供");
        gtk_box_append(GTK_BOX(modelAdvancedBox), makeInfoRow("API Key", keyStatus));
    }
    auto *modelAdvanced = gtk_expander_new("高级详情");
    gtk_widget_add_css_class(modelAdvanced, "advanced-details");
    gtk_expander_set_child(GTK_EXPANDER(modelAdvanced), modelAdvancedBox);
    gtk_box_append(GTK_BOX(modelPage), modelAdvanced);

    auto *supportCallout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(supportCallout, "model-callout");
    gtk_box_append(GTK_BOX(supportCallout), makeLabel("支持 TiPE", "model-callout-title"));
    gtk_box_append(GTK_BOX(supportCallout),
                   makeLabel("感谢支持 TiPE 的开发。付款前请在手机上核对收款人。", "model-callout-text"));
    gtk_box_append(GTK_BOX(supportPage), supportCallout);
    auto *supportRow = gtk_flow_box_new();
    gtk_widget_add_css_class(supportRow, "support-row");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(supportRow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(supportRow), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(supportRow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(supportRow), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(supportRow), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(supportRow), 12);
    gtk_flow_box_append(GTK_FLOW_BOX(supportRow), makeSupportSlot("微信", supportImagePath(supportImageNames[0])));
    gtk_flow_box_append(GTK_FLOW_BOX(supportRow), makeSupportSlot("支付宝", supportImagePath(supportImageNames[1])));
    gtk_box_append(GTK_BOX(supportPage), supportRow);

    auto *notebook = gtk_notebook_new();
    gtk_widget_set_hexpand(notebook, TRUE);
    gtk_widget_set_vexpand(notebook, TRUE);
    const auto appendPage = [notebook](GtkWidget *page, const char *title) {
        auto *scrolled = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled, gtk_label_new(title));
    };
    appendPage(learningPage, primaryPageTitles[0].data());
    appendPage(modelPage, primaryPageTitles[1].data());
    appendPage(supportPage, primaryPageTitles[2].data());
    gtk_box_append(GTK_BOX(container), notebook);
}

void updateStatusLabel(LearningPanelData &data, std::string_view message) {
    data.statusText = std::string(message);
    if (data.statusLabel) {
        gtk_label_set_text(GTK_LABEL(data.statusLabel), data.statusText.c_str());
    }
}

bool refreshRows(LearningPanelData &data, bool runRefreshCommand) {
    if (runRefreshCommand && !data.refreshCommand.empty()) {
        gchar *standardError = nullptr;
        gint exitStatus = 0;
        GError *error = nullptr;
        const gboolean spawned = g_spawn_command_line_sync(data.refreshCommand.c_str(), nullptr, &standardError,
                                                           &exitStatus, &error);
        if (!spawned || exitStatus != 0) {
            std::string message = "分析失败";
            if (error) {
                message += ": ";
                message += error->message;
                g_error_free(error);
            } else if (standardError && standardError[0] != '\0') {
                message += ": ";
                message += standardError;
            }
            if (standardError) {
                g_free(standardError);
            }
            updateStatusLabel(data, message);
            return false;
        }
        if (standardError) {
            g_free(standardError);
        }
    }
    if (data.inputPath.empty() || data.inputPath == "-") {
        updateStatusLabel(data, "已载入学习数据");
        return true;
    }
    if (!loadPanelData(data)) {
        updateStatusLabel(data, "刷新失败");
        return false;
    }
    if (data.rowsBox) {
        clearBox(data.rowsBox);
        renderRowsInto(data.rowsBox, data);
    }
    if (data.analyzeButton) {
        gtk_button_set_label(GTK_BUTTON(data.analyzeButton), analyzeButtonText(data).c_str());
    }
    const auto analysisStatus = analysisStatusText(data);
    updateStatusLabel(data, analysisStatus.empty() ? "学习数据已更新" : analysisStatus);
    return true;
}

void refreshClicked(GtkButton *, gpointer userData) {
    auto *data = static_cast<LearningPanelData *>(userData);
    refreshRows(*data, false);
}

void analyzeClicked(GtkButton *, gpointer userData) {
    auto *data = static_cast<LearningPanelData *>(userData);
    refreshRows(*data, true);
}

std::string commandOutputValue(std::string_view output, std::string_view key) {
    std::istringstream stream{std::string(output)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto fields = split(line, '\t');
        if (fields.size() >= 2 && fields[0] == key) {
            return fields[1];
        }
    }
    return {};
}

std::string personalTrainingSuccessMessage(const LearningPanelData &data, std::string_view output) {
    std::string message = "TiP 训练完成";
    const auto samples = commandOutputValue(output, "samples");
    const auto rankingSamples = commandOutputValue(output, "ranking-samples");
    const auto rawAuxiliary = commandOutputValue(output, "raw-profile-auxiliary-positive");
    const auto activeRawTokens = commandOutputValue(output, "active-raw-token-evidence");
    const auto patterns = commandOutputValue(output, "active-correction-patterns");
    const auto habits = commandOutputValue(output, "active-key-habits");
    const auto validation = commandOutputValue(output, "validation-accuracy");
    const auto baseline = commandOutputValue(output, "validation-baseline-accuracy");
    const auto validationGain = commandOutputValue(output, "validation-gain");
    const auto genericValidation = commandOutputValue(output, "validation-generic-non-leading-accuracy");
    const auto genericSafe = commandOutputValue(output, "generic-ranking-safe");
    const auto rawProfileValidation = commandOutputValue(output, "raw-profile-validation-accuracy");
    const auto rawProfileFalsePromotions = commandOutputValue(output, "raw-profile-validation-false-promotions");
    const auto rawProfileSafe = commandOutputValue(output, "raw-profile-safe");
    const auto keyboardCorrectionSafe = commandOutputValue(output, "keyboard-correction-safe");
    const auto runtimeRawCandidates = commandOutputValue(output, "runtime-raw-token-candidates");
    const auto runtimeRawActive = commandOutputValue(output, "runtime-raw-token-active");
    const auto runtimeRawUpdated = commandOutputValue(output, "runtime-raw-token-updated");
    const auto runtimePatternsActive = commandOutputValue(output, "runtime-correction-pattern-active");
    const auto runtimePatternsUpdated = commandOutputValue(output, "runtime-correction-pattern-updated");
    const auto runtimeHabitsActive = commandOutputValue(output, "runtime-key-habit-active");
    const auto runtimeHabitsUpdated = commandOutputValue(output, "runtime-key-habit-updated");
    const auto nonLeading = commandOutputValue(output, "non-leading-samples");
    const auto recommendation = commandOutputValue(output, "recommendation");
    const auto modelUpdated = commandOutputValue(output, "model-updated");
    const auto modelUpdateKind = commandOutputValue(output, "model-update-kind");
    const auto mergeStrategy = commandOutputValue(output, "merge-strategy");
    const auto retainedPairs = commandOutputValue(output, "retained-active-pair-evidence");
    const auto retainedRawTokens = commandOutputValue(output, "retained-active-raw-token-evidence");
    const auto retainedPatterns = commandOutputValue(output, "retained-active-correction-patterns");
    const auto retainedHabits = commandOutputValue(output, "retained-active-key-habits");
    if (!samples.empty() || !patterns.empty() || !habits.empty()) {
        message += "：";
        if (!samples.empty()) {
            message += samples + " 条监督";
            if (!rankingSamples.empty() || !rawAuxiliary.empty()) {
                message += "（";
                if (!rankingSamples.empty()) {
                    message += rankingSamples + " 条排序";
                }
                if (!rawAuxiliary.empty()) {
                    if (!rankingSamples.empty()) {
                        message += "，";
                    }
                    message += rawAuxiliary + " 条英文模式辅助";
                }
                message += "）";
            }
        }
        if (!patterns.empty()) {
            if (!samples.empty()) {
                message += "，";
            }
            message += patterns + " 个已生效纠错模式";
        }
        if (!habits.empty()) {
            if (!samples.empty() || !patterns.empty()) {
                message += "，";
            }
            message += habits + " 个全局按键习惯";
        }
        if (!activeRawTokens.empty()) {
            if (!samples.empty() || !patterns.empty() || !habits.empty()) {
                message += "，";
            }
            message += activeRawTokens + " 个已生效英文词记忆";
        }
    }
    if (!validation.empty() && validation != "unavailable") {
        message += "；分层留出验证 " + validation;
        if (!baseline.empty() && baseline != "unavailable") {
            message += "（首项基线 " + baseline + "）";
        }
        if (!validationGain.empty() && validationGain != "unavailable") {
            message += "，收益 " + validationGain;
        }
    }
    if (!nonLeading.empty()) {
        message += "；非首选训练样本 " + nonLeading;
    }
    if (!genericValidation.empty() && genericValidation != "unavailable") {
        message += "；真实泛化验证 " + genericValidation;
    }
    if (genericSafe == "0") {
        message += "；真实泛化改序仍关闭";
    } else if (genericSafe == "1") {
        message += "；真实泛化改序已通过";
    }
    if (!rawProfileValidation.empty() && rawProfileValidation != "unavailable") {
        message += "；英文原样验证 " + rawProfileValidation;
        if (!rawProfileFalsePromotions.empty() && rawProfileFalsePromotions != "unavailable") {
            message += "（错误提前 " + rawProfileFalsePromotions + "）";
        }
    }
    if (!rawProfileSafe.empty()) {
        message += rawProfileSafe == "1" ? "；英文原样泛化已通过" : "；英文原样泛化仍关闭";
    }
    if (!keyboardCorrectionSafe.empty()) {
        message += keyboardCorrectionSafe == "1" ? "；键盘习惯纠错已通过" : "；键盘习惯纠错仍关闭";
    }
    if (!runtimeRawUpdated.empty() && runtimeRawUpdated != "0") {
        message += "；普通输入新增 " + runtimeRawUpdated + " 个英文词记忆";
    } else if (!runtimeRawActive.empty() && runtimeRawActive != "0") {
        message += "；已确认英文词记忆均已在普通输入生效";
    } else if (!runtimeRawCandidates.empty() && runtimeRawCandidates != "0") {
        message += "；英文词证据已保留，尚未进入普通输入";
    }
    if ((!runtimePatternsUpdated.empty() && runtimePatternsUpdated != "0") ||
        (!runtimeHabitsUpdated.empty() && runtimeHabitsUpdated != "0")) {
        message += "；普通输入已更新纠错模式 " +
                   (runtimePatternsUpdated.empty() ? "0" : runtimePatternsUpdated) + "、按键习惯 " +
                   (runtimeHabitsUpdated.empty() ? "0" : runtimeHabitsUpdated);
    } else if ((!runtimePatternsActive.empty() && runtimePatternsActive != "0") ||
               (!runtimeHabitsActive.empty() && runtimeHabitsActive != "0")) {
        message += "；TiP 纠错模式和按键习惯已在普通输入生效";
    }
    if (modelUpdated == "0") {
        message += "；新模型未替换，已保留当前模型";
    } else if (modelUpdated == "1") {
        if (modelUpdateKind == "safe-component-validation-upgrade") {
            message += "；个人证据与训练评估协议已升级";
        } else if (modelUpdateKind == "safe-component-upgrade") {
            message += "；安全纠错组件已升级";
        } else if (modelUpdateKind == "safe-capability-upgrade") {
            message += "；已验证能力组件已升级";
        } else if (modelUpdateKind == "safe-validation-upgrade") {
            message += "；训练评估协议已升级";
        } else {
            message += "；模型文件已更新";
        }
    }
    if (mergeStrategy == "max-count-monotonic-v1") {
        message += "；旧模型已生效证据无回退";
        if (!retainedPairs.empty() || !retainedRawTokens.empty() || !retainedPatterns.empty() ||
            !retainedHabits.empty()) {
            message += "（选词 " + (retainedPairs.empty() ? "0" : retainedPairs) +
                       "，英文词 " + (retainedRawTokens.empty() ? "0" : retainedRawTokens) +
                       "，纠错 " + (retainedPatterns.empty() ? "0" : retainedPatterns) +
                       "，按键习惯 " + (retainedHabits.empty() ? "0" : retainedHabits) + "）";
        }
    }
    const bool personalActive = rowValue(data, "model-config", "configured-mode") == "personal";
    if (recommendation == "keep-heuristic") {
        message += personalActive ? "；当前仍在使用 TiP，建议切回离线启发式"
                                  : "；当前建议继续使用离线启发式";
    } else if (recommendation == "collect-more-data") {
        if (modelUpdateKind == "safe-component-validation-upgrade") {
            message += personalActive ? "；通用改序仍关闭，新个人证据已在下次按需分析中生效"
                                      : "；通用改序仍关闭，个人证据与新评估协议已保存";
        } else if (modelUpdateKind == "safe-component-upgrade") {
            message += personalActive ? "；通用改序仍关闭，下次按需分析使用新纠错组件"
                                      : "；通用改序仍关闭，排序样本还需继续积累";
        } else if (modelUpdateKind == "safe-validation-upgrade") {
            message += personalActive ? "；真实泛化仍关闭，后续训练使用无泄漏评估"
                                      : "；真实泛化仍关闭，后续训练使用新评估协议";
        } else if (modelUpdateKind == "safe-capability-upgrade") {
            message += personalActive ? "；已验证能力已更新，下次按需分析生效"
                                      : "；已验证能力已更新，可按需启用 TiP";
        } else {
            message += personalActive ? "；当前仍在使用 TiP，但样本仍然较少"
                                      : "；样本较少，暂不建议切换";
        }
    } else if (personalActive && modelUpdated == "1") {
        message += "；当前 TiP 已更新，下次按需分析立即生效";
    } else if (personalActive) {
        message += "；当前 TiP 会继续使用已存在的模型文件";
    } else {
        message += "；可在“模型设置”中选择 TiP";
    }
    return message;
}

std::string personalTrainingUserMessage(const LearningPanelData &data, std::string_view output) {
    const bool runtimeSyncFailed = commandOutputValue(output, "runtime-distill") == "failed";
    if (commandOutputValue(output, "model-updated") == "0") {
        return std::string("检查完成。新记录还不足以改善 TiP，已继续使用当前版本。") +
               (runtimeSyncFailed ? "即时学习同步暂未完成，下次更新会自动重试。" : "");
    }
    const auto overview = learningOverviewFrom(data);
    auto message = "TiP 已更新：记住了 " + std::to_string(overview.selectionRules) + " 个选词、" +
                   std::to_string(overview.englishTokens) + " 个英文词和 " +
                   std::to_string(overview.corrections) + " 种按键纠正。";
    if (runtimeSyncFailed) {
        message += "即时学习同步暂未完成，下次更新会自动重试。";
    }
    return message;
}

void personalTrainingFinished(GObject *source, GAsyncResult *result, gpointer userData) {
    auto *data = static_cast<LearningPanelData *>(userData);
    gchar *standardOutput = nullptr;
    gchar *standardError = nullptr;
    GError *error = nullptr;
    const gboolean communicated = g_subprocess_communicate_utf8_finish(
        G_SUBPROCESS(source), result, &standardOutput, &standardError, &error);
    const bool succeeded = communicated && g_subprocess_get_successful(G_SUBPROCESS(source));
    if (succeeded) {
        refreshModelConfigRows(*data);
        const std::string_view output = standardOutput ? standardOutput : "";
        updateStatusLabel(*data, personalTrainingUserMessage(*data, output));
    } else {
        std::string message = "TiP 训练失败";
        if (error) {
            message += ": ";
            message += error->message;
        } else if (standardError && standardError[0] != '\0') {
            message += ": ";
            message += standardError;
        }
        updateStatusLabel(*data, message);
    }
    if (data->personalTrainButton) {
        gtk_widget_set_sensitive(data->personalTrainButton, TRUE);
    }
    if (data->analyzeButton) {
        gtk_widget_set_sensitive(data->analyzeButton, !data->inputPath.empty() && data->inputPath != "-");
    }
    if (error) {
        g_error_free(error);
    }
    g_free(standardOutput);
    g_free(standardError);
}

void startPersonalTraining(LearningPanelData &data) {
    if (data.personalTrainCommand.empty()) {
        return;
    }
    GError *error = nullptr;
    const auto flags = static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    auto arguments = personalTrainingArguments(data);
    std::vector<const gchar *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    auto *process = g_subprocess_newv(argv.data(), flags, &error);
    if (!process) {
        std::string message = "无法启动 TiP 训练";
        if (error) {
            message += ": ";
            message += error->message;
            g_error_free(error);
        }
        updateStatusLabel(data, message);
        return;
    }
    if (data.personalTrainButton) {
        gtk_widget_set_sensitive(data.personalTrainButton, FALSE);
    }
    if (data.analyzeButton) {
        gtk_widget_set_sensitive(data.analyzeButton, FALSE);
    }
    updateStatusLabel(data, "正在用历史样本更新 TiP…");
    g_subprocess_communicate_utf8_async(process, nullptr, nullptr, personalTrainingFinished, &data);
    g_object_unref(process);
}

void personalTrainClicked(GtkButton *, gpointer userData) {
    startPersonalTraining(*static_cast<LearningPanelData *>(userData));
}

void setModelConfigRow(LearningPanelData &data, std::string_view key, std::string value) {
    for (auto row = data.rows.begin(); row != data.rows.end(); ++row) {
        if (row->section == "model-config" && row->key == key) {
            if (value.empty()) {
                data.rows.erase(row);
            } else {
                row->values = {std::move(value)};
            }
            return;
        }
    }
    if (!value.empty()) {
        data.rows.push_back({"model-config", std::string(key), {std::move(value)}});
    }
}

void applyModelSettingsToPanel(LearningPanelData &data, const ModelSettings &settings) {
    setModelConfigRow(data, "configured-mode", settings.mode);
    setModelConfigRow(data, "continuous-default", settings.continuous ? "1" : "0");
    setModelConfigRow(data, "training-context", settings.trainingContext ? "1" : "0");
    setModelConfigRow(data, "training-surrounding", settings.trainingSurrounding ? "1" : "0");
    setModelConfigRow(data, "send-recent-input", settings.cloudRecentInput ? "1" : "0");
    setModelConfigRow(data, "send-surrounding", settings.cloudSurrounding ? "1" : "0");
    setModelConfigRow(data, "model", settings.model);
    setModelConfigRow(data, "base-url", settings.baseUrl);
    setModelConfigRow(data, "chat-path", settings.chatPath);
    setModelConfigRow(data, "api-key-env", settings.apiKeyEnv);
    if (!settings.apiKey.empty()) {
        setModelConfigRow(data, "api-key-source", "stored-file");
        setModelConfigRow(data, "api-key-runtime", "set");
        setModelConfigRow(data, "api-key-env", "");
    } else if (settings.clearApiKey) {
        setModelConfigRow(data, "api-key-source", settings.apiKeyEnv.empty() ? "none" : "environment");
        setModelConfigRow(data, "api-key-runtime", "unset");
    }
    setModelConfigRow(data, "custom-command", settings.customCommand);
    setModelConfigRow(data, "personal-model", settings.personalModelPath);
    setModelConfigRow(data, "dump-path", settings.dumpPath);
    setModelConfigRow(data, "llama-command", settings.llamaCommand);
    setModelConfigRow(data, "llama-threads", settings.llamaThreads);
    setModelConfigRow(data, "llama-context", settings.llamaContext);
    setModelConfigRow(data, "timeout", settings.timeout);
    setModelConfigRow(data, "http-timeout", settings.httpTimeout);
    setModelConfigRow(data, "temperature", settings.temperature);
    setModelConfigRow(data, "max-tokens", settings.maxTokens);
    if (data.analyzeButton) {
        gtk_button_set_label(GTK_BUTTON(data.analyzeButton), analyzeButtonText(data).c_str());
    }
    if (data.rowsBox) {
        clearBox(data.rowsBox);
        renderRowsInto(data.rowsBox, data);
    }
}

bool applyModelConfigShowOutput(LearningPanelData &data, std::string_view output) {
    std::vector<LearningPanelRow> modelRows;
    std::istringstream stream{std::string(output)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto fields = split(line, '\t');
        if (fields.size() < 3 || fields[0] != "model-status") {
            continue;
        }
        modelRows.push_back({"model-config", fields[1], {fields[2]}});
    }
    if (modelRows.empty()) {
        return false;
    }
    std::erase_if(data.rows, [](const auto &row) { return row.section == "model-config"; });
    data.rows.insert(data.rows.end(), modelRows.begin(), modelRows.end());
    if (data.analyzeButton) {
        gtk_button_set_label(GTK_BUTTON(data.analyzeButton), analyzeButtonText(data).c_str());
    }
    if (data.rowsBox) {
        clearBox(data.rowsBox);
        renderRowsInto(data.rowsBox, data);
    }
    return true;
}

bool refreshModelConfigRows(LearningPanelData &data) {
    if (data.modelConfigCommand.empty()) {
        return false;
    }
    gchar *standardOutput = nullptr;
    gchar *standardError = nullptr;
    gint waitStatus = 0;
    GError *error = nullptr;
    gchar *argv[] = {const_cast<gchar *>(data.modelConfigCommand.c_str()), const_cast<gchar *>("--show"), nullptr};
    const gboolean spawned = g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                                          &standardOutput, &standardError, &waitStatus, &error);
    const bool succeeded = spawned && g_spawn_check_wait_status(waitStatus, nullptr) && standardOutput &&
                           applyModelConfigShowOutput(data, standardOutput);
    if (error) {
        g_error_free(error);
    }
    g_free(standardOutput);
    g_free(standardError);
    return succeeded;
}

std::size_t selectedModeIndex(std::string_view mode) {
    for (std::size_t index = 0; index < std::size(modelModes); ++index) {
        if (modelModes[index] == mode) {
            return index;
        }
    }
    return 0;
}

ModelSettings settingsFromWidgets(const ModelSettingsWindow &widgets) {
    ModelSettings settings;
    const auto current = modelSettingsFrom(*widgets.panel);
    const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(widgets.mode));
    settings.mode = selected < std::size(modelModes) ? std::string(modelModes[selected]) : "heuristic";
    settings.model = gtk_editable_get_text(GTK_EDITABLE(widgets.model));
    settings.baseUrl = gtk_editable_get_text(GTK_EDITABLE(widgets.baseUrl));
    settings.chatPath = gtk_editable_get_text(GTK_EDITABLE(widgets.chatPath));
    settings.apiKeyEnv = gtk_editable_get_text(GTK_EDITABLE(widgets.apiKeyEnv));
    settings.apiKey = gtk_editable_get_text(GTK_EDITABLE(widgets.apiKey));
    settings.apiKeyStored = current.apiKeyStored;
    settings.apiKeyRuntime = current.apiKeyRuntime;
    settings.clearApiKey = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets.clearApiKey));
    if (!settings.apiKey.empty()) {
        settings.apiKeyEnv.clear();
        settings.clearApiKey = false;
    }
    settings.customCommand = gtk_editable_get_text(GTK_EDITABLE(widgets.customCommand));
    settings.personalModelPath = gtk_editable_get_text(GTK_EDITABLE(widgets.personalModelPath));
    settings.dumpPath = gtk_editable_get_text(GTK_EDITABLE(widgets.dumpPath));
    settings.llamaCommand = gtk_editable_get_text(GTK_EDITABLE(widgets.llamaCommand));
    settings.llamaThreads = gtk_editable_get_text(GTK_EDITABLE(widgets.llamaThreads));
    settings.llamaContext = gtk_editable_get_text(GTK_EDITABLE(widgets.llamaContext));
    settings.timeout = gtk_editable_get_text(GTK_EDITABLE(widgets.timeout));
    settings.httpTimeout = gtk_editable_get_text(GTK_EDITABLE(widgets.httpTimeout));
    settings.temperature = gtk_editable_get_text(GTK_EDITABLE(widgets.temperature));
    settings.maxTokens = gtk_editable_get_text(GTK_EDITABLE(widgets.maxTokens));
    settings.continuous = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets.continuous));
    settings.trainingContext = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets.trainingContext));
    settings.trainingSurrounding = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets.trainingSurrounding));
    settings.cloudRecentInput = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets.cloudRecentInput));
    settings.cloudSurrounding = gtk_check_button_get_active(GTK_CHECK_BUTTON(widgets.cloudSurrounding));
    return settings;
}

void updateModelSettingsSensitivity(ModelSettingsWindow &widgets) {
    const auto settings = settingsFromWidgets(widgets);
    const bool endpointMode = settings.mode == "ollama" || settings.mode == "openai" ||
                              settings.mode == "openai-compatible";
    const bool cloudMode = settings.mode == "openai" || settings.mode == "openai-compatible";
    const bool namedModelMode = endpointMode || settings.mode == "llama-cpp";
    for (auto *widget : {widgets.modelLabel, widgets.model}) {
        gtk_widget_set_visible(widget, namedModelMode);
    }
    const bool editableBaseUrl = settings.mode == "ollama" || settings.mode == "openai-compatible";
    for (auto *widget : {widgets.baseUrlLabel, widgets.baseUrl}) {
        gtk_widget_set_visible(widget, editableBaseUrl);
    }
    for (auto *widget : {widgets.chatPathLabel, widgets.chatPath}) {
        gtk_widget_set_visible(widget, editableBaseUrl);
    }
    for (auto *widget : {widgets.apiKeyEnvLabel, widgets.apiKeyEnv, widgets.apiKeyLabel, widgets.apiKey,
                         widgets.clearApiKey, widgets.cloudPrivacy}) {
        gtk_widget_set_visible(widget, cloudMode);
    }
    gtk_label_set_text(GTK_LABEL(widgets.modelLabel), settings.mode == "llama-cpp" ? "GGUF 模型文件" : "模型名称");
    if (settings.mode == "llama-cpp") {
        gtk_entry_set_placeholder_text(GTK_ENTRY(widgets.model),
                                       "/home/user/.local/share/tipe/models/model.gguf");
    } else if (settings.mode == "ollama") {
        gtk_entry_set_placeholder_text(GTK_ENTRY(widgets.model), "例如 qwen2.5:0.5b");
    } else {
        gtk_entry_set_placeholder_text(GTK_ENTRY(widgets.model), "输入服务商提供的模型名称");
    }
    gtk_label_set_text(GTK_LABEL(widgets.baseUrlLabel),
                       settings.mode == "ollama" ? "Ollama 地址" : "服务商 API 地址");
    gtk_label_set_text(GTK_LABEL(widgets.modeHint), modelSetupHint(settings.mode).c_str());
    const bool customMode = settings.mode == "custom";
    gtk_widget_set_visible(widgets.customCommandLabel, customMode);
    gtk_widget_set_visible(widgets.customCommand, customMode);
    const bool personalMode = settings.mode == "personal";
    gtk_widget_set_visible(widgets.personalModelPathLabel, personalMode);
    gtk_widget_set_visible(widgets.personalModelPath, personalMode);
    gtk_widget_set_visible(widgets.trainingContext, personalMode);
    gtk_widget_set_visible(widgets.trainingSurrounding, personalMode);
    const bool dumpMode = settings.mode == "dump";
    gtk_widget_set_visible(widgets.dumpPathLabel, dumpMode);
    gtk_widget_set_visible(widgets.dumpPath, dumpMode);
    const bool llamaMode = settings.mode == "llama-cpp";
    for (auto *widget : {widgets.llamaCommandLabel, widgets.llamaCommand, widgets.llamaThreadsLabel,
                         widgets.llamaThreads, widgets.llamaContextLabel, widgets.llamaContext}) {
        gtk_widget_set_visible(widget, llamaMode);
    }
    for (auto *widget : {widgets.httpTimeoutLabel, widgets.httpTimeout, widgets.temperatureLabel,
                         widgets.temperature, widgets.maxTokensLabel, widgets.maxTokens}) {
        gtk_widget_set_visible(widget, endpointMode);
    }
    gtk_widget_set_visible(widgets.timeoutLabel, settings.mode != "off");
    gtk_widget_set_visible(widgets.timeout, settings.mode != "off");
    gtk_widget_set_visible(widgets.continuous, settings.mode != "off");
}

void modelModeChanged(GObject *, GParamSpec *, gpointer userData) {
    updateModelSettingsSensitivity(*static_cast<ModelSettingsWindow *>(userData));
}

void cancelModelSettings(GtkButton *, gpointer userData) {
    auto *widgets = static_cast<ModelSettingsWindow *>(userData);
    gtk_window_destroy(GTK_WINDOW(widgets->window));
}

std::string modelSettingsValidationError(const ModelSettings &settings) {
    const bool cloudMode = settings.mode == "openai" || settings.mode == "openai-compatible";
    const bool namedModelMode = cloudMode || settings.mode == "ollama" || settings.mode == "llama-cpp";
    if (namedModelMode && settings.model.empty()) {
        return settings.mode == "llama-cpp" ? "还没有选择 GGUF 模型文件。" : "还没有填写模型名称。";
    }
    if (settings.mode == "openai-compatible" && settings.baseUrl.empty()) {
        return "还没有填写服务商 API 地址。";
    }
    const bool hasCloudKey = !settings.apiKey.empty() || !settings.apiKeyEnv.empty() ||
                             (settings.apiKeyRuntime == "set" && !settings.clearApiKey);
    if (cloudMode && !hasCloudKey) {
        return "还没有填写 API Key。它需要从模型服务商的 API 平台创建。";
    }
    if (settings.mode == "custom" && settings.customCommand.empty()) {
        return "还没有填写自定义模型命令。";
    }
    return {};
}

void checkModelSettings(GtkButton *, gpointer userData) {
    auto *widgets = static_cast<ModelSettingsWindow *>(userData);
    const auto settings = settingsFromWidgets(*widgets);
    const auto validationError = modelSettingsValidationError(settings);
    if (!validationError.empty()) {
        gtk_label_set_text(GTK_LABEL(widgets->status), validationError.c_str());
        return;
    }
    auto arguments = modelConfigArguments(settings);
    arguments.push_back("--dry-run");
    std::vector<const gchar *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(widgets->panel->modelConfigCommand.c_str());
    for (const auto &argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    auto flags = static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (!settings.apiKey.empty()) {
        flags = static_cast<GSubprocessFlags>(flags | G_SUBPROCESS_FLAGS_STDIN_PIPE);
    }
    GError *error = nullptr;
    gchar *standardOutput = nullptr;
    gchar *standardError = nullptr;
    gtk_widget_set_sensitive(widgets->check, FALSE);
    auto *process = g_subprocess_newv(argv.data(), flags, &error);
    const auto input = settings.apiKey.empty() ? std::string{} : settings.apiKey + "\n";
    const gboolean communicated =
        process && g_subprocess_communicate_utf8(process, input.empty() ? nullptr : input.c_str(), nullptr,
                                                 &standardOutput, &standardError, &error);
    if (communicated && g_subprocess_get_successful(process)) {
        gtk_label_set_text(GTK_LABEL(widgets->status), "填写内容可以保存。这个检查没有联网，也不会产生费用。");
    } else {
        std::string message = "检查失败";
        if (error) {
            message += ": ";
            message += error->message;
        } else if (standardError && standardError[0] != '\0') {
            message += ": ";
            message += standardError;
        }
        gtk_label_set_text(GTK_LABEL(widgets->status), message.c_str());
    }
    gtk_widget_set_sensitive(widgets->check, TRUE);
    if (error) {
        g_error_free(error);
    }
    if (process) {
        g_object_unref(process);
    }
    g_free(standardOutput);
    g_free(standardError);
}

void saveModelSettings(GtkButton *, gpointer userData) {
    auto *widgets = static_cast<ModelSettingsWindow *>(userData);
    const auto settings = settingsFromWidgets(*widgets);
    const auto validationError = modelSettingsValidationError(settings);
    if (!validationError.empty()) {
        gtk_label_set_text(GTK_LABEL(widgets->status), validationError.c_str());
        return;
    }
    auto arguments = modelConfigArguments(settings);
    std::vector<const gchar *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(widgets->panel->modelConfigCommand.c_str());
    for (const auto &argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    gchar *standardOutput = nullptr;
    gchar *standardError = nullptr;
    GError *error = nullptr;
    gtk_widget_set_sensitive(widgets->save, FALSE);
    auto flags = static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (!settings.apiKey.empty()) {
        flags = static_cast<GSubprocessFlags>(flags | G_SUBPROCESS_FLAGS_STDIN_PIPE);
    }
    auto *process = g_subprocess_newv(argv.data(), flags, &error);
    const auto input = settings.apiKey.empty() ? std::string{} : settings.apiKey + "\n";
    const gboolean communicated =
        process && g_subprocess_communicate_utf8(process, input.empty() ? nullptr : input.c_str(), nullptr,
                                                 &standardOutput, &standardError, &error);
    const bool succeeded = communicated && g_subprocess_get_successful(process);
    if (!succeeded) {
        std::string message = "保存失败";
        if (error) {
            message += ": ";
            message += error->message;
        } else if (standardError && standardError[0] != '\0') {
            message += ": ";
            message += standardError;
        }
        gtk_label_set_text(GTK_LABEL(widgets->status), message.c_str());
        gtk_widget_set_sensitive(widgets->save, TRUE);
    } else {
        if (!refreshModelConfigRows(*widgets->panel)) {
            applyModelSettingsToPanel(*widgets->panel, settings);
        }
        updateStatusLabel(*widgets->panel,
                          "模型设置已保存。本窗口立即使用；输入法内分析会在下次重启后使用。");
        gtk_window_destroy(GTK_WINDOW(widgets->window));
    }
    if (error) {
        g_error_free(error);
    }
    if (process) {
        g_object_unref(process);
    }
    g_free(standardOutput);
    g_free(standardError);
}

GtkWidget *attachSettingsField(GtkGrid *grid, int row, const char *labelText, GtkWidget *field) {
    auto *label = makeLabel(labelText, "field-label");
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_grid_attach(grid, field, 1, row, 1, 1);
    return label;
}

void modelSettingsClicked(GtkButton *, gpointer userData) {
    auto *panel = static_cast<LearningPanelData *>(userData);
    auto *widgets = new ModelSettingsWindow;
    widgets->panel = panel;
    const auto current = modelSettingsFrom(*panel);

    widgets->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(widgets->window), "TiPE 选择模型");
    gtk_window_set_icon_name(GTK_WINDOW(widgets->window), "tipe");
    gtk_window_set_default_size(GTK_WINDOW(widgets->window), 620, 600);
    gtk_window_set_modal(GTK_WINDOW(widgets->window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(widgets->window), GTK_WINDOW(panel->window));
    g_object_set_data_full(G_OBJECT(widgets->window), "tipe-model-settings", widgets,
                           [](gpointer value) { delete static_cast<ModelSettingsWindow *>(value); });

    auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(root, "settings-root");
    gtk_box_append(GTK_BOX(root), makeLabel("选择模型", "title"));
    gtk_box_append(GTK_BOX(root),
                   makeLabel("先选择一种使用方式。云端模型只会在你主动分析时联网。",
                             "settings-hint"));

    auto *grid = GTK_GRID(gtk_grid_new());
    gtk_widget_add_css_class(GTK_WIDGET(grid), "settings-grid");
    gtk_grid_set_row_spacing(grid, 10);
    gtk_grid_set_column_spacing(grid, 12);
    auto *advancedGrid = GTK_GRID(gtk_grid_new());
    gtk_widget_add_css_class(GTK_WIDGET(advancedGrid), "settings-grid");
    gtk_grid_set_row_spacing(advancedGrid, 10);
    gtk_grid_set_column_spacing(advancedGrid, 12);
    const char *modeLabels[] = {"TiP 本地学习（推荐）", "Ollama 本地大模型", "GGUF 本地大模型",
                                "OpenAI 云端模型", "其他兼容云端服务", "基础规则（高级）",
                                "自定义模型（高级）", "请求转储（高级）", "关闭智能分析", nullptr};
    widgets->mode = gtk_drop_down_new_from_strings(modeLabels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(widgets->mode), selectedModeIndex(current.mode));
    attachSettingsField(grid, 0, "使用方式", widgets->mode);
    widgets->modeHint = makeLabel(modelSetupHint(current.mode), "settings-hint");
    gtk_grid_attach(grid, widgets->modeHint, 1, 1, 1, 1);
    widgets->model = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->model), current.model.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->model), "例如 qwen2.5:0.5b");
    widgets->modelLabel = attachSettingsField(grid, 2, "模型名称", widgets->model);
    widgets->baseUrl = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->baseUrl), current.baseUrl.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->baseUrl), "例如 http://127.0.0.1:11434/v1");
    widgets->baseUrlLabel = attachSettingsField(grid, 3, "服务地址", widgets->baseUrl);
    widgets->chatPath = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->chatPath), current.chatPath.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->chatPath), "/chat/completions");
    widgets->chatPathLabel = attachSettingsField(advancedGrid, 0, "接口路径", widgets->chatPath);
    widgets->apiKey = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(widgets->apiKey), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->apiKey), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->apiKey),
                                   current.apiKeyRuntime == "set"
                                       ? "已保存；留空保持不变"
                                       : (current.apiKeyStored ? "保存文件不可用；可粘贴新 Key" : "粘贴 API Key"));
    gtk_widget_set_tooltip_text(widgets->apiKey, "密钥不会写入模型配置或命令行");
    widgets->apiKeyLabel = attachSettingsField(grid, 4, "API Key", widgets->apiKey);
    widgets->clearApiKey = gtk_check_button_new_with_label("删除已保存的 API Key");
    gtk_widget_set_sensitive(widgets->clearApiKey, current.apiKeyStored);
    gtk_grid_attach(grid, widgets->clearApiKey, 1, 5, 1, 1);
    widgets->apiKeyEnv = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->apiKeyEnv), current.apiKeyEnv.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->apiKeyEnv), "例如 OPENAI_API_KEY");
    gtk_widget_set_tooltip_text(widgets->apiKeyEnv, "TiPE 只保存变量名，不保存 API 密钥本身");
    widgets->apiKeyEnvLabel = attachSettingsField(advancedGrid, 1, "API 密钥变量", widgets->apiKeyEnv);
    widgets->customCommand = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->customCommand), current.customCommand.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->customCommand), "/path/to/model-wrapper");
    widgets->customCommandLabel = attachSettingsField(advancedGrid, 2, "自定义命令", widgets->customCommand);
    widgets->personalModelPath = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->personalModelPath), current.personalModelPath.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->personalModelPath),
                                   "默认 ~/.local/share/tipe/personal-reranker.json");
    widgets->personalModelPathLabel =
        attachSettingsField(advancedGrid, 3, "TiP 模型文件", widgets->personalModelPath);
    widgets->dumpPath = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->dumpPath), current.dumpPath.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->dumpPath), "默认 ~/.cache/tipe/model-request.tsv");
    widgets->dumpPathLabel = attachSettingsField(advancedGrid, 4, "转储文件", widgets->dumpPath);
    widgets->llamaCommand = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->llamaCommand), current.llamaCommand.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->llamaCommand), "/usr/bin/llama-cli");
    widgets->llamaCommandLabel = attachSettingsField(advancedGrid, 5, "llama.cpp 命令", widgets->llamaCommand);
    widgets->llamaThreads = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->llamaThreads), current.llamaThreads.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->llamaThreads), "6");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->llamaThreads), GTK_INPUT_PURPOSE_DIGITS);
    widgets->llamaThreadsLabel =
        attachSettingsField(advancedGrid, 6, "本地模型线程数", widgets->llamaThreads);
    widgets->llamaContext = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->llamaContext), current.llamaContext.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->llamaContext), "8192");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->llamaContext), GTK_INPUT_PURPOSE_DIGITS);
    widgets->llamaContextLabel =
        attachSettingsField(advancedGrid, 7, "本地模型上下文", widgets->llamaContext);
    widgets->timeout = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->timeout), current.timeout.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->timeout), "2");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->timeout), GTK_INPUT_PURPOSE_DIGITS);
    widgets->timeoutLabel = attachSettingsField(advancedGrid, 8, "分析超时（秒）", widgets->timeout);
    widgets->httpTimeout = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->httpTimeout), current.httpTimeout.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->httpTimeout), "8");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->httpTimeout), GTK_INPUT_PURPOSE_DIGITS);
    widgets->httpTimeoutLabel =
        attachSettingsField(advancedGrid, 9, "网络超时（秒）", widgets->httpTimeout);
    widgets->temperature = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->temperature), current.temperature.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->temperature), "0");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->temperature), GTK_INPUT_PURPOSE_NUMBER);
    widgets->temperatureLabel = attachSettingsField(advancedGrid, 10, "模型温度", widgets->temperature);
    widgets->maxTokens = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(widgets->maxTokens), current.maxTokens.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->maxTokens), "128");
    gtk_entry_set_input_purpose(GTK_ENTRY(widgets->maxTokens), GTK_INPUT_PURPOSE_DIGITS);
    widgets->maxTokensLabel = attachSettingsField(advancedGrid, 11, "最大输出 token", widgets->maxTokens);
    widgets->continuous = gtk_check_button_new_with_label("默认启用连续轻量重排");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->continuous), current.continuous);
    gtk_grid_attach(advancedGrid, widgets->continuous, 1, 12, 1, 1);
    widgets->trainingContext = gtk_check_button_new_with_label("TiP 训练使用近期提交上下文指纹");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->trainingContext), current.trainingContext);
    gtk_widget_set_tooltip_text(widgets->trainingContext, "只使用稳定指纹，不导出原始文本");
    gtk_grid_attach(advancedGrid, widgets->trainingContext, 1, 13, 1, 1);
    widgets->trainingSurrounding = gtk_check_button_new_with_label("TiP 训练使用光标周围上下文指纹");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->trainingSurrounding), current.trainingSurrounding);
    gtk_widget_set_tooltip_text(widgets->trainingSurrounding, "只使用稳定指纹，不导出原始文本");
    gtk_grid_attach(advancedGrid, widgets->trainingSurrounding, 1, 14, 1, 1);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(grid));

    widgets->cloudPrivacy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(widgets->cloudPrivacy, "privacy-box");
    gtk_box_append(GTK_BOX(widgets->cloudPrivacy), makeLabel("允许发送的数据", "section-title"));
    gtk_box_append(GTK_BOX(widgets->cloudPrivacy),
                   makeLabel("当前拼音和候选始终会发送。下面两项默认关闭。", "settings-hint"));
    widgets->cloudRecentInput =
        gtk_check_button_new_with_label("同时发送近期按键和修改记录（更容易理解漏打、错打）");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->cloudRecentInput), current.cloudRecentInput);
    gtk_box_append(GTK_BOX(widgets->cloudPrivacy), widgets->cloudRecentInput);
    widgets->cloudSurrounding =
        gtk_check_button_new_with_label("同时发送光标附近文字和应用名称（更容易理解上下文）");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(widgets->cloudSurrounding), current.cloudSurrounding);
    gtk_box_append(GTK_BOX(widgets->cloudPrivacy), widgets->cloudSurrounding);
    gtk_box_append(GTK_BOX(root), widgets->cloudPrivacy);

    auto *advanced = gtk_expander_new("高级设置");
    gtk_widget_add_css_class(advanced, "advanced-settings");
    gtk_expander_set_child(GTK_EXPANDER(advanced), GTK_WIDGET(advancedGrid));
    gtk_box_append(GTK_BOX(root), advanced);

    widgets->status = makeLabel("保存后，本窗口立即使用；输入法内的分析会在下次重启后使用。", "status");
    gtk_box_append(GTK_BOX(root), widgets->status);
    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    auto *cancel = gtk_button_new_with_label("取消");
    widgets->check = gtk_button_new_with_label("检查填写内容");
    widgets->save = gtk_button_new_with_label("保存设置");
    gtk_widget_add_css_class(widgets->save, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancelModelSettings), widgets);
    g_signal_connect(widgets->check, "clicked", G_CALLBACK(checkModelSettings), widgets);
    g_signal_connect(widgets->save, "clicked", G_CALLBACK(saveModelSettings), widgets);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), widgets->check);
    gtk_box_append(GTK_BOX(actions), widgets->save);
    gtk_box_append(GTK_BOX(root), actions);
    g_signal_connect(widgets->mode, "notify::selected", G_CALLBACK(modelModeChanged), widgets);
    updateModelSettingsSensitivity(*widgets);
    auto *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), root);
    gtk_window_set_child(GTK_WINDOW(widgets->window), scrolled);
    gtk_window_present(GTK_WINDOW(widgets->window));
}

void activate(GtkApplication *app, gpointer userData) {
    auto *data = static_cast<LearningPanelData *>(userData);
    if (!loadPanelData(*data)) {
        std::cerr << "无法读取面板数据\n";
        g_application_quit(G_APPLICATION(app));
        return;
    }
    installCss();
    data->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(data->window), data->windowTitle.c_str());
    gtk_window_set_icon_name(GTK_WINDOW(data->window), "tipe");
    gtk_window_set_default_size(GTK_WINDOW(data->window), 720, 560);

    data->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(data->content, "root");
    auto *brandHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(brandHeader, "brand-header");
    auto *brandIcon = gtk_image_new_from_icon_name("tipe");
    gtk_image_set_pixel_size(GTK_IMAGE(brandIcon), 36);
    gtk_box_append(GTK_BOX(brandHeader), brandIcon);
    auto *brandText = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(brandText), makeLabel("TiPE", "brand-title"));
    gtk_box_append(GTK_BOX(brandText), makeLabel("学习情况与模型设置", "brand-subtitle"));
    gtk_box_append(GTK_BOX(brandHeader), brandText);
    gtk_box_append(GTK_BOX(data->content), brandHeader);
    data->statusLabel = makeLabel("", "status");
    gtk_widget_add_css_class(data->statusLabel, "toolbar-status");
    gtk_box_append(GTK_BOX(data->content), data->statusLabel);
    data->rowsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(data->rowsBox, TRUE);
    gtk_widget_set_vexpand(data->rowsBox, TRUE);
    renderRowsInto(data->rowsBox, *data);
    gtk_box_append(GTK_BOX(data->content), data->rowsBox);
    updateStatusLabel(*data, data->inputPath.empty() || data->inputPath == "-" ? "已载入学习数据"
                                                                           : "学习数据已更新");
    gtk_window_set_child(GTK_WINDOW(data->window), data->content);
    gtk_window_present(GTK_WINDOW(data->window));
}

bool selfTest() {
    if (primaryPageTitles != std::array<std::string_view, 3>{"学习", "模型", "支持"}) {
        std::cerr << "primary window should expose exactly learning, model, and support pages\n";
        return false;
    }
    if (supportImageNames != std::array<std::string_view, 2>{"wechat", "alipay"}) {
        std::cerr << "support page should keep stable WeChat and Alipay asset names\n";
        return false;
    }
    LearningPanelData overviewData;
    overviewData.rows = {
        {"training", "summary", {"records", "124", "learnable", "119", "choices", "82", "observations", "37"}},
        {"model-config", "personal-model-status", {"ready"}},
        {"model-config", "personal-model-active-pair-evidence", {"53"}},
        {"model-config", "personal-model-active-raw-token-evidence", {"11"}},
        {"model-config", "personal-model-active-correction-patterns", {"19"}},
        {"model-config", "personal-model-active-key-habits", {"7"}},
        {"learning", "dictionary-history",
         {"status", "ready", "bytes", "4096", "path", "/tmp/user.history", "reason", "healthy"}},
    };
    const auto overview = learningOverviewFrom(overviewData);
    if (overview.records != 124 || overview.learnable != 119 || overview.choices != 82 ||
        overview.observations != 37 || overview.selectionRules != 53 || overview.englishTokens != 11 ||
        overview.corrections != 26 || !overview.modelReady || overview.dictionaryHistoryStatus != "ready" ||
        overview.dictionaryHistoryBytes != 4096 || overview.dictionaryHistoryPath != "/tmp/user.history" ||
        dictionaryHistoryStatusText(overview) != "正在工作" ||
        learningHeadline(overview) != "TiPE 正在使用学到的输入习惯" ||
        learningSummaryText(overview) != "已经记录 124 次完整输入，其中 119 条可以用来更新 TiP。") {
        std::cerr << "learning overview should use plain user-facing outcomes\n";
        return false;
    }
    ModelSettings privateCloudSettings;
    if (cloudDataSummary(privateCloudSettings) != "当前拼音和候选") {
        std::cerr << "cloud data sharing should default to the current composition only\n";
        return false;
    }
    privateCloudSettings.mode = "openai-compatible";
    if (modelSettingsValidationError(privateCloudSettings) != "还没有填写模型名称。") {
        std::cerr << "cloud setup should explain the first missing user field\n";
        return false;
    }
    LearningPanelData data;
    char program[] = "tipe-learning-panel-window";
    char titleOption[] = "--title";
    char titleValue[] = "TiPE";
    char refreshOption[] = "--refresh-command";
    char refreshValue[] = "/bin/true";
    char modelConfigOption[] = "--model-config-command";
    char modelConfigValue[] = "/tmp/tipe-model-config";
    char personalTrainOption[] = "--personal-train-command";
    char personalTrainValue[] = "/tmp/tipe-personal-train";
    char *titleArgv[] = {program, titleOption, titleValue, refreshOption, refreshValue, modelConfigOption,
                         modelConfigValue, personalTrainOption, personalTrainValue};
    auto parsedTitle = parseArgs(9, titleArgv);
    if (parsedTitle.windowTitle != "TiPE" || parsedTitle.refreshCommand != "/bin/true" ||
        parsedTitle.modelConfigCommand != "/tmp/tipe-model-config" ||
        parsedTitle.personalTrainCommand != "/tmp/tipe-personal-train") {
        std::cerr << "window title argument should parse\n";
        return false;
    }
    if (commandOutputValue("samples\t12\r\nactive-correction-patterns\t3\n", "samples") != "12" ||
        commandOutputValue("samples\t12\r\nactive-correction-patterns\t3\n", "active-correction-patterns") != "3" ||
        !commandOutputValue("samples\t12\n", "missing").empty()) {
        std::cerr << "personal training output values should parse\n";
        return false;
    }
    LearningPanelData activePersonalModel;
    activePersonalModel.rows.push_back({"model-config", "configured-mode", {"personal"}});
    if (personalTrainingUserMessage(activePersonalModel,
                                    "model-updated\t1\nruntime-distill\tfailed\n") !=
        "TiP 已更新：记住了 0 个选词、0 个英文词和 0 种按键纠正。"
        "即时学习同步暂未完成，下次更新会自动重试。") {
        std::cerr << "optional runtime synchronization failure should not hide a successful TiP update\n";
        return false;
    }
    if (personalTrainingSuccessMessage(
            activePersonalModel,
            "samples\t65\nranking-samples\t28\nraw-profile-auxiliary-positive\t14\n"
            "active-raw-token-evidence\t5\nactive-correction-patterns\t6\nactive-key-habits\t2\n"
            "validation-accuracy\t12/13\n"
            "validation-baseline-accuracy\t12/13\nvalidation-gain\t1\nnon-leading-samples\t8\n"
            "validation-generic-non-leading-accuracy\t1/5\ngeneric-ranking-safe\t0\n"
            "runtime-raw-token-candidates\t5\nruntime-raw-token-updated\t3\n"
            "runtime-correction-pattern-active\t6\nruntime-correction-pattern-updated\t2\n"
            "runtime-key-habit-active\t2\nruntime-key-habit-updated\t1\n"
            "recommendation\tready\nmodel-updated\t1\n") !=
        "TiP 训练完成：65 条监督（28 条排序，14 条英文模式辅助），6 个已生效纠错模式，"
        "2 个全局按键习惯，5 个已生效英文词记忆；分层留出验证 "
        "12/13（首项基线 "
        "12/13），收益 1；非首选训练样本 8；真实泛化验证 1/5；真实泛化改序仍关闭"
        "；普通输入新增 3 个英文词记忆；普通输入已更新纠错模式 2、按键习惯 1；模型文件已更新"
        "；当前 TiP 已更新，下次按需分析立即生效") {
        std::cerr << "active personal training status should report immediate model refresh\n";
        return false;
    }
    if (personalTrainingSuccessMessage(
            activePersonalModel,
            "samples\t65\nvalidation-accuracy\t12/13\nvalidation-baseline-accuracy\t12/13\n"
            "recommendation\tcollect-more-data\nmodel-updated\t0\n") !=
        "TiP 训练完成：65 条监督；分层留出验证 12/13（首项基线 12/13）；新模型未替换，已保留当前模型"
        "；当前仍在使用 TiP，但样本仍然较少") {
        std::cerr << "preserved personal training status should not imply the candidate model was activated\n";
        return false;
    }
    if (personalTrainingSuccessMessage(
            activePersonalModel,
            "samples\t65\nvalidation-accuracy\t13/13\nvalidation-baseline-accuracy\t13/13\n"
            "recommendation\tcollect-more-data\nmodel-updated\t1\n"
            "model-update-kind\tsafe-component-upgrade\nmerge-strategy\tmax-count-monotonic-v1\n"
            "retained-active-pair-evidence\t14\nretained-active-raw-token-evidence\t7\n"
            "retained-active-correction-patterns\t9\n"
            "retained-active-key-habits\t4\n") !=
        "TiP 训练完成：65 条监督；分层留出验证 13/13（首项基线 13/13）"
        "；安全纠错组件已升级；旧模型已生效证据无回退（选词 14，英文词 7，纠错 9，按键习惯 4）"
        "；通用改序仍关闭，下次按需分析使用新纠错组件") {
        std::cerr << "safe component upgrade status should distinguish correction updates from generic ranking\n";
        return false;
    }
    if (personalTrainingSuccessMessage(
            activePersonalModel,
            "samples\t65\nvalidation-accuracy\t12/13\nvalidation-baseline-accuracy\t12/13\n"
            "recommendation\tcollect-more-data\nmodel-updated\t1\n"
            "model-update-kind\tsafe-validation-upgrade\n") !=
        "TiP 训练完成：65 条监督；分层留出验证 12/13（首项基线 12/13）"
        "；训练评估协议已升级；真实泛化仍关闭，后续训练使用无泄漏评估") {
        std::cerr << "validation upgrade status should explain the safe protocol migration\n";
        return false;
    }
    ModelSettings openaiSettings;
    openaiSettings.mode = "openai";
    openaiSettings.model = "gpt-test";
    openaiSettings.baseUrl = "https://api.openai.com/v1";
    openaiSettings.chatPath = "/v1/responses-compatible";
    openaiSettings.apiKeyEnv = "OPENAI_API_KEY";
    openaiSettings.customCommand = "/must/not/be/passed";
    openaiSettings.timeout = "7";
    openaiSettings.httpTimeout = "11";
    openaiSettings.temperature = "0.25";
    openaiSettings.maxTokens = "256";
    openaiSettings.continuous = true;
    openaiSettings.cloudRecentInput = true;
    const std::vector<std::string> expectedOpenaiArgs = {
        "--write", "openai", "--model", "gpt-test", "--api-key-env", "OPENAI_API_KEY", "--timeout", "7",
        "--http-timeout", "11", "--temperature", "0.25", "--max-tokens", "256", "--send-recent-input", "on",
        "--send-surrounding", "off", "--continuous", "on", "--training-context", "off",
        "--training-surrounding", "off"};
    if (modelConfigArguments(openaiSettings) != expectedOpenaiArgs) {
        std::cerr << "OpenAI model settings should generate safe model-config arguments\n";
        return false;
    }
    openaiSettings.cloudSurrounding = true;
    if (cloudDataSummary(openaiSettings) !=
        "当前拼音和候选、近期按键和修改记录、光标附近文字和应用名称") {
        std::cerr << "cloud data sharing summary should name every enabled category\n";
        return false;
    }
    ModelSettings storedKeySettings = openaiSettings;
    storedKeySettings.apiKeyEnv.clear();
    storedKeySettings.apiKey = "must-not-appear-in-argv";
    const auto storedKeyArgs = modelConfigArguments(storedKeySettings);
    if (std::find(storedKeyArgs.begin(), storedKeyArgs.end(), "--api-key-stdin") == storedKeyArgs.end() ||
        std::find(storedKeyArgs.begin(), storedKeyArgs.end(), storedKeySettings.apiKey) != storedKeyArgs.end()) {
        std::cerr << "direct API keys should be sent over stdin and never appear in model-config arguments\n";
        return false;
    }
    storedKeySettings.apiKey.clear();
    storedKeySettings.clearApiKey = true;
    const auto clearStoredKeyArgs = modelConfigArguments(storedKeySettings);
    if (std::find(clearStoredKeyArgs.begin(), clearStoredKeyArgs.end(), "--clear-api-key") ==
        clearStoredKeyArgs.end()) {
        std::cerr << "stored API keys should have an explicit removal action\n";
        return false;
    }
    ModelSettings customSettings;
    customSettings.mode = "custom";
    customSettings.customCommand = "/tmp/local-wrapper --profile small";
    customSettings.timeout = "4";
    const std::vector<std::string> expectedCustomArgs = {
        "--write", "custom", "--command", "/tmp/local-wrapper --profile small", "--timeout", "4",
        "--continuous", "off", "--training-context", "off", "--training-surrounding", "off"};
    if (modelConfigArguments(customSettings) != expectedCustomArgs) {
        std::cerr << "custom model settings should preserve one command argument without a shell\n";
        return false;
    }
    ModelSettings personalSettings;
    personalSettings.mode = "personal";
    personalSettings.personalModelPath = "/tmp/tipe-personal.json";
    personalSettings.timeout = "5";
    personalSettings.trainingContext = true;
    personalSettings.trainingSurrounding = true;
    const std::vector<std::string> expectedPersonalArgs = {
        "--write", "personal", "--personal-model", "/tmp/tipe-personal.json", "--timeout", "5",
        "--continuous", "off", "--training-context", "on", "--training-surrounding", "on"};
    if (modelConfigArguments(personalSettings) != expectedPersonalArgs) {
        std::cerr << "personal model settings should preserve the local model path\n";
        return false;
    }
    ModelSettings llamaSettings;
    llamaSettings.mode = "llama-cpp";
    llamaSettings.model = "/tmp/qwen.gguf";
    llamaSettings.llamaCommand = "/usr/bin/llama-cli";
    llamaSettings.llamaThreads = "4";
    llamaSettings.llamaContext = "4096";
    llamaSettings.timeout = "30";
    const std::vector<std::string> expectedLlamaArgs = {
        "--write", "llama-cpp", "--model", "/tmp/qwen.gguf", "--llama-command", "/usr/bin/llama-cli",
        "--llama-threads", "4", "--llama-context", "4096", "--timeout", "30", "--continuous", "off",
        "--training-context", "off", "--training-surrounding", "off"};
    if (modelConfigArguments(llamaSettings) != expectedLlamaArgs) {
        std::cerr << "llama.cpp model settings should preserve the GGUF path\n";
        return false;
    }
    ModelSettings dumpSettings;
    dumpSettings.mode = "dump";
    dumpSettings.dumpPath = "/tmp/tipe request.tsv";
    dumpSettings.timeout = "2";
    const std::vector<std::string> expectedDumpArgs = {
        "--write", "dump", "--dump-path", "/tmp/tipe request.tsv", "--timeout", "2", "--continuous",
        "off", "--training-context", "off", "--training-surrounding", "off"};
    if (selectedModeIndex("dump") == 0 || modelConfigArguments(dumpSettings) != expectedDumpArgs) {
        std::cerr << "request-dump settings should round-trip without switching to heuristic mode\n";
        return false;
    }
    LearningPanelData privateTraining;
    privateTraining.personalTrainCommand = "/tmp/tipe-personal-train";
    if (personalTrainingArguments(privateTraining) != std::vector<std::string>{"/tmp/tipe-personal-train"}) {
        std::cerr << "personal training should exclude context fingerprints by default\n";
        return false;
    }
    privateTraining.rows.push_back({"model-config", "training-context", {"1"}});
    privateTraining.rows.push_back({"model-config", "training-surrounding", {"1"}});
    privateTraining.rows.push_back({"model-config", "personal-model", {"/tmp/TiP model.json"}});
    const std::vector<std::string> expectedTrainingArgs = {
        "/tmp/tipe-personal-train", "--output", "/tmp/TiP model.json", "--include-context",
        "--include-surrounding"};
    if (personalTrainingArguments(privateTraining) != expectedTrainingArgs) {
        std::cerr << "personal training should pass only explicitly enabled context flags\n";
        return false;
    }
    LearningPanelData shownConfig;
    shownConfig.rows.push_back({"model-config", "configured-mode", {"heuristic"}});
    if (!applyModelConfigShowOutput(
            shownConfig,
            "config\t/tmp/model-env\nmodel-status\tconfigured-mode\tollama\n"
            "model-status\tkind\tlocal-http:ollama\nmodel-status\tmodel\tqwen-test\n") ||
        shownConfig.rows.size() != 3 || rowValue(shownConfig, "model-config", "configured-mode") != "ollama" ||
        rowValue(shownConfig, "model-config", "kind") != "local-http:ollama" ||
        rowValue(shownConfig, "model-config", "model") != "qwen-test") {
        std::cerr << "saved model config status should replace stale panel rows\n";
        return false;
    }
    std::istringstream input(
        "panel\tstate\tpreedit\tnihao\n"
        "panel\tstate\trequest-source\tlive-supervision\t/tmp/supervision-current.tsv\n"
        "panel\tstate\trequest-mtime\t1700000000\n"
        "panel\tcandidates\ttotal\t2\tvisible\t2\tnumbered\t1\n"
        "panel\tcandidates\tfirst\t0\t你好\n"
        "panel\tcandidates\tselected\t1\t你号\n"
        "panel\tcandidates\tvisible\t1\t0\t你好\n"
        "panel\tcandidates\tnumbered\t1\t1\t0\t你好\n"
        "panel\tsupervision\trecent-events\t2\tcorrection-events\t13\tcontext\t1\tsegment-chains\t1\tpending-segments\t1\n"
        "panel\tsupervision\tmodel-input\tpreedit\tnihao\tcandidates\t2\tvisible\t2\tnumbered\t1\tcontext\t1\tsegment-chains\t1\tpending-segments\t1\n"
        "panel\tsupervision\tmode\tactive-preedit\n"
        "panel\tsupervision\tevent-trail\trecent\t2\tlimit\t64\tpurpose\tui-and-short-action-order\n"
        "panel\tsupervision\tcorrection-trail\trecent\t13\tlimit\t256\tpurpose\tdelete-retype-and-middle-edit-learning\n"
        "panel\tsupervision\tevent-count\tletter\t1\n"
        "panel\tsupervision\tevent-count\tcursor-move\t1\n"
        "panel\tsupervision\tcorrection-event-count\tletter\t9\n"
        "panel\tsupervision\tcorrection-event-count\tbackspace\t4\n"
        "panel\tsupervision\tevent-item\t1\tletter\tn\n"
        "panel\tsupervision\tcorrection-event-item\t1\tbackspace\t\n"
        "panel\tsegment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好\n"
        "panel\tlearning\tpreferences\t1\ttotal\t8\tcorrections\t1\ttotal\t2\n"
        "panel\tlearning\tpending-segment-signal\t1\twoc\two\t我\tc\tawaiting-suffix-confirmation\n"
        "panel\tlearning\tcorrection-signal\t1\tfull-delete-retype\tihao\tnihao\n"
        "panel\tmodel-config\tkind\toffline-heuristic\n"
        "panel\tmodel-config\tconfigured-command\t/home/user/.local/bin/tipe-model-current\n"
        "panel\tmodel-config\tconfigured-command-valid\t1\n"
        "panel\tmodel-config\tprocess-command\tunset\n"
        "panel\tmodel-config\tprocess-command-scope\tcurrent-shell-environment\n"
        "panel\tmodel-config\tprocess-command-active-scope\tcurrent-shell-only-not-fcitx5-runtime\n"
        "panel\tmodel-config\truntime-verification\ttipe-doctor\n"
        "panel\tmodel-config\tprocess-command-active\t0\n"
        "panel\tmodel-config\tactivation-hint\ttipe-restart-fcitx5 --model-current\n"
        "panel\tmodel-config\tself-test-command\t/home/user/.local/bin/tipe-model-self-test --current --config /home/user/.config/tipe/model-env\n"
        "panel\tmodel-config\tdry-run-test-command\t/home/user/.local/bin/tipe-model-self-test --current --config /home/user/.config/tipe/model-env --adapter-dry-run\n"
        "panel\tmodel-config\tdry-run-test-supported\t0\n"
        "panel\ttop-preference\t1\t8\tnihao\t你好\n"
        "panel\ttop-correction\t1\t2\tihao\tnihao\n"
        "panel\ttop-correction-pattern\t1\t5\tmissing\ti\t1\n"
        "panel\tbehavior\traw-english-hint\t0\tsource\tnone\n"
        "panel\tbehavior\tedit-summary\tcurrent\tnihao\tcursor\t5\ttyped-tail\tnihao\tlast-erased\tihao\tlast-edited\tihao\tmiddle-edit\t\n"
        "panel\tbehavior\tpossible-correction\t1\tfull-delete-retype\tihao\tnihao\n"
        "panel\tbehavior\tcorrection-pattern\t1\tmissing\ti\t1\t5\n"
        "panel\tbehavior\trealtime-correction\t1\tskipped\talready-present\tmissing\ti\t1\t5\t\n"
        "panel\tmodel-replay\tstatus\tok\n"
        "panel\tmodel-replay\twrapper\t/tmp/model.sh\trows\t2\n"
        "panel\tmodel-output\taccepted-candidate\t1\t你号\n"
        "panel\tmodel-output\taccepted-correction\t1\tihao\tnihao\n"
        "panel\tmodel-output\taccepted-preference\t1\tnihao\t你号\n"
        "panel\tmodel-output\taccepted-segment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好\n"
        "panel\tmodel-output\tsummary\trows\t4\tcandidates\t1\tcorrections\t1\tpreferences\t1\tsegment-chains\t1\n"
        "panel\tmodel-output\tlearned\tpreferences\t1\tcorrections\t1\tsegment-chains\t1\tpath\t/tmp/prefs.tsv\n"
        "panel\tmodel-output\tlearned-top-preference\t1\tnihao\t你号\t2\n"
        "panel\tmodel-output\tlearned-top-correction\t1\tihao\tnihao\t2\n"
        "panel\tmodel-output\tlearned-top-correction-pattern\t1\t2\tmissing\tn\t0\n"
        "panel\tmodel-output\tlearned-top-segment-chain\t1\tnihao\tni\t你\thao\tnihao\t你好\t5\n"
        "panel\tmodel-output\tpreferences-summary\trows\t4\n"
        "panel\tmodel-output\tpreferences-summary\tpreferences\t1\t2\n"
        "panel\tmodel-output\tpreferences-summary\tlegacy-preferences\t0\t0\n"
        "panel\tmodel-output\tpreferences-summary\tcorrections\t1\t2\n"
        "panel\tmodel-output\tpreferences-summary\tcorrection-patterns\t1\t2\n"
        "panel\tmodel-output\tpreferences-summary\tsegment-chains\t1\t5\n"
        "panel\tmodel-output\tpreferences-top-preference\t1\t2\tnihao\t你号\n"
        "panel\tmodel-output\tpreferences-top-correction\t1\t2\tihao\tnihao\n"
        "panel\tmodel-output\tpreferences-top-correction-pattern\t1\t2\tmissing\tn\t0\n"
        "panel\tmodel-output\tpreferences-top-segment-chain\t1\t5\tnihao\t你好\n"
        "panel\tmodel-output\tnote\tselected-candidate-learned\tnihao\t1\t你号\n"
        "panel\tmodel-output\trow-candidate\t1\t你号\n"
        "panel\tmodel-output\trow-correction\t2\tihao\tnihao\n");
    if (!loadPanelStream(data, input) || data.rows.size() != 68) {
        std::cerr << "panel rows should parse\n";
        return false;
    }
    if (data.rows[0].section != "state" || data.rows[0].key != "preedit" || data.rows[0].values[0] != "nihao") {
        std::cerr << "state preedit row should parse\n";
        return false;
    }
    const auto summaryLines = summaryLinesFor(data);
    const std::vector<std::string> expectedSummary{
        "快照来源：当前实时输入（/tmp/supervision-current.tsv）",
        "快照时间：2023-11-15 06:13:20",
        "正在输入：nihao",
        "候选：2 个，总计；当前显示 2 个；带数字 1 个",
        "首选候选：你好",
        "当前选中：你号",
        "分段链等待确认：woc 已先选 我，当前剩余 c",
        "纠错可学习：ihao -> nihao（全部删除后重打）",
        "已监督：2 个近期按键，13 个纠错记忆事件，1 个上下文提交",
        "监督模式：活动拼音和候选",
        "按键类型：字母 1，光标移动 1",
        "纠错轨迹类型：字母 9，退格 4",
        "分析后端：离线启发式",
        "当前窗口进程未携带模型入口环境；fcitx5 运行状态以 tipe-doctor runtime 为准",
        "模型可自测：/home/user/.local/bin/tipe-model-self-test --current --config /home/user/.config/tipe/model-env",
        "可能的输错模式：ihao -> nihao（全部删除后重打）",
        "实时纠错规则：跳过 漏字 i（目标已存在）",
        "最强已学习选择：nihao -> 你好（次数 8）",
        "本次分析接受：候选 1，纠错 1，偏好 1，分段链 1",
        "写入学习文件：/tmp/prefs.tsv",
        "下次排序会优先：nihao -> 你号",
        "下次分段会借用：nihao 先选 你，剩余 hao 会优先 好",
        "模型学到选择：nihao -> 你号（次数 2）",
        "模型学到纠错：ihao -> nihao（次数 2）",
        "模型学到习惯：漏字 n（位置 0，次数 2）",
        "模型学到分段短语：nihao 先选 你，剩余 hao 优先 好（次数 5）",
        "学习文件：偏好 2，纠错 2，分段链 5",
        "分析已学习本次选择：nihao -> 你号",
        "模型：分析完成",
    };
    if (summaryLines != expectedSummary) {
        std::cerr << "summary rows should explain current learning state\n";
        return false;
    }
    if (analysisStatusText(data) !=
        "分析完成：输出 4，候选 1，纠错 1，偏好 1，分段链 1；已写入学习：偏好 1，纠错 1，分段链 1") {
        std::cerr << "analysis status should summarize accepted and persisted learning output\n";
        return false;
    }
    LearningPanelData waitingData;
    std::istringstream waitingInput(
        "panel\tstate\tpreedit\t\n"
        "panel\tstate\tstatus\twaiting-for-live-supervision\n"
        "panel\tcandidates\ttotal\t0\tvisible\t0\tnumbered\t0\n");
    if (!loadPanelStream(waitingData, waitingInput)) {
        std::cerr << "waiting panel rows should parse\n";
        return false;
    }
    const auto waitingSummary = summaryLinesFor(waitingData);
    if (waitingSummary.size() < 3 || waitingSummary[0] != "等待当前 TiPE 输入" ||
        waitingSummary[1] != "开始输入拼音后，这个窗口会自动刷新。" ||
        waitingSummary[2] != "正在输入：无活动拼音") {
        std::cerr << "waiting summary should explain how to activate live supervision\n";
        return false;
    }
    LearningPanelData passThroughData;
    std::istringstream passThroughInput(
        "panel\tstate\tpreedit\t\n"
        "panel\tsupervision\trecent-events\t4\tcorrection-events\t4\tcontext\t0\tsegment-chains\t0\tpending-segments\t0\n"
        "panel\tsupervision\tmode\tpass-through-only\n"
        "panel\tsupervision\tevent-count\tspace\t1\n"
        "panel\tsupervision\tevent-count\tdelete\t1\n");
    if (!loadPanelStream(passThroughData, passThroughInput)) {
        std::cerr << "pass-through panel rows should parse\n";
        return false;
    }
    const auto passThroughSummary = summaryLinesFor(passThroughData);
    if (std::find(passThroughSummary.begin(), passThroughSummary.end(),
                  "监督模式：仅按键行为；正在观察按键习惯，开始拼音后才分析候选") == passThroughSummary.end() ||
        displayTextFor({"supervision", "mode", {"pass-through-only"}}) != "监督模式：仅按键习惯") {
        std::cerr << "pass-through supervision mode should be explicit in the window\n";
        return false;
    }
    LearningPanelData learningStatusData;
    std::istringstream learningStatusInput(
        "panel\tstate\tpreedit\tnihao\n"
        "panel\tlearning\tstatus\tready-to-learn\tnext-step\tprefer-suggested-protocols\n"
        "panel\tlearning\tstatus-suggested-protocol\t1\tpreference\tnihao\t你号\t2\n"
        "panel\tlearning\tstatus-signal-count\tselected_candidate\t1\n");
    if (!loadPanelStream(learningStatusData, learningStatusInput)) {
        std::cerr << "learning status panel rows should parse\n";
        return false;
    }
    const auto learningStatusSummary = summaryLinesFor(learningStatusData);
    if (std::find(learningStatusSummary.begin(), learningStatusSummary.end(),
                  "学习状态：可学习，下一步：优先使用建议学习行") == learningStatusSummary.end() ||
        std::find(learningStatusSummary.begin(), learningStatusSummary.end(),
                  "建议学习行：preference\tnihao\t你号\t2") == learningStatusSummary.end() ||
        displayTextFor({"learning", "status", {"awaiting-suffix-confirmation", "next-step", "wait-for-selected-suffix"}}) !=
            "学习状态：等待后缀确认 / 下一步 等待选中后缀" ||
        displayTextFor({"learning", "status-awaiting-suffix", {"1", "woc", "wo", "我", "c"}}) !=
            "等待后缀 #1：woc 先选 我，剩余 c" ||
        displayTextFor({"learning", "status-signal-count", {"selected_candidate", "1"}}) !=
            "学习信号：候选选择 1" ||
        displayTextFor({"learning", "status-signal-count", {"selected_candidate_prefix", "1"}}) !=
            "学习信号：前缀候选选择 1" ||
        displayTextFor({"history", "learnable-preference",
                        {"1", "nihao", "你号", "2", "preference", "nihao", "你号", "2"}}) !=
            "可由历史学习排序 #1：nihao -> 你号（2 次，建议 preference\tnihao\t你号\t2）") {
        std::cerr << "learning status rows should format for humans\n";
        return false;
    }
    LearningPanelData lastData;
    std::istringstream lastInput(
        "panel\tstate\tpreedit\tnihao\n"
        "panel\tstate\tstatus\tshowing-last-supervision\n"
        "panel\tcandidates\ttotal\t2\tvisible\t2\tnumbered\t1\n");
    if (!loadPanelStream(lastData, lastInput)) {
        std::cerr << "last snapshot panel rows should parse\n";
        return false;
    }
    const auto lastSummary = summaryLinesFor(lastData);
    if (lastSummary.size() < 3 || lastSummary[0] != "正在显示上一次 TiPE 输入" ||
        lastSummary[1] != "开始输入拼音后，会切换到当前实时输入。" ||
        lastSummary[2] != "正在输入：nihao") {
        std::cerr << "last snapshot summary should distinguish stale supervision from live input\n";
        return false;
    }
    LearningPanelData emptyModelData;
    std::istringstream emptyModelInput(
        "panel\tstate\tpreedit\tnijixunong\n"
        "panel\tmodel-replay\tstatus\tok\n"
        "panel\tmodel-output\tsummary\trows\t0\tcandidates\t0\tcorrections\t0\tpreferences\t0\tsegment-chains\t0\n"
        "panel\tmodel-output\tnote\tno-safe-learning-signal\tnijixunong\n");
    if (!loadPanelStream(emptyModelData, emptyModelInput)) {
        std::cerr << "empty model panel rows should parse\n";
        return false;
    }
    const auto emptyModelSummary = summaryLinesFor(emptyModelData);
    if (emptyModelSummary.empty() ||
        emptyModelSummary.back() != "模型：分析完成，没有建议改变排序或学习内容") {
        std::cerr << "empty model output should be explained as no suggested change\n";
        return false;
    }
    if (analysisStatusText(emptyModelData) != "分析完成：输出 0，候选 0，纠错 0，偏好 0，分段链 0") {
        std::cerr << "empty model status should still summarize zero accepted rows\n";
        return false;
    }
    LearningPanelData manualAnalyzeData;
    std::istringstream manualAnalyzeInput(
        "panel\tstate\tpreedit\tnihao\n"
        "panel\tmodel-replay\tmode\tmanual\tlearn-output\t0\ttrigger\tAnalyze\n");
    if (!loadPanelStream(manualAnalyzeData, manualAnalyzeInput) || analyzeLearns(manualAnalyzeData) ||
        analyzeButtonText(manualAnalyzeData) != "预览当前输入" ||
        refreshHintText(manualAnalyzeData) != "" ||
        summaryLinesFor(manualAnalyzeData).back() != "模型：尚未运行；“预览当前输入”只运行一次") {
        std::cerr << "manual analyze mode should remain read-only unless learn-output is set\n";
        return false;
    }
    manualAnalyzeData.refreshCommand = "/bin/true";
    if (refreshHintText(manualAnalyzeData) != "；“预览当前输入”只运行一次模型") {
        std::cerr << "manual analyze hint should explain read-only model run\n";
        return false;
    }
    LearningPanelData learnAnalyzeData;
    std::istringstream learnAnalyzeInput(
        "panel\tstate\tpreedit\tnihao\n"
        "panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze\n");
    if (!loadPanelStream(learnAnalyzeData, learnAnalyzeInput) || !analyzeLearns(learnAnalyzeData) ||
        analyzeButtonText(learnAnalyzeData) != "分析当前输入" ||
        summaryLinesFor(learnAnalyzeData).back() !=
            "模型：尚未运行；“分析当前输入”会运行一次，安全结果写入学习") {
        std::cerr << "learn-output analyze mode should make learning explicit\n";
        return false;
    }
    learnAnalyzeData.refreshCommand = "/bin/true";
    if (refreshHintText(learnAnalyzeData) !=
        "；“分析当前输入”会运行一次模型，安全结果写入学习") {
        std::cerr << "learn-output analyze hint should explain persisted learning\n";
        return false;
    }
    LearningPanelData personalAnalyzeData;
    personalAnalyzeData.personalTrainCommand = "/bin/true";
    std::istringstream personalAnalyzeInput(
        "panel\tstate\tpreedit\tnihao\n"
        "panel\tmodel-replay\tmode\tmanual\tlearn-output\t1\ttrigger\tAnalyze\n"
        "panel\tmodel-config\tconfigured-mode\tpersonal\n");
    if (!loadPanelStream(personalAnalyzeData, personalAnalyzeInput) ||
        analyzeButtonText(personalAnalyzeData) != "分析当前输入" ||
        summaryLinesFor(personalAnalyzeData).back() !=
            "模型：尚未运行；“分析当前输入”会运行一次，安全结果写入学习") {
        std::cerr << "personal analysis should remain separate from model training\n";
        return false;
    }
    personalAnalyzeData.refreshCommand = "/bin/true";
    if (refreshHintText(personalAnalyzeData) !=
        "；“分析当前输入”会运行一次模型，安全结果写入学习") {
        std::cerr << "personal mode should use the same one-shot analysis hint\n";
        return false;
    }
    if (displayTextFor(data.rows[0]) != "正在输入：nihao" ||
        displayTextFor(data.rows[1]) != "快照来源：当前实时输入 / /tmp/supervision-current.tsv" ||
        displayTextFor(data.rows[2]) != "快照时间：2023-11-15 06:13:20") {
        std::cerr << "request source rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"state", "status", {"showing-last-supervision"}}) != "状态：上一次输入快照") {
        std::cerr << "state status row should be localized for humans\n";
        return false;
    }
    if (displayTextFor({"state", "surrounding-before", {"前文"}}) != "光标前文：前文") {
        std::cerr << "state surrounding-before row should be localized for humans\n";
        return false;
    }
    if (displayTextFor({"state", "surrounding-after", {""}}) != "光标后文：无") {
        std::cerr << "state surrounding-after row should be localized for humans\n";
        return false;
    }
    if (modelKindText("personal-reranker") != "TiP" ||
        modelKindText("local-llama-cpp") != "本地 llama.cpp" ||
        modelKindText("official-openai:openai-compatible") != "官方 OpenAI：openai-compatible" ||
        modelKindText("openai-compatible:openai-compatible") != "OpenAI 兼容：openai-compatible") {
        std::cerr << "model kind labels should distinguish official OpenAI from compatible providers\n";
        return false;
    }
    if (displayTextFor({"model-config", "invocation", {"on-demand-single-process"}}) !=
            "调用方式：按需启动一次，完成后退出" ||
        displayTextFor({"model-config", "llama-threads", {"6"}}) != "本地模型线程数：6" ||
        displayTextFor({"model-config", "llama-context", {"8192"}}) != "本地模型上下文：8192" ||
        displayTextFor({"model-config", "timeout", {"30"}}) != "分析超时（秒）：30") {
        std::cerr << "llama.cpp status rows should use Chinese-first labels\n";
        return false;
    }
    if (displayTextFor({"model-config", "personal-model-architecture",
                        {"hashed-pairwise-ranker+personal-edit-channel+raw-token-memory+"
                         "raw-offer-profile+pinyin-prior"}}) !=
            "TiP 结构：候选成对排序 + 个人按键错误通道 + 英文词记忆 + 英文原样分类 + 拼音先验" ||
        displayTextFor({"model-config", "personal-model-name", {"TiP"}}) != "模型名称：TiP" ||
        displayTextFor({"model-config", "personal-model-active-raw-token-evidence", {"7"}}) !=
            "生效英文词记忆：7" ||
        displayTextFor({"model-config", "personal-model-feature-version", {"3"}}) !=
            "TiP 特征版本：3" ||
        displayTextFor({"model-config", "personal-model-training-ranking-samples", {"75"}}) !=
            "TiP 排序样本：75" ||
        displayTextFor({"model-config", "personal-model-training-correction-only-samples", {"167"}}) !=
            "纠错专用样本：167" ||
        displayTextFor({"model-config", "personal-model-training-raw-profile-auxiliary-positive-samples",
                        {"134"}}) != "英文模式辅助正样本：134" ||
        displayTextFor({"model-config", "personal-model-pinyin-prior-entries", {"38999"}}) !=
            "拼音先验条目：38999") {
        std::cerr << "personal model architecture rows should use Chinese-first labels\n";
        return false;
    }
    const auto recommendationText =
        displayTextFor({"model-config", "personal-model-training-recommendation", {"keep-heuristic"}});
    const auto personalStatusText = displayTextFor({"model-config", "personal-model-status", {"invalid"}});
    const auto validationAccuracyText =
        displayTextFor({"model-config", "personal-model-training-validation-accuracy", {"0.923077"}});
    const auto validationStrategyText = displayTextFor(
        {"model-config", "personal-model-training-validation-strategy", {"capability-isolated-temporal-v4"}});
    const auto genericValidationText = displayTextFor(
        {"model-config", "personal-model-training-validation-generic-non-leading-accuracy", {"0.4"}});
    const auto derivedPrefixExcludedText = displayTextFor(
        {"model-config", "personal-model-training-validation-generic-excluded-derived-prefix", {"2"}});
    const auto validationGainText =
        displayTextFor({"model-config", "personal-model-training-validation-gain", {"1"}});
    const auto nonLeadingSamplesText =
        displayTextFor({"model-config", "personal-model-training-non-leading-samples", {"7"}});
    if (recommendationText != "TiP 建议：继续使用离线启发式" ||
        personalStatusText != "TiP 状态：模型文件无效" ||
        validationStrategyText != "验证集策略：按能力分层验证，并隔离派生前缀、英文原样与已有证据" ||
        genericValidationText != "真实泛化验证准确率：0.4" ||
        derivedPrefixExcludedText != "排除派生前缀样本：2" ||
        validationAccuracyText != "留出集准确率：0.923077" ||
        validationGainText != "验证收益：1" ||
        nonLeadingSamplesText != "非首选训练样本：7") {
        std::cerr << "personal model quality rows should use Chinese-first labels\n";
        std::cerr << recommendationText << "\n" << personalStatusText << "\n" << validationStrategyText << "\n"
                  << genericValidationText << "\n" << derivedPrefixExcludedText << "\n"
                  << validationAccuracyText << "\n" << validationGainText << "\n" << nonLeadingSamplesText << "\n";
        return false;
    }
    if (displayTextFor(data.rows[3]) != "总数 2 / 当前显示 2 / 带数字 1") {
        std::cerr << "candidate summary should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[10]) != "监督模式：活动拼音" ||
        displayTextFor(data.rows[13]) != "监督统计：字母 1" ||
        displayTextFor(data.rows[15]) != "纠错统计：字母 9") {
        std::cerr << "declared supervision event counts should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[4]) != "首选 #0：你好" ||
        displayTextFor(data.rows[5]) != "当前选中 #1：你号" ||
        displayTextFor(data.rows[6]) != "显示位 #1 / 候选 #0：你好" ||
        displayTextFor(data.rows[7]) != "数字 1 / 候选 #0：你好") {
        std::cerr << "candidate detail rows should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[8]) !=
        "近期事件 2 / 纠错事件 13 / 上下文 1 / 分段链 1 / 待确认分段 1") {
        std::cerr << "supervision summary should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[9]) !=
        "模型看到：拼音 nihao / 候选 2 / 显示 2 / 带数字 1 / 上下文 1 / 分段链 1 / 待确认分段 1" ||
        displayTextFor(data.rows[11]) != "按键轨迹 2 个事件 / 上限 64 / 界面和短操作顺序" ||
        displayTextFor(data.rows[12]) !=
            "纠错轨迹 13 个事件 / 上限 256 / 删除重打和中间编辑学习") {
        std::cerr << "supervision detail rows should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[17]) != "近期按键 #1：字母 n" ||
        displayTextFor(data.rows[18]) != "纠错轨迹 #1：退格") {
        std::cerr << "supervision event item rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"supervision", "event-item", {"2", "cursor-move", "Down"}}) !=
            "近期按键 #2：光标移动 下方向键" ||
        displayTextFor({"behavior", "recent-event", {"delete", "Delete"}}) != "近期事件 删除：删除") {
        std::cerr << "supervision key values should be localized for humans\n";
        return false;
    }
    if (displayTextFor({"behavior", "preedit-leading-context", {"active", "1", "events", "2"}}) !=
            "拼音前按键：2 个" ||
        displayTextFor({"behavior", "preedit-leading-context", {"active", "0", "events", "0"}}) !=
            "拼音前按键：无" ||
        displayTextFor({"behavior", "preedit-leading-event", {"1", "observed", "WindowSwitch"}}) !=
            "拼音前按键 #1：观察 切换窗口" ||
        displayTextFor({"behavior", "preedit-leading-event", {"2", "cursor-move", "Left"}}) !=
            "拼音前按键 #2：光标移动 左方向键") {
        std::cerr << "preedit-leading behavior rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"model-replay", "mode", {"manual", "learn-output", "0", "trigger", "Analyze"}}) !=
        "模式 手动 / 写入学习 否 / 触发 分析") {
        std::cerr << "model replay mode rows should be localized for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[19]) != "#1 nihao -> 你 / 剩余 hao / 合并 你好") {
        std::cerr << "segment chain rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"learning",
                        "segment-chain-signal",
                        {"1", "woc", "wo", "我", "c", "wocao", "我操", "suffix-correction-chain"}}) !=
        "分段链证据 #1：当前剩余 c 可接在 我 后，合并 我操；纠错目标 wocao") {
        std::cerr << "suffix segment chain learning signals should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[20]) != "选择偏好 1（总计 8）/ 纠错 1（总计 2）") {
        std::cerr << "learning summary should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[21]) != "待确认分段 #1：woc 已先选 我（wo），当前剩余 c") {
        std::cerr << "pending segment learning signal should format for humans\n";
        return false;
    }
    if (displayTextFor({"learning",
                        "pending-segment-signal",
                        {"1", "woc", "wo", "我", "c", "confirmed-suffix", "操", "woc", "我操"}}) !=
        "可学习分段 #1：woc 已先选 我（wo），当前后缀 c 选 操，合并 我操") {
        std::cerr << "confirmed pending segment learning signal should format for humans\n";
        return false;
    }
    LearningPanelData confirmedPendingData;
    std::istringstream confirmedPendingInput(
        "panel\tstate\tpreedit\tc\n"
        "panel\tlearning\tpending-segment-signal\t1\twoc\two\t我\tc\tconfirmed-suffix\t操\twoc\t我操\n");
    if (!loadPanelStream(confirmedPendingData, confirmedPendingInput)) {
        std::cerr << "confirmed pending segment panel rows should parse\n";
        return false;
    }
    const auto confirmedPendingSummary = summaryLinesFor(confirmedPendingData);
    if (std::find(confirmedPendingSummary.begin(), confirmedPendingSummary.end(),
                  "分段链可学习：woc 已先选 我，当前后缀 c 选 操，合并 我操") ==
        confirmedPendingSummary.end()) {
        std::cerr << "confirmed pending segment summary should explain learned chain\n";
        return false;
    }
    if (displayTextFor(data.rows[22]) != "可学习纠错 #1：ihao -> nihao（全部删除后重打）") {
        std::cerr << "correction learning signal should format for humans\n";
        return false;
    }
    if (displayTextFor({"history", "correction", {"1", "ihao", "nihao", "2"}}) !=
        "近期纠错 #1：ihao -> nihao（2 次）") {
        std::cerr << "history correction rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"history", "active-event-count", {"1", "letter", "5"}}) !=
            "近期组合按键 #1：字母 5" ||
        displayTextFor({"history", "pass-through-event-count", {"1", "space", "3"}}) !=
            "近期透传按键 #1：空格 3") {
        std::cerr << "history split event count rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"learning",
                        "evidence-summary",
                        {"nihao", "preferences", "1", "legacy-preferences", "0", "corrections", "1",
                         "segment-chains", "0"}}) !=
        "当前拼音证据：nihao / 排序 1 / 旧偏好 0 / 纠错 1 / 分段链 0") {
        std::cerr << "current preedit evidence summary should format for humans\n";
        return false;
    }
    if (displayTextFor({"learning", "evidence-preference", {"1", "nihao", "你号", "3"}}) !=
        "已学排序：nihao -> 你号（3 次，行 1）") {
        std::cerr << "current preedit learned preference evidence should format for humans\n";
        return false;
    }
    if (displayTextFor({"learning", "evidence-supervised-raw-token", {"2", "to", "3", "active"}}) !=
            "英文模式已确认：to（3 次，已生效，行 2）" ||
        displayTextFor({"learning", "evidence-effect", {"supervised-raw-token", "2", "to", "3"}}) !=
            "下次英文直出：to 已由英文模式确认（3 次，行 2）") {
        std::cerr << "supervised English token evidence should format for humans\n";
        return false;
    }
    if (displayTextFor({"learning", "evidence-correction", {"3", "corrected", "ihao", "nihao", "2"}}) !=
        "已学纠错：ihao -> nihao（2 次，纠正目标，行 3）") {
        std::cerr << "current preedit learned correction evidence should format for humans\n";
        return false;
    }
    if (displayTextFor({"model-output", "rejected-summary", {"rows", "1", "status", "1"}}) !=
        "模型输出被拒绝：行 1 / 检查状态 1") {
        std::cerr << "rejected model output summary should format for humans\n";
        return false;
    }
    if (displayTextFor({"model-output", "rejected-row", {"1", "segment_chain", "nihao", "n", "你", "hao"}}) !=
        "被拒绝输出 #1 分段链 nihao => 你") {
        std::cerr << "rejected model output rows should format for humans\n";
        return false;
    }
    if (displayTextFor({"model-output", "note", {"no-new-learning", "nihao", "already-known-or-no-new-safe-row"}}) !=
        "说明：没有新增学习 nihao（已存在或没有新的安全学习行）") {
        std::cerr << "no-new-learning model output notes should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[23]) != "类型：离线启发式" ||
        displayTextFor(data.rows[24]) != "配置文件模型命令：/home/user/.local/bin/tipe-model-current" ||
        displayTextFor(data.rows[25]) != "配置文件模型入口有效：是" ||
        displayTextFor(data.rows[27]) != "命令环境范围：当前 shell 环境" ||
        displayTextFor(data.rows[28]) != "模型入口状态范围：仅当前 shell，未进入 fcitx5 运行时" ||
        displayTextFor(data.rows[29]) != "运行时核验：tipe-doctor" ||
        displayTextFor(data.rows[30]) != "当前命令环境使用模型入口：否" ||
        displayTextFor(data.rows[31]) != "启用提示：如需让 fcitx5 加载模型入口，运行 tipe-restart-fcitx5 --model-current" ||
        displayTextFor(data.rows[32]) !=
            "模型自测：/home/user/.local/bin/tipe-model-self-test --current --config /home/user/.config/tipe/model-env" ||
        displayTextFor(data.rows[33]) !=
            "无网络自测：/home/user/.local/bin/tipe-model-self-test --current --config /home/user/.config/tipe/model-env --adapter-dry-run" ||
        displayTextFor(data.rows[34]) != "支持无网络自测：否" ||
        displayTextFor(data.rows[35]) != "#1 次数 8：nihao -> 你好" ||
        displayTextFor(data.rows[36]) != "#1 次数 2：ihao => nihao" ||
        displayTextFor(data.rows[37]) != "#1 次数 5：漏字 i / 位置 1") {
        std::cerr << "top learned rows should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[38]) != "英文直出提示 0（无）" ||
        displayTextFor(data.rows[39]) != "编辑摘要：当前 nihao / 光标 5 / 最近输入 nihao / 刚删空 ihao / 刚重写 ihao" ||
        displayTextFor(data.rows[40]) != "#1 全部删除后重打：ihao -> nihao" ||
        displayTextFor(data.rows[41]) != "纠错模式 #1：漏字 i / 位置 1 / 次数 5" ||
        displayTextFor(data.rows[42]) != "实时纠错 #1：跳过 / 漏字 i / 位置 1 / 次数 5 / 原因 目标已存在") {
        std::cerr << "behavior rows should format for humans\n";
        return false;
    }
    if (displayTextFor(data.rows[43]) != "回放成功" ||
        displayTextFor(data.rows[44]) != "模型命令 /tmp/model.sh / 行 2" ||
        displayTextFor(data.rows[45]) != "已接受候选 #1：你号" ||
        displayTextFor(data.rows[46]) != "已接受纠错 #1：ihao => nihao" ||
        displayTextFor(data.rows[47]) != "已接受偏好 #1：nihao -> 你号" ||
        displayTextFor(data.rows[48]) != "已接受分段链 #1：nihao 先选 你，剩余 hao => 好" ||
        displayTextFor(data.rows[49]) != "汇总 行 4 / 候选 1 / 纠错 1 / 偏好 1 / 分段链 1" ||
        displayTextFor(data.rows[50]) != "已学习 偏好 1 / 纠错 1 / 分段链 1 / 路径 /tmp/prefs.tsv" ||
        displayTextFor(data.rows[51]) != "已学习偏好 #1：nihao -> 你号（次数 2）" ||
        displayTextFor(data.rows[52]) != "已学习纠错 #1：ihao => nihao（次数 2）" ||
        displayTextFor(data.rows[53]) != "已学习习惯 #1：次数 2 / 漏字 n / 位置 0" ||
        displayTextFor(data.rows[54]) != "已学习分段链 #1：nihao 先选 你，剩余 hao => 好（次数 5）" ||
        displayTextFor(data.rows[55]) != "学习文件汇总：行 4" ||
        displayTextFor(data.rows[56]) != "学习文件汇总：偏好 1（总计 2）" ||
        displayTextFor(data.rows[57]) != "学习文件汇总：旧偏好 0（总计 0）" ||
        displayTextFor(data.rows[58]) != "学习文件汇总：纠错 1（总计 2）" ||
        displayTextFor(data.rows[59]) != "学习文件汇总：纠错习惯 1（总计 2）" ||
        displayTextFor(data.rows[60]) != "学习文件汇总：分段链 1（总计 5）" ||
        displayTextFor(data.rows[61]) != "学习文件偏好 #1：次数 2 / nihao -> 你号" ||
        displayTextFor(data.rows[62]) != "学习文件纠错 #1：次数 2 / ihao => nihao" ||
        displayTextFor(data.rows[63]) != "学习文件习惯 #1：次数 2 / 漏字 n / 位置 0" ||
        displayTextFor(data.rows[64]) != "学习文件分段链 #1：次数 5 / nihao => 你好" ||
        displayTextFor(data.rows[65]) != "说明：已学习选中候选 nihao -> 你号" ||
        displayTextFor(data.rows[66]) != "模型输出 #1 候选 你号" ||
        displayTextFor(data.rows[67]) != "模型输出 #2 纠错 ihao => nihao") {
        std::cerr << "model output rows should format for humans\n";
        return false;
    }
    if (parsePanelLine(data, "not-panel\tstate\tbad")) {
        std::cerr << "invalid panel prefix should be rejected\n";
        return false;
    }
    std::istringstream reloadInput("panel\tstate\tpreedit\txin\n");
    if (!loadPanelStream(data, reloadInput) || data.rows.size() != 69) {
        std::cerr << "stream loading should append parsed rows\n";
        return false;
    }
    LearningPanelData fileData;
    fileData.inputPath = "-";
    std::istringstream stdinInput("panel\tstate\tpreedit\tstdin\n");
    if (!loadPanelStream(fileData, stdinInput) || !refreshRows(fileData, false) ||
        fileData.statusText != "已载入学习数据") {
        std::cerr << "refresh should report static stdin snapshots without re-reading stdin\n";
        return false;
    }
    LearningPanelData badRefreshData;
    badRefreshData.inputPath = "-";
    badRefreshData.refreshCommand = "/nonexistent/tipe-learning-panel-refresh-command";
    if (refreshRows(badRefreshData, true) || badRefreshData.statusText.find("分析失败") == std::string::npos) {
        std::cerr << "refresh command failures should be reported without crashing\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    auto data = parseArgs(argc, argv);
    if (data.selfTest) {
        return selfTest() ? 0 : 1;
    }
    if (data.parseOnly) {
        return printParsedPanel(data) ? 0 : 1;
    }
    if (data.argumentError) {
        return 2;
    }
    auto args = gtkArgs(argv);
    int gtkArgc = static_cast<int>(args.size());
    auto *app = gtk_application_new("dev.tipe.LearningPanel", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &data);
    const int status = g_application_run(G_APPLICATION(app), gtkArgc, args.data());
    g_object_unref(app);
    return status;
}
