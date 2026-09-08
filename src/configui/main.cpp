#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>
#include <fcitxqtconfiguiplugin.h>
#include <fcitxqtconfiguiwidget.h>

namespace fcitx {

class WeTypeXConfigWidget final : public FcitxQtConfigUIWidget {
  Q_OBJECT

public:
  explicit WeTypeXConfigWidget(QWidget *parent = nullptr)
      : FcitxQtConfigUIWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(
        QStringLiteral("打开 WeTypeX 设置可管理常用语、设备配对、跨设备同步、"
                       "语音、外观和隔空传送。所有输入选项与快捷键仍由当前 "
                       "Fcitx5 配置页保存。"));
    description->setWordWrap(true);
    auto *open = new QPushButton(QStringLiteral("打开 WeTypeX 完整设置"));
    connect(open, &QPushButton::clicked, this, [] {
      QProcess::startDetached(QStringLiteral(WETYPE_SETTINGS), {});
    });
    layout->addWidget(description);
    layout->addWidget(open, 0, Qt::AlignLeft);
    layout->addStretch();
  }

  void load() override {}
  void save() override { Q_EMIT changed(false); }
  QString title() override { return QStringLiteral("WeTypeX 完整设置"); }
  QString icon() override { return QStringLiteral("fcitx5-wetypex"); }
};

class WeTypeXConfigPlugin final : public FcitxQtConfigUIPlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID FcitxQtConfigUIFactoryInterface_iid FILE
                    "wetypexconfig.json")

public:
  explicit WeTypeXConfigPlugin(QObject *parent = nullptr)
      : FcitxQtConfigUIPlugin(parent) {}
  FcitxQtConfigUIWidget *create(const QString &) override {
    return new WeTypeXConfigWidget;
  }
};

} // namespace fcitx

#include "main.moc"
