#pragma once
#include "application/LocationService.h"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
class QTableWidget;
class QLineEdit;
class QComboBox;

class LocationsPage : public QWidget {
public:
    LocationsPage(LocationService &locations, const Session &session, QWidget *parent = nullptr);
    void reload();
    std::function<void()> onExpired;
    std::function<void()> onChanged;

private:
    LocationService &locations;
    const Session &session;
    QTableWidget *table;
    QLabel *feedback;
    QPushButton *create;
    QPushButton *remove;
    QPushButton *edit;
    QLineEdit *search;
    QComboBox *filter;
    QLabel *summary, *state, *emptyTitle, *emptyBody;
    QPushButton *retry;
    int loadGeneration = 0;
    qint64 selectedId = 0, createdId = 0;
    QString savedMessage = QStringLiteral("Local cadastrado.");
    void createLocation();
    void editLocation();
    void saveLocation(const std::optional<Location> &expected);
    void deleteLocation();
    void updateActions();
};
