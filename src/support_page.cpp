#include "tipe/support_page.hpp"

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
    setMinimumSize(180, 300);
    setFixedHeight(380);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setProperty("paymentImageLoaded", !original_.isNull());
    setProperty("paymentImageSize", original_.size());
  }
  QSize minimumSizeHint() const override { return {180, 300}; }
 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    const auto size = original_.size().scaled(contentsRect().size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width()-size.width())/2, (height()-size.height())/2), size);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(target, original_);
  }
 private:
  QPixmap original_;
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
  auto *card = new QFrame(parent);
  card->setFrameShape(QFrame::StyledPanel);
  card->setFrameShadow(QFrame::Plain);
  card->setObjectName(name_id + "-card");
  auto *layout = new QVBoxLayout(card);
  layout->setContentsMargins(12, 12, 12, 12);

  auto *title = new QLabel(name, card);
  title->setAlignment(Qt::AlignCenter);
  title->setObjectName(name_id + "-title");
  layout->addWidget(title);

  const auto path = ":/tipe/support/" + name_id + ".jpg";
  auto *image = new PaymentImage(path, card);
  image->setObjectName(name_id + "-code");
  image->setAccessibleName(name + "收款码");
  layout->addWidget(image);
  auto *view = new QPushButton("查看大图", card);
  view->setObjectName(name_id + "-view");
  QObject::connect(view, &QPushButton::clicked, card, [card, name, path] {
    auto *dialog = new QDialog(card);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(name + "收款码");
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
  auto *page = new QWidget;
  scroll->setWidget(page);
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  auto *title = new QLabel("支持 TiPE", page);
  title->setObjectName("support-title");
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 1);
  title->setFont(title_font);
  layout->addWidget(title);

  auto *summary = new QLabel(
      "TiPE 是一个本地优先的中文拼音输入法。它会安静地学习你明确选择的词，"
      "帮助改进后续候选。感谢你支持 TiPE 的持续开发。",
      page);
  summary->setObjectName("support-summary");
  summary->setWordWrap(true);
  layout->addWidget(summary);

  auto *privacy = new QLabel(
      "学习数据默认保存在本机，不会因为按键而自动上传内容。"
      "你可以在“学习状态”页暂停、清空或导出数据。",
      page);
  privacy->setObjectName("support-privacy");
  privacy->setWordWrap(true);
  layout->addWidget(privacy);

  auto *codes = new QBoxLayout(QBoxLayout::LeftToRight);
  codes->setObjectName("payment-codes");
  scroll->codes = codes;
  codes->setSpacing(12);
  codes->addWidget(make_payment_card("微信", "wechat", page), 1);
  codes->addWidget(make_payment_card("支付宝", "alipay", page), 1);
  layout->addLayout(codes);

  auto *note = new QLabel("使用微信或支付宝扫一扫。感谢支持 TiPE 的持续开发。",
                          page);
  note->setObjectName("payment-note");
  note->setWordWrap(true);
  layout->addWidget(note);
  layout->addStretch();
  return scroll;
}

}  // namespace tipe::ui
