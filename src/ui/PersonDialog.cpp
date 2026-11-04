#include "PersonDialog.h"
#include <QtWidgets>

namespace {
QString saveError(const std::exception &e) {
    const auto message=QString::fromUtf8(e.what());
    const QStringList expected={"Nome é obrigatório.","Nome excede o limite de caracteres.","Selecione Professor ou Auxiliar.","Matéria excede o limite de caracteres.",
        "Pessoa não encontrada.","Cadastros inativos não podem ser editados.","Nenhuma alteração para salvar.",
        "Este cadastro foi alterado em outra operação. Atualize a lista e confira os dados antes de tentar novamente."};
    return expected.contains(message) ? message : QStringLiteral("Não foi possível salvar o cadastro. Confira os dados e tente novamente.");
}
}

PersonDialog::PersonDialog(PeopleService &people, std::function<Session()> currentSession, Mode mode,
                           const Person &expected, QWidget *parent) : QDialog(parent) {
    const bool editing=mode==Mode::Edit;
    const QString saveLabel=editing ? QStringLiteral("Salvar alterações") : QStringLiteral("Cadastrar pessoa");
    setObjectName("personDialog"); setWindowTitle(editing ? "Editar pessoa" : "Cadastrar pessoa"); setMinimumWidth(460);
    auto *form=new QFormLayout(this); form->setContentsMargins(24,24,24,24); form->setSpacing(16);
    auto *name=new QLineEdit; name->setObjectName("personName"); name->setMaxLength(120); name->setAccessibleName("Nome *"); form->addRow("Nome *",name);
    auto *role=new QComboBox; role->setObjectName("personRole"); role->setAccessibleName("Função *"); role->addItems({"Selecione a função","Professor","Auxiliar"}); form->addRow("Função *",role);
    auto *subject=new QLineEdit; subject->setObjectName("personSubject"); subject->setMaxLength(120); subject->setAccessibleName("Matéria (opcional)"); subject->setEnabled(false); form->addRow("Matéria (opcional)",subject);
    connect(role,&QComboBox::currentIndexChanged,this,[subject](int index){subject->setEnabled(index==1);});
    if (editing) {name->setText(expected.name); subject->setText(expected.subject); role->setCurrentIndex(expected.role==ResponsibleRole::Professor ? 1 : 2);}
    else name->setText(expected.name.trimmed());
    auto *error=new QLabel; error->setObjectName("errorText"); error->setTextFormat(Qt::PlainText); error->setWordWrap(true); form->addRow(error);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);
    auto *save=buttons->button(QDialogButtonBox::Save); save->setText(saveLabel); save->setObjectName("primary");
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancelar"); form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this,&people,currentSession,editing,expected,name,role,subject,error,save,saveLabel] {
        if (!save->isEnabled()) return;
        if (name->text().trimmed().isEmpty()) {error->setText("Preencha os campos obrigatórios indicados com *."); name->setFocus(); return;}
        if (role->currentIndex()==0) {error->setText("Selecione Professor ou Auxiliar."); role->setFocus(); return;}
        const PersonDraft draft{name->text(),role->currentIndex()==1 ? ResponsibleRole::Professor : ResponsibleRole::Assistant,role->currentIndex()==1 ? subject->text() : QString()};
        save->setEnabled(false);
        try {
            if (!editing) {
                const auto homonyms=people.activeHomonyms(draft.name);
                if (!homonyms.isEmpty()) {
                    const auto &first=homonyms.first();
                    QMessageBox confirm(QMessageBox::Question,"Cadastrar pessoa",QString("Já existe %1 (%2). Cadastrar mesmo assim?").arg(first.name,roleLabel(first.role)),QMessageBox::NoButton,this);
                    confirm.setTextFormat(Qt::PlainText); auto *cancel=confirm.addButton("Cancelar",QMessageBox::RejectRole); auto *accept=confirm.addButton("Cadastrar pessoa",QMessageBox::AcceptRole);
                    confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel); confirm.exec();
                    if (!isVisible()) return;
                    if (confirm.clickedButton()!=accept) {save->setEnabled(true); name->setFocus(); return;}
                }
            }
        } catch (const std::exception &e) {error->setText(saveError(e)); save->setEnabled(true); name->setFocus(); return;}
        save->setText("Salvando…");
        // O contexto cancela a chamada se o diálogo for destruído; um diálogo fechado nunca grava.
        QTimer::singleShot(0,this,[this,&people,currentSession,editing,expected,draft,error,save,saveLabel,name] {
            if (!isVisible()) return;
            try {
                if (editing) {people.update(currentSession(),expected,draft); savedId=expected.id;}
                else savedId=people.create(currentSession(),draft);
                accept();
            } catch (const AuthorizationError &) {
                // Exceções não podem sair de callbacks Qt: relançar somente depois de exec().
                authorizationError=std::current_exception(); reject();
            } catch (const std::exception &e) {
                error->setText(saveError(e)); save->setText(saveLabel); save->setEnabled(true); name->setFocus();
            }
        });
    });
    name->setFocus();
}

int PersonDialog::exec() {
    authorizationError=nullptr;
    const int result=QDialog::exec();
    if (authorizationError) std::rethrow_exception(authorizationError);
    return result;
}
