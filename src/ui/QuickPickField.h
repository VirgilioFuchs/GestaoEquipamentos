#pragma once
#include <QVector>
#include <QWidget>
#include <optional>
class QCompleter;
class QLabel;
class QLineEdit;
class QPushButton;
class QStringListModel;
// Campo de pesquisa rápida sobre um catálogo: sugestões compactas, validação curta e botão de cadastro sempre
// disponível. A seleção é sempre por ID; o texto digitado só identifica um cadastro quando corresponde a exatamente um
// nome.
class QuickPickField : public QWidget {
    Q_OBJECT
public:
    struct Entry {
        qint64 id = 0;
        QString name;
        QString label;
    };
    struct Messages {
        QString found, partial, notFound, ambiguous, required;
    };
    QuickPickField(const QString &placeholder, const QString &addText, const Messages &messages,
                   QWidget *parent = nullptr);
    void setEntries(const QVector<Entry> &entries);
    void select(qint64 id);
    void choose(const QString &label);
    std::optional<qint64> selectedId() const { return selected; }
    QString typedText() const;
    QStringList suggestions() const;
    QStringList entryNames() const;
    bool validate();
    QLineEdit *editor() const { return edit; }
    QPushButton *addButton() const { return add; }
signals:
    void selectionChanged();
    void addRequested(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void resolve();
    void refreshSuggestions();
    void showStatus(const QString &text, const char *tone);
    QVector<Entry> entries;
    Messages messages;
    std::optional<qint64> selected;
    QLineEdit *edit;
    QPushButton *add;
    QLabel *status;
    QCompleter *completer;
    QStringListModel *model;
};
