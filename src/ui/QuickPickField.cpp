#include "QuickPickField.h"
#include "domain/HistoryPeriod.h"
#include <QtWidgets>

QuickPickField::QuickPickField(const QString &placeholder, const QString &addText, const Messages &messages, QWidget *parent)
    : QWidget(parent), messages(messages) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(4);
    auto *row = new QHBoxLayout; row->setSpacing(8);
    edit = new QLineEdit; edit->setPlaceholderText(placeholder); edit->setAccessibleName(placeholder);
    edit->setClearButtonEnabled(true); edit->setMaxLength(160);
    add = new QPushButton(addText); add->setAutoDefault(false);
    row->addWidget(edit,1); row->addWidget(add);
    layout->addLayout(row);
    status = new QLabel; status->setObjectName("quickPickStatus"); status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    model = new QStringListModel(this);
    completer = new QCompleter(model, this);
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer->setMaxVisibleItems(8);
    completer->setWidget(edit);
    completer->popup()->setObjectName("quickPickPopup");
    connect(completer, qOverload<const QString &>(&QCompleter::activated), this, &QuickPickField::choose);
    connect(edit, &QLineEdit::textChanged, this, &QuickPickField::resolve);
    connect(edit, &QLineEdit::textEdited, this, [this] {
        refreshSuggestions();
        if (model->rowCount() > 0) completer->complete(); else completer->popup()->hide();
    });
    connect(add, &QPushButton::clicked, this, [this] { emit addRequested(selected ? QString() : typedText()); });
    edit->installEventFilter(this);
    resolve();
}

void QuickPickField::setEntries(const QVector<Entry> &rows) {
    entries = rows;
    resolve();
}

void QuickPickField::select(qint64 id) {
    for (const auto &entry : entries) if (entry.id == id) { choose(entry.label); return; }
}

void QuickPickField::choose(const QString &label) {
    edit->setText(label);
    edit->setCursorPosition(0);
    completer->popup()->hide();
}

QString QuickPickField::typedText() const {
    return edit->text().trimmed();
}

QStringList QuickPickField::suggestions() const {
    const auto needle = selected ? QString() : searchFold(typedText());
    QStringList labels;
    for (const auto &entry : entries) if (needle.isEmpty() || searchFold(entry.label).contains(needle)) labels << entry.label;
    return labels;
}

QStringList QuickPickField::entryNames() const {
    QStringList names;
    for (const auto &entry : entries) names << entry.name;
    return names;
}

bool QuickPickField::validate() {
    if (selected) return true;
    if (typedText().isEmpty()) showStatus(messages.required, "error");
    else if (status->property("tone").toString() == "hint") showStatus(messages.notFound, "error");
    edit->setFocus();
    return false;
}

bool QuickPickField::eventFilter(QObject *watched, QEvent *event) {
    if (watched == edit && !completer->popup()->isVisible()) {
        const bool click = event->type() == QEvent::MouseButtonPress;
        const bool down = event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Down;
        if ((click || down) && edit->isEnabled()) {
            refreshSuggestions();
            if (model->rowCount() > 0) completer->complete();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QuickPickField::resolve() {
    const auto previous = selected;
    selected.reset();
    const auto text = typedText();
    QVector<qint64> sameName;
    for (const auto &entry : entries) {
        if (entry.label == text) { selected = entry.id; break; }
        if (!text.isEmpty() && searchFold(entry.name) == searchFold(text)) sameName << entry.id;
    }
    if (!selected && sameName.size() == 1) selected = sameName.first();
    if (selected) showStatus(messages.found.contains("%1") ? messages.found.arg(*selected) : messages.found, "ok");
    else if (text.isEmpty()) showStatus({}, "hint");
    else if (sameName.size() > 1) showStatus(messages.ambiguous, "error");
    else if (!suggestions().isEmpty()) showStatus(messages.partial, "hint");
    else showStatus(messages.notFound, "error");
    if (previous != selected) emit selectionChanged();
}

void QuickPickField::refreshSuggestions() {
    model->setStringList(suggestions());
}

void QuickPickField::showStatus(const QString &text, const char *tone) {
    status->setText(text);
    status->setProperty("tone", QString::fromLatin1(tone));
    status->style()->unpolish(status); status->style()->polish(status);
    status->setVisible(!text.isEmpty());
}
