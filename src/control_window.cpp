#include "tipe/control_window.hpp"
#include "tipe/learning.hpp"
#include "tipe/support_page.hpp"
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
    setWindowTitle("TiPE 输入法"); setObjectName("tipe-control-window");
    auto *tabs = new QTabWidget(this); tabs->setObjectName("control-tabs");
    auto *status_page = new QWidget(tabs); auto *sl = new QVBoxLayout(status_page);
    status_ = new QLabel(status_page); status_->setObjectName("learning-status"); status_->setWordWrap(true);
    action_ = new QLabel(status_page); action_->setObjectName("action-status"); action_->setWordWrap(true);
    pause_ = new QPushButton(status_page); pause_->setObjectName("pause-learning");
    enable_ = new QPushButton(status_page); enable_->setObjectName("enable-learning");
    clear_ = new QPushButton("清空本地学习", status_page); clear_->setObjectName("clear-learning");
    rollback_ = new QPushButton("回滚上一版", status_page); rollback_->setObjectName("rollback-learning");
    export_ = new QPushButton("导出学习数据", status_page); export_->setObjectName("export-learning");
    auto *buttons = new QHBoxLayout; buttons->addWidget(pause_); buttons->addWidget(enable_); buttons->addStretch();
    auto *data_buttons = new QHBoxLayout; data_buttons->addWidget(export_); data_buttons->addWidget(rollback_); data_buttons->addWidget(clear_); data_buttons->addStretch();
    sl->setContentsMargins(20, 20, 20, 20); sl->setSpacing(18);
    sl->addWidget(status_); sl->addLayout(buttons); sl->addLayout(data_buttons); sl->addWidget(action_); sl->addStretch();
    auto *model_page = new QWidget(tabs); auto *ml = new QVBoxLayout(model_page);
    backend_ = new QComboBox(model_page); backend_->setObjectName("model-backend");
    model_status_ = new QLabel(model_page); model_status_->setObjectName("model-status"); model_status_->setWordWrap(true);
    auto *model_scroll = new QScrollArea(model_page); model_scroll->setObjectName("model-details-scroll");
    model_scroll->setWidgetResizable(true); model_scroll->setFrameShape(QFrame::NoFrame);
    model_scroll->setWidget(model_status_);
    model_status_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    model_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    model_status_->setMargin(4);
    ml->setContentsMargins(20, 20, 20, 20); ml->setSpacing(12);
    ml->addWidget(new QLabel("选择候选增强方式：", model_page)); ml->addWidget(backend_);
    auto *key_row=new QHBoxLayout;
    auto *key_path=new QLineEdit(model_page);key_path->setObjectName("cloud-key-path");
    key_path->setPlaceholderText("私有 API Key 文件路径（仅保存位置）");
    key_path->setText(QString::fromStdString(learner_->models().cloud_api_key_path()));
    auto *save_key=new QPushButton("保存位置",model_page);save_key->setObjectName("save-cloud-key-path");
    key_row->addWidget(key_path);key_row->addWidget(save_key);ml->addLayout(key_row);
    auto *key_result=new QLabel(model_page);key_result->setObjectName("cloud-key-result");key_result->setWordWrap(true);
    ml->addWidget(key_result);ml->addWidget(model_scroll);
    connect(save_key,&QPushButton::clicked,this,[this,key_path,key_result]{
      const bool ok=learner_->models().set_cloud_api_key_path(key_path->text().toStdString());
      key_result->setText(ok?"已保存文件位置。此操作不会启用云端或读取密钥。":"无法保存，请检查路径长度和文件权限。");
      refresh_models();
    });
    tabs->addTab(status_page, "学习状态"); tabs->addTab(model_page, "模型"); tabs->addTab(tipe::ui::make_support_page(tabs), "支持");
    auto *root = new QVBoxLayout(this); root->addWidget(tabs); resize(600, 520);
    connect(pause_, &QPushButton::clicked, this, [this]{ const bool n=!learner_->paused(); if(n)export_worker_.request_stop(); const bool ok=learner_->pause(n); refresh_status(); action_->setText(ok?(n?"已暂停学习":"已恢复学习"):"暂停/恢复未确认，请检查输入法进程"); });
    connect(enable_, &QPushButton::clicked, this, [this]{ const bool n=!learner_->enabled(); if(!n)export_worker_.request_stop(); const bool ok=learner_->set_enabled(n); refresh_status(); action_->setText(ok?(n?"已开启学习":"已关闭学习"):"开启/关闭未确认，请检查输入法进程"); });
    connect(clear_, &QPushButton::clicked, this, [this]{ export_worker_.request_stop(); const bool ok=learner_->clear(); refresh_status(); action_->setText(ok?"已清空本地学习":"清空未确认，请检查输入法进程"); });
    connect(rollback_, &QPushButton::clicked, this, [this]{ export_worker_.request_stop(); const bool ok=learner_->rollback(); refresh_status(); action_->setText(ok?"已回滚上一版个人学习模型":"没有可回滚的模型，或输入法当前不可用"); });
    connect(export_, &QPushButton::clicked, this, [this]{ start_export(); });
    connect(backend_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int row){ if(row<0)return; const auto b=static_cast<tipe::ModelBackend>(backend_->itemData(row).toInt()); const bool ok=learner_->models().select(b); refresh_models(); action_->setText(ok?QString("已切换模型：%1").arg(backend_->currentText()):"模型不可用，未切换"); });
    timer_=new QTimer(this); connect(timer_, &QTimer::timeout, this, [this]{refresh_status(); refresh_models();}); timer_->start(1000); refresh_models(); refresh_status();
  }
  ~ControlWindowImpl() override {
    export_worker_.request_stop();
    if(export_worker_.joinable())export_worker_.join();
  }
  void refresh_status() {
    const auto s=learner_->status_snapshot(); QString m="TiP（个人学习）"; for(const auto&i:learner_->models().backends()) if(i.selected)m=QString::fromStdString(i.name);
    status_->setText(QString("%1\n观察次数：%2\n已学词/模式：%3\n当前模型：%4\n学习：%5").arg(QString::fromStdString(s.status)).arg((qulonglong)s.observed).arg((qulonglong)s.learned).arg(m).arg(!s.enabled?"已关闭":s.paused?"已暂停":"运行中")); pause_->setText(s.paused?"恢复学习":"暂停学习"); enable_->setText(s.enabled?"关闭学习":"开启学习");
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
    if(selected>=0)backend_->setCurrentIndex(selected); QString text; for(const auto&i:infos) text+=QString::fromStdString(i.name+"："+i.status+"\n"+i.description+"\n运行："+i.run_when+"\n隐私："+i.privacy+"\n\n");
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
  void start_export() {
    if(export_busy_) {
      export_worker_.request_stop();
      action_->setText("正在取消导出…");
      return;
    }
    if(export_worker_.joinable())export_worker_.join();
    export_busy_=true;
    export_->setText("取消导出");
    const auto path=export_path();
    const auto dir=state_dir_;
    action_->setText("正在后台导出，可随时取消。");
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
        action_->setText(ok?QString("已导出到 %1").arg(path):
                         cancelled?"已取消导出，已有文件保持不变。":
                                   "导出失败，请检查目标目录是否可写。");
      },Qt::QueuedConnection);
    });
  }
  QString export_path()const { if(const char*p=std::getenv("TIPE_CONTROL_EXPORT_PATH");p&&*p)return QString::fromLocal8Bit(p); return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/tipe-learning.tsv"; }
  std::string state_dir_; std::unique_ptr<tipe::Learner> learner_; QLabel *status_{},*action_{},*model_status_{}; QPushButton *pause_{},*enable_{},*clear_{},*rollback_{},*export_{}; QComboBox *backend_{}; QTimer *timer_{};
  bool export_busy_{};
  std::jthread export_worker_;
};
}
QWidget *make_control_window(const std::string &dir, QWidget *parent) { return new ControlWindowImpl(dir,parent); }
}
