#include "tipe/control_window.hpp"
#include "tipe/learning.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QAbstractItemView>
#include <QDir>
#include <QFont>
#include <QWidget>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
  const std::string dir = "/tmp/tipe-control-window-test";
  const std::string export_path = dir + "/export.tsv";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  // Publish one durable entry before opening the read-only control observer.
  {
    unsetenv("TIPE_LEARNER_READ_ONLY");
    tipe::Learner writer(dir);
    tipe::Event event;
    event.type = tipe::EventType::Commit;
    event.pinyin = "nihao";
    event.committed = "你好";
    writer.push(event);
    assert(writer.wait_until_idle(std::chrono::seconds(2)));
  }
  setenv("TIPE_CONTROL_EXPORT_PATH", export_path.c_str(), 1);
  QApplication app(argc, argv);
  if (std::getenv("TIPE_CONTROL_LARGE_FONT")) {
    auto font = app.font(); font.setPointSize(16); app.setFont(font);
  }
  auto *window = tipe::ui::make_control_window(dir);
  assert(window && window->objectName() == "tipe-control-window");
  window->show();
  app.processEvents();

  auto *status = window->findChild<QLabel *>("learning-status");
  auto *action = window->findChild<QLabel *>("action-status");
  auto *pause = window->findChild<QPushButton *>("pause-learning");
  auto *enable = window->findChild<QPushButton *>("enable-learning");
  auto *clear = window->findChild<QPushButton *>("clear-learning");
  auto *export_button = window->findChild<QPushButton *>("export-learning");
  auto *backend = window->findChild<QComboBox *>("model-backend");
  auto *model_status = window->findChild<QLabel *>("model-status");
  assert(status && action && pause && enable && clear && export_button && backend && model_status);
  assert(status->text().contains("已学词/模式：1"));
  assert(status->text().contains("TiP（个人学习）"));
  assert(backend->count() == 3);
  assert(model_status->text().contains("本地小模型文件："));
  auto *key_path=window->findChild<QLineEdit *>("cloud-key-path");
  auto *save_key=window->findChild<QPushButton *>("save-cloud-key-path");
  assert(key_path && save_key);
  key_path->setText(QString::fromStdString(dir+"/private-api-key"));
  save_key->click();app.processEvents();
  tipe::ModelSettings saved_settings(dir);
  assert(saved_settings.cloud_api_key_path()==dir+"/private-api-key");
  assert(!saved_settings.cloud_enabled());

  // Optional reproducible visual artifacts use the actual QWidget tree;
  // they are independent from the running desktop and never move a window.
  if (const char *capture_dir = std::getenv("TIPE_CONTROL_CAPTURE_DIR")) {
    QDir().mkpath(QString::fromLocal8Bit(capture_dir));
    auto *tabs = window->findChild<QTabWidget *>("control-tabs");
    assert(tabs && tabs->count() == 3);
    for (int index = 0; index < tabs->count(); ++index) {
      tabs->setCurrentIndex(index); app.processEvents();
      assert(window->grab().save(QString::fromLocal8Bit(capture_dir) + "/page-" + QString::number(index) + ".png"));
    }
    tabs->setCurrentIndex(0); app.processEvents();
  }

  // A refresh must retain the model items while its popup is being read.
  // Persistent indexes are invalidated by clear()/rebuild even when all
  // displayed values are unchanged.
  const QPersistentModelIndex original_item(backend->model()->index(0, 0));

  int local_row = -1;
  for (int i = 0; i < backend->count(); ++i)
    if (backend->itemData(i).toInt() == static_cast<int>(tipe::ModelBackend::LocalSmall))
      local_row = i;
  assert(local_row >= 0);
  const auto local_flags =
      backend->model()->flags(backend->model()->index(local_row, 0));
  assert(local_flags.testFlag(Qt::ItemIsEnabled));
  backend->setCurrentIndex(local_row);
  app.processEvents();
  assert(action->text().contains("已切换模型"));
  assert(std::filesystem::exists(dir + "/models.current"));

  // The one-second observer timer must pick up a publication made after the
  // control center was opened, rather than leaving the initial snapshot stale.
  unsetenv("TIPE_LEARNER_READ_ONLY");
  {
    tipe::Learner writer(dir);
    tipe::Event later;
    later.type = tipe::EventType::Commit;
    later.pinyin = "xiexie";
    later.committed = "谢谢";
    writer.push(later);
    assert(writer.wait_until_idle(std::chrono::seconds(2)));
  }
  setenv("TIPE_LEARNER_READ_ONLY", "1", 1);
  const auto refresh_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(1300);
  while (std::chrono::steady_clock::now() < refresh_deadline) {
    app.processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(status->text().contains("已学词/模式：2"));
  assert(original_item.isValid());

  pause->click();
  app.processEvents();
  assert(action->text() == "已暂停学习");
  assert(status->text().contains("已暂停"));
  pause->click();
  app.processEvents();
  assert(action->text() == "已恢复学习");

  enable->click();
  app.processEvents();
  assert(action->text() == "已关闭学习");
  assert(status->text().contains("已关闭"));
  enable->click();
  app.processEvents();
  assert(action->text() == "已开启学习");

  export_button->click();
  assert(export_button->text()=="取消导出");
  const auto export_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(export_button->text()=="取消导出" && std::chrono::steady_clock::now()<export_deadline) {
    app.processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(action->text().contains(QString::fromStdString(export_path)));
  assert(std::filesystem::exists(export_path));
  std::ifstream exported(export_path);
  std::string line;
  bool found_hello = false;
  while (std::getline(exported, line))
    found_hello = found_hello || line.find("你好") != std::string::npos;
  assert(found_hello);
  // Cancellation is immediately available without waiting for a worker or
  // blocking the event loop. A tiny export may already have atomically
  // finished; both outcomes must restore the button and keep a whole file.
  const auto saved_size=std::filesystem::file_size(export_path);
  export_button->click();
  export_button->click();
  assert(action->text()=="正在取消导出…");
  const auto cancel_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(export_button->text()=="取消导出" && std::chrono::steady_clock::now()<cancel_deadline) {
    app.processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(export_button->text()=="导出学习数据");
  assert(action->text().contains("已取消") || action->text().contains("已导出"));
  assert(std::filesystem::file_size(export_path)==saved_size);

  clear->click();
  app.processEvents();
  assert(action->text() == "已清空本地学习");
  assert(status->text().contains("已学词/模式：0"));
  delete window;
  unsetenv("TIPE_CONTROL_EXPORT_PATH");
  return 0;
}
