#include <QApplication>
#include <QMainWindow>
#include <QMessageBox>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>

int main(int argc, char **argv) {
  QApplication app(argc, argv);

  qputenv("SHELL", QByteArray("/bin/bash"));

  auto result = KPluginFactory::loadFactory("konsolepart");
  if (!result) {
    QMessageBox::critical(nullptr, "kerminal", "Failed to load konsolepart.");
    return 1;
  }

  QMainWindow window;
  auto *part = result.plugin->create<KParts::ReadOnlyPart>(&window);
  if (!part) {
    QMessageBox::critical(nullptr, "kerminal", "Failed to create konsole part.");
    return 1;
  }

  window.setCentralWidget(part->widget());
  window.resize(960, 600);
  window.show();

  return app.exec();
}
