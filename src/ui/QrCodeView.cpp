#include "QrCodeView.h"
#include "qrcodegen.hpp"
#include <QImage>
#include <QPainter>
#include <cmath>

QrCodeView::QrCodeView(QWidget *parent) : QWidget(parent) {
    setAccessibleName("QR code para o Google Authenticator");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void QrCodeView::setContent(const QByteArray &utf8) {
    const auto code = qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    size = code.getSize();
    modules.fill(false, size * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) modules[y * size + x] = code.getModule(x, y);
    resizeToHint();
}

void QrCodeView::clear() {
    modules.fill(false); modules.clear(); size = 0;
    resizeToHint();
}

// Dentro de QScrollArea sem widgetResizable, updateGeometry() não redimensiona o widget;
// sem resize explícito o QR ficaria cortado no tamanho que tinha quando estava vazio.
void QrCodeView::resizeToHint() {
    updateGeometry();
    resize(sizeHint());
    update();
}

// Escala inteira em pixels do dispositivo: com 150% cada módulo continua com bordas exatas.
int QrCodeView::devicePixelsPerModule() const {
    const qreal ratio = devicePixelRatioF();
    const int side = size + 2 * QuietZone;
    const int minimum = int(std::ceil(MinimumModulePixels * ratio));
    return std::max(minimum, int(std::ceil(MinimumSide * ratio / side)));
}

QSize QrCodeView::sizeHint() const {
    if (isEmpty()) return {MinimumSide, MinimumSide};
    const int device = (size + 2 * QuietZone) * devicePixelsPerModule();
    const int logical = int(std::ceil(device / devicePixelRatioF()));
    return {logical, logical};
}

void QrCodeView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    if (isEmpty()) return;
    const int scale = devicePixelsPerModule();
    const int side = (size + 2 * QuietZone) * scale;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (modules[y * size + x])
                for (int dy = 0; dy < scale; ++dy) {
                    auto *line = reinterpret_cast<QRgb *>(image.scanLine((y + QuietZone) * scale + dy));
                    for (int dx = 0; dx < scale; ++dx) line[(x + QuietZone) * scale + dx] = qRgb(0, 0, 0);
                }
    image.setDevicePixelRatio(devicePixelRatioF());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QPointF(0, 0), image);
}
