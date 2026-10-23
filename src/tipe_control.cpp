#include "tipe/control_window.hpp"
#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>
#include <cstdio>
#include <memory>

int main(int argc,char **argv) {
  QApplication app(argc,argv);
  app.setDesktopFileName("tipe");
  const auto runtime=QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if(runtime.isEmpty())return 1;
  const auto address=runtime+"/tipe-control.sock";
  auto show_existing=[&](int timeout) {
    QLocalSocket socket;
    socket.connectToServer(address);
    if(!socket.waitForConnected(timeout))return false;
    socket.write("show\n");
    if(!socket.waitForReadyRead(1500))return false;
    return socket.readLine()=="shown\n";
  };
  if(show_existing(100))return 0;
  QLockFile lock(address+".lock");
  lock.setStaleLockTime(0);
  if(!lock.tryLock(0)) {
    if(show_existing(1500))return 0;
    std::fputs("TiPE 监督窗口正在启动，请稍后再打开。\n",stderr);
    return 1;
  }
  // Only the lock owner may remove a stale endpoint after an interrupted run.
  QLocalServer::removeServer(address);
  QLocalServer server;
  server.setSocketOptions(QLocalServer::UserAccessOption);
  if(!server.listen(address))return 1;
  auto window=std::unique_ptr<QWidget>(tipe::ui::make_control_window());
  QObject::connect(&server,&QLocalServer::newConnection,&app,[&] {
    while(auto *socket=server.nextPendingConnection()) {
      auto show=[&,socket] {
        if(socket->bytesAvailable()>64) {socket->abort();return;}
        if(!socket->canReadLine())return;
        if(socket->readLine()!="show\n") {socket->abort();return;}
        if(window->isMinimized())window->showNormal();
        else window->show();
        window->raise();window->activateWindow();
        socket->write("shown\n");socket->disconnectFromServer();
      };
      QObject::connect(socket,&QLocalSocket::readyRead,&app,show);
      QObject::connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
      QTimer::singleShot(3000,socket,&QLocalSocket::abort);
      show();
    }
  });
  window->show();
  return app.exec();
}
