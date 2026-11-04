#pragma once
#include <QVector>
#include <QWidget>

// QR em painel branco com zona de silêncio de 4 módulos e módulos de pixels inteiros do dispositivo.
// Guarda só a matriz de módulos; o texto codificado não é mantido nem exibido.
class QrCodeView : public QWidget {
    Q_OBJECT
public:
    static constexpr int QuietZone = 4;
    static constexpr int MinimumModulePixels = 4;
    static constexpr int MinimumSide = 240;
    explicit QrCodeView(QWidget *parent = nullptr);
    // Lança std::exception se o texto não couber em um QR.
    void setContent(const QByteArray &utf8);
    void clear();
    bool isEmpty() const { return modules.isEmpty(); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int devicePixelsPerModule() const;
    void resizeToHint();
    int size = 0;
    QVector<bool> modules;
};
