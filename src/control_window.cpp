#include "tipe/control_window.hpp"
#include "tipe/learning.hpp"
#include "tipe/support_page.hpp"
#include "tipe/control_style.hpp"
#include <QScreen>
#include <array>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdlib>
#include <memory>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace tipe::ui {
namespace {
class ControlWindowImpl final : public QWidget {
 public:
  explicit ControlWindowImpl(std::string dir, QWidget *parent)
      : QWidget(parent), state_dir_(std::move(dir)),
        learner_(std::make_unique<tipe::Learner>(state_dir_, tipe::LearnerMode::Control)) {
    setWindowTitle("TiPE · 控制中心");
    setObjectName("tipe-control-window");
    design::theme(this);
    setMinimumSize(460, 460);
    auto *root = design::column(this, 24, 16);
    auto *header = new QHBoxLayout;
    auto *logo = design::label("T", this, "logo", 1.9, true);
    logo->setAlignment(Qt::AlignCenter); logo->setFixedSize(46, 46);
    header->addWidget(logo);
    auto *brand = new QVBoxLayout; brand->setSpacing(2);
    brand->addWidget(design::label("TiPE", this, nullptr, 1.35, true));
    brand->addWidget(design::label("输入法控制中心", this, "muted", 0.9));
    header->addLayout(brand); header->addStretch();
    root->addLayout(header);
    auto *tabs = new QTabWidget(this); tabs->setObjectName("control-tabs");
    tabs->setDocumentMode(true); tabs->setUsesScrollButtons(false);
    tabs->tabBar()->setDrawBase(false);
    auto *status_page = new QWidget;
    auto *sl = design::column(status_page, 2, 12);
    sl->addWidget(design::label("学习状态", status_page, nullptr, 1.35, true));
    auto *hero = design::card(status_page, "hero");
    auto *hero_layout = design::column(hero, 18, 9);
    auto *hero_top = new design::ReflowRow(hero, 440);
    hero_title_ = design::label("", hero_top, nullptr, 1.15, true);
    health_ = design::label("", hero_top, "pill", 0.9);
    health_->setObjectName("learning-health");
    health_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    hero_top->box->addWidget(hero_title_, 1); hero_top->box->addWidget(health_);
    hero_layout->addWidget(hero_top);
    status_ = design::label("", hero, "muted"); status_->setObjectName("learning-status");
    hero_layout->addWidget(status_);
    auto *buttons = new design::ReflowRow(hero, 370);
    pause_ = new QPushButton(buttons); pause_->setObjectName("pause-learning"); pause_->setProperty("role", "primary");
    enable_ = new QPushButton(buttons); enable_->setObjectName("enable-learning"); enable_->setProperty("role", "quiet");
    buttons->box->addWidget(pause_); buttons->box->addWidget(enable_); buttons->box->addStretch();
    hero_layout->addWidget(buttons);
    sl->addWidget(hero);
    auto *metrics = new design::ReflowRow(status_page, 610);
    auto metric = [metrics](const QString &caption, const char *id, double scale) {
      auto *card = design::card(metrics); auto *layout = design::column(card, 18, 8);
      layout->addWidget(design::label(caption, card, "muted", 0.9));
      auto *value = design::label("", card, nullptr, scale, true); value->setObjectName(id);
      layout->addWidget(value); metrics->box->addWidget(card, 1); return value;
    };
    observed_ = metric("观察次数", "observed-count", 1.8);
    learned_ = metric("已学习词 / 模式", "learned-count", 1.8);
    current_model_ = metric("当前模型", "current-model", 1.1);
    sl->addWidget(metrics);
    auto *data = design::card(status_page); auto *dl = design::column(data, 18, 12);
    dl->addWidget(design::label("学习数据", data, nullptr, 1.1, true));
    auto data_action = [data, dl](const QString &title, const QString &description,
                                 const QString &button, const char *id, const char *role = nullptr) {
      auto *row = new design::ReflowRow(data, 480);
      auto *copy = new QWidget(row); auto *cl = design::column(copy, 0, 5);
      cl->addWidget(design::label(title, copy, nullptr, 1.0, true));
      cl->addWidget(design::label(description, copy, "muted", 0.9));
      auto *action = new QPushButton(button, row); action->setObjectName(id);
      if (role) action->setProperty("role", role);
      row->box->addWidget(copy, 1); row->box->addWidget(action);
      dl->addWidget(row); return action;
    };
    export_ = data_action("导出记录", "保存为 TSV 文件。", "导出学习数据", "export-learning");
    rollback_ = data_action("回滚模型", "恢复上一版个人模型。", "回滚上一版", "rollback-learning");
    clear_ = data_action("清空记录", "删除个人词、模式与观察计数。", "清空本地学习", "clear-learning", "danger");
    sl->addWidget(data);
    sl->addWidget(design::label("学习数据保存在本机。密码与敏感字段不会进入学习。", status_page, "muted", 0.9));
    sl->addStretch();
    auto *model_page = new QWidget; auto *ml = design::column(model_page, 2, 14);
    ml->addWidget(design::label("模型", model_page, nullptr, 1.35, true));
    ml->addWidget(design::label("选择候选排序的增强方式。", model_page, "muted"));
    backend_ = new QComboBox(model_page); backend_->setObjectName("model-backend");
    backend_->setAccessibleName("当前候选增强方式");
    ml->addWidget(backend_);
    const std::array<QString, 3> names{"TiP 个人学习", "本地小模型", "云端大模型"};
    const std::array<QString, 3> descriptions{
      "根据选词、修改和分段选择记录，调整候选排序。",
      "在本机学习汉字搭配，辅助候选排序。基础首选仍受保护。",
      "按需补充候选。需要配置服务并明确同意，默认关闭。"};
    const std::array<QString, 3> notes{
      "自动更新 · 数据保存在本机", "后台学习 · 无需联网", "仅发送必要语义 · 不发送原始按键流"};
    for (size_t i = 0; i < names.size(); ++i) {
      auto *card = design::card(model_page, "model-card"); model_cards_[i] = card;
      card->setProperty("selected", false);
      auto *cl = design::column(card, 18, 8);
      auto *row = new design::ReflowRow(card, 410);
      row->box->addWidget(design::label(names[i], row, nullptr, 1.1, true), 1);
      model_badges_[i] = design::label("", row, "eyebrow", 0.9);
      model_badges_[i]->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
      row->box->addWidget(model_badges_[i]); cl->addWidget(row);
      cl->addWidget(design::label(descriptions[i], card));
      cl->addWidget(design::label(notes[i], card, "muted", 0.9));
      ml->addWidget(card);
    }
    auto *details_toggle = new QPushButton("云端与文件设置  ＋", model_page);
    details_toggle->setObjectName("model-settings-toggle"); details_toggle->setProperty("role", "quiet");
    details_toggle->setCheckable(true); ml->addWidget(details_toggle);
    auto *settings = design::card(model_page); settings->setObjectName("model-settings");
    auto *settings_layout = design::column(settings, 18, 12);
    settings_layout->addWidget(design::label("API Key 文件位置", settings, nullptr, 1.0, true));
    settings_layout->addWidget(design::label("这里只保存文件位置。启用云端还需要配置服务并明确同意。", settings, "muted", 0.9));
    auto *key_row = new design::ReflowRow(settings, 490);
    auto *key_path = new QLineEdit(key_row); key_path->setObjectName("cloud-key-path");
    key_path->setPlaceholderText("选择存放 API Key 的私有文件");
    key_path->setAccessibleName("私有 API Key 文件路径");
    key_path->setText(QString::fromStdString(learner_->models().cloud_api_key_path()));
    key_path->setMinimumWidth(0);
    auto *save_key = new QPushButton("保存位置", key_row); save_key->setObjectName("save-cloud-key-path");
    key_row->box->addWidget(key_path, 1); key_row->box->addWidget(save_key); settings_layout->addWidget(key_row);
    auto *key_result = design::label("", settings, "muted", 0.9); key_result->setObjectName("cloud-key-result");
    settings_layout->addWidget(key_result);
    model_status_ = design::label("", settings, "muted", 0.9); model_status_->setObjectName("model-status");
    model_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    model_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    settings_layout->addWidget(model_status_); ml->addWidget(settings); settings->hide();
    connect(details_toggle, &QPushButton::toggled, this, [settings, details_toggle](bool open) {
      settings->setVisible(open); details_toggle->setText(open ? "收起云端与文件设置  −" : "云端与文件设置  ＋");
    });
    connect(save_key, &QPushButton::clicked, this, [this, key_path, key_result] {
      const bool ok = learner_->models().set_cloud_api_key_path(key_path->text().toStdString());
      key_result->setText(ok ? "已保存文件位置。此操作不会启用云端或读取密钥。" : "无法保存，请检查路径长度和文件权限。");
      refresh_models();
    });
    ml->addStretch();
    auto *status_scroll = design::scroll_page(status_page, tabs); status_scroll->setObjectName("learning-page-scroll");
    auto *model_scroll = design::scroll_page(model_page, tabs); model_scroll->setObjectName("model-page-scroll");
    tabs->addTab(status_scroll, "学习状态");
    tabs->addTab(model_scroll, "模型");
    tabs->addTab(tipe::ui::make_support_page(tabs), "支持");
    root->addWidget(tabs, 1);
    action_ = design::label("", this, "muted", 0.9); action_->setObjectName("action-status");
    root->addWidget(action_);
    action_->hide();
    const auto available = screen()->availableGeometry().size() - QSize(48, 64);
    resize(QSize(880, 760).boundedTo(available).expandedTo(minimumSize()));
    connect(pause_, &QPushButton::clicked, this, [this]{ const bool n=!learner_->paused(); if(n)export_worker_.request_stop(); const bool ok=learner_->pause(n); refresh_status(); feedback(ok?(n?"已暂停学习":"已恢复学习"):"暂停/恢复未确认，请检查输入法进程"); });
    connect(enable_, &QPushButton::clicked, this, [this]{ const bool n=!learner_->enabled(); if(!n)export_worker_.request_stop(); const bool ok=learner_->set_enabled(n); refresh_status(); feedback(ok?(n?"已开启学习":"已关闭学习"):"开启/关闭未确认，请检查输入法进程"); });
    connect(clear_, &QPushButton::clicked, this, [this]{ export_worker_.request_stop(); const bool ok=learner_->clear(); refresh_status(); feedback(ok?"已清空本地学习":"清空未确认，请检查输入法进程"); });
    connect(rollback_, &QPushButton::clicked, this, [this]{ export_worker_.request_stop(); const bool ok=learner_->rollback(); refresh_status(); feedback(ok?"已回滚上一版个人学习模型":"没有可回滚的模型，或输入法当前不可用"); });
    connect(export_, &QPushButton::clicked, this, [this]{ start_export(); });
    connect(backend_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int row){ if(row<0)return; const auto b=static_cast<tipe::ModelBackend>(backend_->itemData(row).toInt()); const bool ok=learner_->models().select(b); refresh_models(); feedback(ok?QString("已切换模型：%1").arg(backend_->currentText()):"模型不可用，未切换"); });
    timer_=new QTimer(this); connect(timer_, &QTimer::timeout, this, [this]{refresh_status(); refresh_models();}); timer_->start(1000); refresh_models(); refresh_status();
  }
  ~ControlWindowImpl() override {
    export_worker_.request_stop();
    if(export_worker_.joinable())export_worker_.join();
  }
  void refresh_status() {
    const auto s=learner_->status_snapshot(); QString m="TiP（个人学习）"; for(const auto&i:learner_->models().backends()) if(i.selected)m=QString::fromStdString(i.name);
    status_->setText(QString::fromStdString(s.status));
    observed_->setText(QString::number(static_cast<qulonglong>(s.observed)));
    learned_->setText(QString::number(static_cast<qulonglong>(s.learned)));
    current_model_->setText(m);
    hero_title_->setText("自动学习");
    health_->setText(!s.enabled ? "已关闭" : s.paused ? "已暂停" : "运行中");
    const QString tone = !s.enabled ? "off" : s.paused ? "paused" : "active";
    if (health_->property("tone").toString() != tone) {
      health_->setProperty("tone", tone); design::repolish(health_);
    }
    pause_->setText(s.paused ? "恢复学习" : "暂停学习");
    pause_->setEnabled(s.enabled);
    enable_->setText(s.enabled ? "关闭学习" : "开启学习");
  }
  void refresh_models() {
    learner_->models().refresh(); const auto infos=learner_->models().backends(); QSignalBlocker block(backend_); int selected=-1;
    // Preserve the live popup, highlight and keyboard focus during the
    // periodic refresh. Rebuilding the combo every second used to cancel a
    // user's selection while they were reading the choices.
    if (backend_->count() != static_cast<int>(infos.size())) {
      backend_->clear();
      for (const auto &info : infos)
        backend_->addItem(QString::fromStdString(info.name), static_cast<int>(info.backend));
    }
    for (size_t index = 0; index < infos.size(); ++index) {
      const auto &info = infos[index];
      if (backend_->itemText(static_cast<int>(index)) != QString::fromStdString(info.name))
        backend_->setItemText(static_cast<int>(index), QString::fromStdString(info.name));
      if (auto *model = qobject_cast<QStandardItemModel *>(backend_->model()))
        if (auto *item = model->item(static_cast<int>(index)))
          item->setEnabled(info.available);
      if (info.selected) selected = static_cast<int>(index);
    }
    if(selected>=0)backend_->setCurrentIndex(selected);
    for (size_t i = 0; i < infos.size() && i < model_cards_.size(); ++i) {
      const bool chosen = infos[i].selected;
      if (model_cards_[i]->property("selected").toBool() != chosen) {
        model_cards_[i]->setProperty("selected", chosen); design::repolish(model_cards_[i]);
      }
      model_badges_[i]->setText(chosen ? "使用中" : infos[i].available ? "可选" : "未配置");
    }
    QString text;
    const auto cloud = learner_->cloud_status();
    const char *cloud_state = cloud.state == tipe::CloudWorkerState::Sending ? "正在后台发送" :
                              cloud.state == tipe::CloudWorkerState::Backoff ? "下次自动重试" :
                              cloud.state == tipe::CloudWorkerState::Paused ? "已暂停" :
                              cloud.state == tipe::CloudWorkerState::Disabled ? "未启用" : "等待输入";
    text+=QString("本地小模型文件：%1\n以上位置是可选领域词表；字符搭配模型从个人学习自动生成。\nAPI Key 文件位置（仅保存路径；仅显式同意请求读取）：%2\n配置文件：%3\n云端后台状态：%4（待发送 %5，已发送 %6，失败 %7）")
      .arg(QString::fromStdString(learner_->models().local_small_path()))
      .arg(QString::fromStdString(learner_->models().cloud_api_key_path()))
      .arg(QString::fromStdString(learner_->models().path())).arg(cloud_state)
      .arg((qulonglong)cloud.queued).arg((qulonglong)cloud.sent).arg((qulonglong)cloud.failed);
    model_status_->setText(text);
  }
 private:
  void feedback(const QString &text) { action_->setText(text); action_->setVisible(!text.isEmpty()); }
  void start_export() {
    if(export_busy_) {
      export_worker_.request_stop();
      feedback("正在取消导出…");
      return;
    }
    if(export_worker_.joinable())export_worker_.join();
    export_busy_=true;
    export_->setText("取消导出");
    const auto path=export_path();
    const auto dir=state_dir_;
    feedback("正在后台导出，可随时取消。");
    export_worker_=std::jthread([this,dir,path](std::stop_token cancellation){
      ::setpriority(PRIO_PROCESS,static_cast<id_t>(::syscall(SYS_gettid)),10);
      bool ok=false;
      try {
        tipe::Learner latest(dir,tipe::LearnerMode::Control);
        ok=latest.export_data(path.toStdString(),cancellation);
      } catch(...) {}
      const bool cancelled=!ok && cancellation.stop_requested();
      QMetaObject::invokeMethod(this,[this,path,ok,cancelled]{
        export_busy_=false;export_->setText("导出学习数据");
        feedback(ok?QString("已导出到 %1").arg(path):
                         cancelled?"已取消导出，已有文件保持不变。":
                                   "导出失败，请检查目标目录是否可写。");
      },Qt::QueuedConnection);
    });
  }
  QString export_path()const { if(const char*p=std::getenv("TIPE_CONTROL_EXPORT_PATH");p&&*p)return QString::fromLocal8Bit(p); return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/tipe-learning.tsv"; }
  std::string state_dir_; std::unique_ptr<tipe::Learner> learner_; QLabel *status_{},*action_{},*model_status_{}; QPushButton *pause_{},*enable_{},*clear_{},*rollback_{},*export_{}; QComboBox *backend_{}; QTimer *timer_{};
  QLabel *observed_{}, *learned_{}, *current_model_{}, *health_{}, *hero_title_{};
  std::array<QFrame *, 3> model_cards_{};
  std::array<QLabel *, 3> model_badges_{};
  bool export_busy_{};
  std::jthread export_worker_;
};
}
QWidget *make_control_window(const std::string &dir, QWidget *parent) { return new ControlWindowImpl(dir,parent); }
}
