#include <QApplication>
#include <QByteArray>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("fcitx5-wetypex-transfer"));
  QWidget window;
  window.setWindowTitle(QStringLiteral("WeTypeX 隔空传送"));
  window.setWindowIcon(QIcon::fromTheme(QStringLiteral("fcitx5-wetypex")));
  window.resize(460, 540);
  window.setStyleSheet(QStringLiteral("QWidget{background:#f7f7f7;color:#222}"
                                      "QLabel#hint{color:#8c8c8c;font-size:13px}"));
  auto *layout = new QVBoxLayout(&window);
  layout->setContentsMargins(24, 22, 24, 22);
  layout->setSpacing(12);
  auto *title = new QLabel(QStringLiteral("我的二维码"));
  title->setAlignment(Qt::AlignCenter);
  title->setStyleSheet(QStringLiteral("font-size:18px;font-weight:500"));
  auto *image = new QLabel;
  image->setAlignment(Qt::AlignCenter);
  auto *status = new QLabel(QStringLiteral("正在创建官方传输会话…"));
  status->setObjectName(QStringLiteral("hint"));
  status->setAlignment(Qt::AlignCenter);
  status->setWordWrap(true);
  layout->addWidget(title);
  layout->addWidget(image, 1);
  layout->addWidget(status);

  auto *process = new QProcess(&window);
  QObject::connect(
      process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      &window, [process, status, image](int code, QProcess::ExitStatus state) {
        const auto result =
            QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        const auto png = QByteArray::fromBase64(
            result.value(QStringLiteral("transfer_image")).toString().toLatin1());
        QPixmap qr;
        if (state != QProcess::NormalExit || code ||
            !result.value(QStringLiteral("ok")).toBool() ||
            !qr.loadFromData(png, "PNG")) {
          status->setText(QStringLiteral("无法创建传输会话，请检查配对和网络状态"));
          return;
        }
        image->setPixmap(qr.scaled(390, 390, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
        const int minutes =
            qMax(1, result.value(QStringLiteral("expiration")).toInt() / 60000);
        status->setText(QStringLiteral("请使用手机扫描；传输码将在 %1 分钟后失效")
                            .arg(minutes));
      });
  process->start(QStringLiteral(WETYPE_ACCOUNT_TOOL),
                 {QStringLiteral("p2p-code")});
  window.show();
  const QString screenshot = qEnvironmentVariable("WETYPE_TRANSFER_SCREENSHOT");
  if (!screenshot.isEmpty())
    QTimer::singleShot(7000, &window, [&app, &window, screenshot] {
      window.grab().save(screenshot);
      app.quit();
    });
  return app.exec();
}
