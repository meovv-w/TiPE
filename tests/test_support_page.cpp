#include "tipe/support_page.hpp"

#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

#include <cassert>
#include <iostream>

static void check_layout(QApplication &app, QWidget *page, int width) {
  page->resize(width, 520);
  page->show();
  app.processEvents();
  auto *scroll = qobject_cast<QScrollArea *>(page);
  auto *wechat = page->findChild<QFrame *>("wechat-card");
  auto *alipay = page->findChild<QFrame *>("alipay-card");
  assert(scroll && wechat && alipay);
  // Both payment destinations must remain separately visible and completely
  // reachable without horizontal scrolling, including a narrow window.
  assert(!wechat->geometry().intersects(alipay->geometry()));
  assert(scroll->widget()->rect().contains(wechat->geometry()));
  assert(scroll->widget()->rect().contains(alipay->geometry()));
  assert(scroll->widget()->width() <= scroll->viewport()->width());
  assert(scroll->horizontalScrollBar()->maximum() == 0);
  const auto *summary = page->findChild<QLabel *>("support-summary");
  const auto *privacy = page->findChild<QLabel *>("support-privacy");
  assert(summary->height() >= summary->heightForWidth(summary->width()));
  assert(privacy->height() >= privacy->heightForWidth(privacy->width()));
}

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  auto *page = tipe::ui::make_support_page();
  assert(page != nullptr);
  assert(page->objectName() == "tipe-support-page");
  const auto *summary = page->findChild<QLabel *>("support-summary");
  const auto *privacy = page->findChild<QLabel *>("support-privacy");
  const auto *wechat = page->findChild<QLabel *>("wechat-code");
  const auto *alipay = page->findChild<QLabel *>("alipay-code");
  assert(summary && !summary->text().isEmpty());
  assert(privacy && privacy->text().contains("保存在本机"));
  assert(wechat && wechat->property("paymentImageLoaded").toBool());
  assert(alipay && alipay->property("paymentImageLoaded").toBool());
  assert(wechat->property("paymentImageSize").toSize() == QSize(828, 1124));
  assert(alipay->property("paymentImageSize").toSize() == QSize(1280, 1919));
  assert(wechat->minimumSize().width() >= 160);
  assert(alipay->minimumSize().height() >= 160);
  check_layout(app, page, 700);
  auto *view = page->findChild<QPushButton *>("wechat-view");
  assert(view);
  view->click();
  app.processEvents();
  auto *dialog = page->findChild<QDialog *>();
  assert(dialog && dialog->isVisible());
  assert(!dialog->findChild<QLabel *>()->pixmap().isNull());
  dialog->close();
  check_layout(app, page, 260);
  if (argc > 1) {
    page->resize(700, 740);
    app.processEvents();
    assert(page->grab().save(QString::fromLocal8Bit(argv[1])));
  }
  delete page;

  QFont large = app.font();
  large.setPointSize(20);
  app.setFont(large);
  page = tipe::ui::make_support_page();
  check_layout(app, page, 700);
  check_layout(app, page, 320);
  delete page;
  std::cout << "support page content, narrow layout and large font checks passed\n";
}
