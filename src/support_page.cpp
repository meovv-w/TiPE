#include "tipe/support_page.hpp"
#include "tipe/control_style.hpp"
#include <QPainterPath>

#include <QBoxLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialog>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QResource>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

static void initialize_support_images() {
  Q_INIT_RESOURCE(support_payments);
}

namespace tipe::ui {
namespace {

class PaymentImage : public QLabel {
 public:
  explicit PaymentImage(const QString &path, QWidget *parent)
      : QLabel(parent), original_(path) {
    setMinimumSize(180, 180);
    setFixedHeight(244);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("paymentImageLoaded", !original_.isNull());
    setProperty("paymentImageSize", original_.size());
    // Display the supplied QR with its white quiet zone; preserve the full
    // original for the enlarged view, including platform and recipient.
    thumbnail_ = original_.copy(path.contains("wechat") ? QRect(210, 264, 410, 410)
                                                       : QRect(218, 686, 846, 846));
  }
  QSize minimumSizeHint() const override { return {180, 180}; }
 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath background; background.addRoundedRect(rect(), 12, 12);
    painter.fillPath(background, Qt::white);
    const auto size = thumbnail_.size().scaled(contentsRect().size() - QSize(20, 20), Qt::KeepAspectRatio);
    const QRect target(QPoint((width()-size.width())/2, (height()-size.height())/2), size);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(target, thumbnail_);
  }
 private:
  QPixmap original_, thumbnail_;
};

class SupportPage : public QScrollArea {
 public:
  explicit SupportPage(QWidget *parent) : QScrollArea(parent) {
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  }

  QBoxLayout *codes{};

 protected:
  void resizeEvent(QResizeEvent *event) override {
    QScrollArea::resizeEvent(event);
    if (codes) {
      // Stack the two cards when their readable minimum sizes no longer fit.
      // Vertical scrolling also keeps all content reachable with large fonts.
      const int card_width = codes->itemAt(0)->widget()->minimumSizeHint().width();
      codes->setDirection(viewport()->width() >= 2 * card_width + 44
                              ? QBoxLayout::LeftToRight
                              : QBoxLayout::TopToBottom);
      codes->invalidate();
      codes->activate();
      widget()->setMinimumWidth(0);
      widget()->updateGeometry();
    }
  }
};

QFrame *make_payment_card(const QString &name, const QString &name_id,
                                 QWidget *parent) {
  auto *card = design::card(parent);
  card->setObjectName(name_id + "-card");
  auto *layout = design::column(card, 18, 12);

  auto *title = design::label(name, card, nullptr, 1.15, true);
  title->setAlignment(Qt::AlignCenter);
  title->setObjectName(name_id + "-title");
  layout->addWidget(title);

  const auto path = ":/tipe/support/" + name_id + ".jpg";
  auto *image = new PaymentImage(path, card);
  image->setObjectName(name_id + "-code");
  image->setAccessibleName(name + "收款码");
  layout->addWidget(image);
  auto *view = new QPushButton("查看收款码原图", card);
  view->setProperty("role", "quiet");
  view->setObjectName(name_id + "-view");
  QObject::connect(view, &QPushButton::clicked, card, [card, name, path] {
    auto *dialog = new QDialog(card);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(name + "收款码");
    design::theme(dialog);
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(dialog);
    const QPixmap original(path);
    const auto available = card->screen()->availableGeometry().size() * 0.8;
    const auto size = original.size().boundedTo(available);
    label->setPixmap(original.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label->setAlignment(Qt::AlignCenter);
    label->setAccessibleName(name + "收款码原图");
    layout->addWidget(label);
    dialog->show();
  });
  layout->addWidget(view);
  return card;
}

}  // namespace

QWidget *make_support_page(QWidget *parent) {
  initialize_support_images();
  auto *scroll = new SupportPage(parent);
  scroll->setObjectName("tipe-support-page");
  design::theme(scroll);
  auto *page = new QWidget;
  scroll->setWidget(page);
  auto *layout = design::column(page, 2, 16);
  auto *title = design::label("支持开发", page, nullptr, 1.35, true);
  title->setObjectName("support-title");
  layout->addWidget(title);
  auto *summary = design::label("微信和支付宝收款码。", page, "muted");
  summary->setObjectName("support-summary");
  layout->addWidget(summary);

  auto *codes = new QBoxLayout(QBoxLayout::LeftToRight);
  codes->setObjectName("payment-codes");
  scroll->codes = codes;
  codes->setSpacing(12);
  codes->addWidget(make_payment_card("微信", "wechat", page), 1);
  codes->addWidget(make_payment_card("支付宝", "alipay", page), 1);
  layout->addLayout(codes);

  auto *note = design::label("使用对应应用扫码，可自选金额。", page, "muted", 0.9);
  note->setObjectName("payment-note");
  layout->addWidget(note);
  auto *privacy = design::label("学习数据保存在本机，默认不联网。", page, "muted", 0.9);
  privacy->setObjectName("support-privacy");
  layout->addWidget(privacy);
  layout->addStretch();
  return scroll;
}

}  // namespace tipe::ui
