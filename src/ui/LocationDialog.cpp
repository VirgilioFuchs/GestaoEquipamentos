#include "LocationDialog.h"
#include <QtWidgets>

QString locationSaveError(const std::exception &e) {
    const QString message=QString::fromUtf8(e.what());
    if (dynamic_cast<const AuthorizationError *>(&e) || message == "Nome do local é obrigatório." ||
        message == "Nome do local excede o limite de caracteres." || message == "Já existe um local ativo com esse nome." ||
        message == "Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente." ||
        message == "Nenhuma alteração para salvar." || message == "Locais excluídos não podem ser editados." || message == "Local não encontrado." || message == "Este local já foi excluído.") return message;
    return "Não foi possível salvar o cadastro. Confira os dados e tente novamente.";
}

LocationDialog::LocationDialog(LocationService &locations, std::function<Session()> currentSession,
                               const std::optional<Location> &expected, const QString &initialName, QWidget *parent) : QDialog(parent) {
    const QString saveLabel = expected ? QStringLiteral("Salvar alterações") : QStringLiteral("Cadastrar local");
    setObjectName("locationFormDialog"); setWindowTitle(expected ? "Editar local" : "Cadastrar local"); setMinimumWidth(430);
    auto *form = new QFormLayout(this);
    form->setContentsMargins(24,24,24,24); form->setSpacing(16);
    auto *name = new QLineEdit; name->setObjectName("locationName"); name->setMaxLength(120); name->setAccessibleName("Nome do local *"); form->addRow("Nome do local *",name);
    name->setText(expected ? expected->name : initialName.trimmed());
    auto *error = new QLabel; error->setObjectName("errorText"); error->setTextFormat(Qt::PlainText); error->setWordWrap(true); form->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save); save->setText(saveLabel); save->setObjectName("primary");
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar"); form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this,&locations,currentSession,expected,name,error,save,saveLabel] {
        if (!save->isEnabled()) return;
        if (name->text().trimmed().isEmpty()) { error->setText("Preencha os campos obrigatórios indicados com *."); name->setFocus(); return; }
        save->setText("Salvando…"); save->setEnabled(false);
        // O contexto cancela a chamada se o diálogo for destruído; um diálogo fechado nunca grava.
        QTimer::singleShot(0,this,[this,&locations,currentSession,expected,name,error,save,saveLabel] {
            if (!isVisible()) return;
            try {
                if (expected) { locations.rename(currentSession(),*expected,name->text()); savedId=expected->id; }
                else savedId=locations.create(currentSession(),name->text());
                accept();
            } catch (const std::exception &e) {
                const auto *refused=dynamic_cast<const AuthorizationError *>(&e);
                if (refused && !onExpired) { authorizationError=std::current_exception(); reject(); return; }
                error->setText(locationSaveError(e));
                if (refused && refused->reason()==AuthorizationError::Reason::Expired) onExpired();
                save->setText(saveLabel); save->setEnabled(true); name->setFocus();
            }
        });
    });
    name->setFocus();
}

int LocationDialog::exec() {
    authorizationError=nullptr;
    const int result=QDialog::exec();
    if (authorizationError) std::rethrow_exception(authorizationError);
    return result;
}
