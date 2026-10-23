#include <QApplication>
#include <QLineEdit>
#include <QTimer>
#include <QInputMethodEvent>
#include <QVariant>
#include <QRect>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QByteArray>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <cstdlib>

namespace {

void emit_event(const QJsonObject &object) {
  QTextStream out(stdout);
  out << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
  out.flush();
}

bool env_enabled(const char *name) {
  const QByteArray value = qgetenv(name);
  return value == "1" || value.compare("true", Qt::CaseInsensitive) == 0 ||
         value.compare("yes", Qt::CaseInsensitive) == 0;
}

class HarnessEdit final : public QLineEdit {
public:
  explicit HarnessEdit(QWidget *parent = nullptr) : QLineEdit(parent) {
    setPlaceholderText(QStringLiteral("TiPE Qt real-app harness"));
    sensitive_ = env_enabled("TIPE_HARNESS_PASSWORD") ||
                 env_enabled("TIPE_HARNESS_SENSITIVE") ||
                 env_enabled("TIPE_HARNESS_PASSWORD_ECHO");
    if (sensitive_) {
      // These are the same Qt hints consumed by the platform input context;
      // they make the password capability observable at the real toolkit
      // boundary instead of merely testing a core boolean.
      setEchoMode(QLineEdit::Password);
      setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData |
                          Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    }
    no_surrounding_ = env_enabled("TIPE_HARNESS_NO_SURROUNDING");
    QObject::connect(this, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                       QJsonObject object;
                       object[QStringLiteral("event")] = QStringLiteral("text");
                       if (sensitive_) {
                         object[QStringLiteral("value_length")] = text.size();
                         object[QStringLiteral("redacted")] = true;
                       } else {
                         object[QStringLiteral("value")] = text;
                       }
                       object[QStringLiteral("cursor")] = cursorPosition();
                       object[QStringLiteral("sensitive")] = sensitive_;
                       emit_event(object);
                     });
  }

  void inputMethodEvent(QInputMethodEvent *event) override {
    QJsonObject object;
    object[QStringLiteral("event")] = QStringLiteral("input_method");
    if (sensitive_) {
      object[QStringLiteral("preedit_length")] = event->preeditString().size();
      object[QStringLiteral("commit_length")] = event->commitString().size();
      object[QStringLiteral("redacted")] = true;
    } else {
      object[QStringLiteral("preedit")] = event->preeditString();
      object[QStringLiteral("commit")] = event->commitString();
    }
    object[QStringLiteral("sensitive")] = sensitive_;
    object[QStringLiteral("replacement_start")] = event->replacementStart();
    object[QStringLiteral("replacement_length")] = event->replacementLength();
    emit_event(object);
    QLineEdit::inputMethodEvent(event);
  }

  QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
    // Some terminal/embedded clients cannot provide surrounding text.  Keep
    // this opt-in switch in the real Qt widget so fallback behavior can be
    // observed without fabricating an Fcitx callback.
    if (no_surrounding_ && query == Qt::ImSurroundingText) {
      QJsonObject object;
      object[QStringLiteral("event")] =
          QStringLiteral("input_method_query");
      object[QStringLiteral("query")] = static_cast<int>(query);
      object[QStringLiteral("surrounding_available")] = false;
      object[QStringLiteral("sensitive")] = sensitive_;
      emit_event(object);
      // An invalid QVariant is Qt's public indication that this client cannot
      // answer ImSurroundingText. Returning an empty QString would advertise
      // a valid (but empty) document and could invite an unsafe restoration.
      return {};
    }
    const QVariant value = QLineEdit::inputMethodQuery(query);
    if (query == Qt::ImCursorRectangle || query == Qt::ImSurroundingText ||
        query == Qt::ImCursorPosition) {
      QJsonObject object;
      object[QStringLiteral("event")] = QStringLiteral("input_method_query");
      object[QStringLiteral("query")] = static_cast<int>(query);
      if (query == Qt::ImCursorRectangle) {
        const QRect rect = value.toRect();
        object[QStringLiteral("value")] =
            QStringLiteral("%1,%2 %3x%4")
                .arg(rect.x())
                .arg(rect.y())
                .arg(rect.width())
                .arg(rect.height());
      }
      else if (query == Qt::ImCursorPosition)
        object[QStringLiteral("value")] = value.toInt();
      else
        object[QStringLiteral("value")] = sensitive_ ? QStringLiteral("<redacted>")
                                                       : value.toString();
      object[QStringLiteral("sensitive")] = sensitive_;
      object[QStringLiteral("surrounding_available")] = !no_surrounding_;
      if (sensitive_ && query == Qt::ImSurroundingText) {
        object.remove(QStringLiteral("value"));
        object[QStringLiteral("value_length")] = value.toString().size();
        object[QStringLiteral("redacted")] = true;
      }
      emit_event(object);
    }
    return value;
  }

  void keyPressEvent(QKeyEvent *event) override {
    const int key = event->key();
    const bool tracked = key == Qt::Key_Backspace || key == Qt::Key_Delete;
    if (tracked) {
      QJsonObject object;
      object[QStringLiteral("event")] = QStringLiteral("key_press");
      object[QStringLiteral("key")] = key;
      object[QStringLiteral("phase")] = QStringLiteral("before");
      object[QStringLiteral("value")] = sensitive_ ? QString() : text();
      object[QStringLiteral("value_length")] = text().size();
      object[QStringLiteral("cursor")] = cursorPosition();
      object[QStringLiteral("sensitive")] = sensitive_;
      object[QStringLiteral("surrounding_available")] = !no_surrounding_;
      emit_event(object);
    }
    QLineEdit::keyPressEvent(event);
    if (tracked) {
      QJsonObject object;
      object[QStringLiteral("event")] = QStringLiteral("key_press");
      object[QStringLiteral("key")] = key;
      object[QStringLiteral("phase")] = QStringLiteral("after");
      object[QStringLiteral("accepted")] = event->isAccepted();
      object[QStringLiteral("value")] = sensitive_ ? QString() : text();
      object[QStringLiteral("value_length")] = text().size();
      object[QStringLiteral("cursor")] = cursorPosition();
      object[QStringLiteral("sensitive")] = sensitive_;
      object[QStringLiteral("surrounding_available")] = !no_surrounding_;
      emit_event(object);
    }
  }

protected:
  void showEvent(QShowEvent *event) override {
    QLineEdit::showEvent(event);
    QJsonObject object;
    object[QStringLiteral("event")] = QStringLiteral("text_rectangle");
    object[QStringLiteral("x")] = x();
    object[QStringLiteral("y")] = y();
    object[QStringLiteral("width")] = width();
    object[QStringLiteral("height")] = height();
    object[QStringLiteral("sensitive")] = sensitive_;
    object[QStringLiteral("echo_mode")] =
        sensitive_ ? QStringLiteral("password") : QStringLiteral("normal");
    object[QStringLiteral("input_method_hints")] =
        static_cast<int>(inputMethodHints());
    emit_event(object);
    emit_event(QJsonObject{{QStringLiteral("event"), QStringLiteral("ready")},
                           {QStringLiteral("toolkit"), QStringLiteral("qt6")},
                           {QStringLiteral("sensitive"), sensitive_},
                           {QStringLiteral("echo_mode"),
                            sensitive_ ? QStringLiteral("password")
                                       : QStringLiteral("normal")},
                           {QStringLiteral("surrounding_available"),
                            !no_surrounding_},
                           {QStringLiteral("input_method_hints"),
                            static_cast<int>(inputMethodHints())}});
  }

private:
  bool sensitive_ = false;
  bool no_surrounding_ = false;
};

} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QWidget edgeWindow;
  HarnessEdit edit;
  QWidget *window = &edit;
  if (env_enabled("TIPE_HARNESS_EDGE")) {
    auto *layout = new QVBoxLayout(&edgeWindow);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addStretch();
    auto *row = new QHBoxLayout;
    row->addStretch();
    edit.setFixedWidth(180);
    row->addWidget(&edit);
    layout->addLayout(row);
    window = &edgeWindow;
  }
  window->setWindowTitle(QStringLiteral("TiPE Qt real-app harness"));
  window->resize(640, 100);
  window->show();
  edit.setFocus();
  const QByteArray timeout = qgetenv("TIPE_HARNESS_TIMEOUT_MS");
  const int ms = timeout.isEmpty() ? 10000 : qMax(100, timeout.toInt());
  QTimer::singleShot(ms, &app, &QCoreApplication::quit);
  return app.exec();
}
