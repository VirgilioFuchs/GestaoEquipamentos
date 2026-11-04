#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QScreen>
#include <QStyle>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include "application/DemoSeed.h"
#include "application/LocationService.h"
#include "application/PeopleService.h"
#include "security/SecretVault.h"
#include "ui/AccessDialogs.h"
#include "ui/AccountDialogs.h"
#include "ui/MainWindow.h"
#include <exception>
#include <functional>
#include <memory>

int main(int argc, char *argv[]) {
    QApplication app(argc,argv);
    LocalDatabase::applyApplicationIdentity();
    QCoreApplication::setApplicationVersion("0.1.0");
    // O tema é claro; o modo escuro do sistema não pode escurecer listas e barras de rolagem que o QSS não cobre.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    QApplication::setStyle("Fusion");
    QApplication::setPalette(QApplication::style()->standardPalette());
    QFile style(":/style.qss"); if(style.open(QIODevice::ReadOnly))app.setStyleSheet(QString::fromUtf8(style.readAll()));
    const auto args=app.arguments();
    const bool demo=args.contains("--demo");
    const bool demo150=args.contains("--demo-150");
    if (demo150 && !demo) { qCritical().noquote()<<"A opção --demo-150 exige --demo."; return 1; }
    const int screenshotIndex=args.indexOf("--screenshot");
    const int pageIndex=args.indexOf("--page");
    const QString page=pageIndex>=0 ? args.value(pageIndex+1) : QStringLiteral("equipamentos");
    if (page!="pessoas" && page!="equipamentos" && page!="locais" && page!="logs" && page!="logs-filtros") {
        qCritical().noquote()<<QString("Página desconhecida: %1.").arg(page); return 1;
    }
    if (pageIndex>=0 && screenshotIndex<0) { qCritical().noquote()<<"A opção --page exige --screenshot."; return 1; }
    const auto failure=[&](const QString &title,const std::exception &e) {
        if(screenshotIndex>=0) qCritical().noquote()<<title+": "+QString::fromUtf8(e.what());
        else QMessageBox::critical(nullptr,title,QString::fromUtf8(e.what()));
        return 1;
    };
    QString path;
    try { path=LocalDatabase::profilePath(demo?DataProfile::Demo:DataProfile::Operational); }
    catch(const std::exception &e) { return failure("Erro",e); }
    try {
        LocalDatabase database(path);
        PasswordHasher hasher;
        AuthService auth(database,hasher);
        NotebookStore store(database,auth);
        AccountService accounts(database,auth);
        LocationService locations(database,auth);
        PeopleService people(database,auth);
        KeychainSecretVault vault(vaultServiceName(demo?DataProfile::Demo:DataProfile::Operational));
        RecoveryService recovery(database,auth,vault);
        Session session;
        std::unique_ptr<MainWindow> window;
        bool automaticDemo=demo;
        const auto signIn=[&]() -> bool {
            if(automaticDemo) { automaticDemo=false; session=demo150 ? seedDemoDataset150(auth,store,accounts,DataProfile::Demo,locations,people) : seedDemo(auth,store,accounts,DataProfile::Demo,locations,people); return true; }
            for(;;) {
                while(!auth.hasPrincipal()) { BootstrapPendingDialog pending(auth); if(pending.exec()!=QDialog::Accepted) return false; }
                LoginDialog login(auth,recovery);
                if(login.exec()!=QDialog::Accepted) return false;
                session=login.session();
                if(!session.mustChangePassword()) return true;
                PasswordSetDialog change(auth,session);
                if(change.exec()==QDialog::Accepted) return true;
                auth.logout(session);
            }
        };
        // A janela só é trocada no laço de eventos principal: diálogos dela podem estar terminando em laços aninhados.
        std::function<void(std::function<void()>)> atTopLevel=[&](std::function<void()> step) {
            if(QThread::currentThread()->loopLevel()>1) { QTimer::singleShot(50,&app,[&atTopLevel,step] { atTopLevel(step); }); return; }
            step();
        };
        const auto discardWindow=[&] {
            auto *previous=window.release();
            Session used=previous->activeSession(); auth.logout(used);
            previous->hide(); previous->deleteLater();
        };
        std::function<void()> open;
        const auto restart=[&] { if(signIn()) open(); else QCoreApplication::quit(); };
        open=[&] {
            window=std::make_unique<MainWindow>(store,auth,accounts,recovery,session,path,locations,people);
            QObject::connect(window.get(),&MainWindow::logoutRequested,&app,[&] {
                atTopLevel([&] { discardWindow(); auth.logout(session); restart(); });
            },Qt::QueuedConnection);
            QObject::connect(window.get(),&MainWindow::sessionReplaced,&app,[&](const Session &fresh) {
                atTopLevel([&,fresh] {
                    discardWindow(); auth.logout(session); session=fresh;
                    if(session.mustChangePassword()) {
                        PasswordSetDialog change(auth,session);
                        if(change.exec()!=QDialog::Accepted) { auth.logout(session); restart(); return; }
                    }
                    open();
                });
            });
            const QSize area=window->screen() ? window->screen()->availableGeometry().size() : QSize();
            if (screenshotIndex<0 && area.isValid() && (area.width()<window->width() || area.height()<window->frameGeometry().height())) window->showMaximized();
            else window->show();
            if (screenshotIndex>=0) window->showPage(page);
        };
        if(!signIn()) return 0;
        open();
        if(demo && screenshotIndex>=0 && screenshotIndex+1<args.size()) {
            const auto output=args[screenshotIndex+1];
            QTimer::singleShot(700,window.get(),[&window,output] { QCoreApplication::exit(window->grab().save(output)?0:2); });
        }
        return app.exec();
    } catch(const std::exception &e) { return failure("Erro ao abrir banco",e); }
}
