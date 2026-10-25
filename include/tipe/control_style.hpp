#pragma once

#include <QBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QResizeEvent>
#include <QResource>
#include <QScrollArea>
#include <QStyle>
#include <QWidget>
#include <algorithm>

inline void initialize_control_icons() {
  static const bool initialized = [] { Q_INIT_RESOURCE(control_icons); return true; }();
  (void)initialized;
}

namespace tipe::ui::design {

inline void theme(QWidget *root) {
  initialize_control_icons();
  auto font = root->font();
  font.setFamilies({"Noto Sans CJK SC", "MiSans", "sans-serif"});
  font.setStyleName("Regular");
  font.setWeight(QFont::Normal);
  font.setPointSizeF(std::max(10.0, font.pointSizeF()));
  root->setFont(font);
  root->setStyleSheet(R"(
    QWidget { color: #eceef4; }
    QWidget#tipe-control-window, QDialog { background: #111318; }
    QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }
    QLabel { background: transparent; border: none; }
    QLabel[role="muted"] { color: #9ba3b5; }
    QLabel[role="eyebrow"] { color: #a6a0ff; font-weight: 600; }
    QLabel[role="logo"] { background: #8176ef; color: white; border-radius: 13px; font-weight: 700; }
    QLabel[role="pill"] { background: #20252f; color: #c5cbd8; border-radius: 12px; padding: 5px 12px; }
    QLabel[role="pill"][tone="active"] { background: #18362f; color: #89dfbd; }
    QLabel[role="pill"][tone="paused"] { background: #3b3020; color: #edc98a; }
    QFrame[role="card"] { background: #1a1e27; border: 1px solid #2b303c; border-radius: 16px; }
    QFrame[role="hero"] { background: #20212f; border: 1px solid #38364f; border-radius: 18px; }
    QFrame[role="model-card"][selected="true"] { background: #242238; border: 1px solid #8e83e7; border-radius: 14px; }
    QFrame[role="model-card"][selected="false"] { background: #1a1e27; border: 1px solid #2b303c; border-radius: 14px; }
    QFrame[role="line"] { background: #2b303c; border: none; max-height: 1px; }
    QTabWidget::pane { border: none; background: transparent; }
    QTabBar::tab { background: transparent; color: #9ba3b5; padding: 10px 22px; margin: 0px 6px 12px 0px; border-radius: 10px; }
    QTabBar::tab:selected { background: #2b283f; color: #d9d3ff; }
    QTabBar::tab:hover:!selected { background: #1e222c; color: #eceef4; }
    QPushButton { background: #292e3b; border: 1px solid #3b4252; border-radius: 9px; padding: 8px 16px; color: #e5e8f0; }
    QPushButton:hover { background: #353b4c; border-color: #626c83; }
    QPushButton:pressed { background: #202430; }
    QPushButton:focus, QComboBox:focus, QLineEdit:focus { border: 1px solid #b6aaff; }
    QPushButton:disabled { color: #747c8f; background: #20242e; border-color: #2b303c; }
    QPushButton[role="primary"] { background: #a89afa; color: #191524; border: 1px solid #a89afa; font-weight: 600; }
    QPushButton[role="primary"]:hover { background: #c0b4ff; border-color: #c0b4ff; }
    QPushButton[role="danger"] { background: #2d2129; color: #eea4ae; border-color: #5c3945; }
    QPushButton[role="danger"]:hover { background: #422934; border-color: #bd7988; }
    QPushButton[role="quiet"] { background: transparent; color: #bcb4f4; border-color: #3c3b51; }
    QComboBox, QLineEdit { background: #12161e; border: 1px solid #414959; border-radius: 9px; padding: 10px 12px; selection-background-color: #6759a9; }
    QComboBox { padding-right: 30px; }
    QComboBox::drop-down { width: 26px; border: none; }
    QComboBox::down-arrow { image: url(:/tipe/ui/chevron-down.png); width: 14px; height: 14px; }
    QComboBox QAbstractItemView { background: #242938; color: #eceef4; selection-background-color: #51466f; border: 1px solid #535b70; padding: 5px; }
    QScrollBar:vertical { background: transparent; width: 8px; margin: 4px 0px; }
    QScrollBar::handle:vertical { background: #485064; min-height: 32px; border-radius: 4px; }
    QScrollBar::handle:vertical:hover { background: #737e98; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
    QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    QToolTip { color: #eceef4; background: #252b37; border: 1px solid #596177; padding: 6px; }
  )");
}

inline QLabel *label(const QString &text, QWidget *parent,
                     const char *role = nullptr, double scale = 1.0, bool bold = false) {
  auto *result = new QLabel(text, parent);
  result->setTextFormat(Qt::PlainText);
  result->setWordWrap(true);
  result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  if (role) result->setProperty("role", role);
  auto font = parent->font();
  font.setPointSizeF(std::max(10.0, font.pointSizeF()) * scale);
  font.setStyleName(bold ? "Medium" : "Regular");
  font.setWeight(bold ? QFont::Medium : QFont::Normal);
  result->setFont(font);
  return result;
}

inline QFrame *card(QWidget *parent, const char *role = "card") {
  auto *frame = new QFrame(parent);
  frame->setProperty("role", role);
  return frame;
}

inline QVBoxLayout *column(QWidget *widget, int padding = 20, int spacing = 12) {
  auto *layout = new QVBoxLayout(widget);
  layout->setContentsMargins(padding, padding, padding, padding);
  layout->setSpacing(spacing);
  return layout;
}

// Horizontal at comfortable widths; vertical on compact windows and large
// fonts. Child minima cannot force a horizontal scrollbar before reflow.
class ReflowRow final : public QWidget {
 public:
  explicit ReflowRow(QWidget *parent, int breakpoint = 570)
      : QWidget(parent), breakpoint_(breakpoint) {
    box = new QBoxLayout(QBoxLayout::LeftToRight, this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(14);
    box->setSizeConstraint(QLayout::SetNoConstraint);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  }
  QSize minimumSizeHint() const override { return {0, 0}; }
  QBoxLayout *box{};
 protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    const auto direction = width() < breakpoint_ * std::max(1.0, font().pointSizeF()/10.0)
                              ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (box->direction() != direction) {
      box->setDirection(direction);
      box->activate();
      updateGeometry();
    }
  }
 private:
  int breakpoint_;
};

inline QScrollArea *scroll_page(QWidget *content, QWidget *parent) {
  auto *scroll = new QScrollArea(parent);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(content);
  return scroll;
}

inline void repolish(QWidget *widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}
}
