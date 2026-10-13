// مولّد أيقونة التطبيق: يرسمها بدقة 1024 ثم يصغّرها لكل مقاس (المقاسات الصغيرة بلا شبكة لتبقى واضحة).
// البناء: g++ -std=c++17 -fPIC tools/make_icon.cpp -o make_icon $(pkg-config --cflags --libs Qt6Gui)
// التشغيل: QT_QPA_PLATFORM=offscreen ./make_icon <مجلد الإخراج>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <cmath>
#include <cstdio>

static QImage blurred(const QImage &src, int factor)
{
    QImage small = src.scaled(src.width() / factor, src.height() / factor, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    small = small.scaled(small.width() / 2, small.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return small.scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

static QPainterPath headPath()
{
    QPainterPath p;
    p.moveTo(512, 262);
    p.cubicTo(616, 262, 680, 340, 680, 452);
    p.cubicTo(680, 590, 606, 712, 512, 742);
    p.cubicTo(418, 712, 344, 590, 344, 452);
    p.cubicTo(344, 340, 408, 262, 512, 262);
    p.closeSubpath();
    return p;
}

// يرسم الأيقونة على لوحة 1024 منطقية
static void paintIcon(QPainter &g, bool detailed)
{
    const QRectF tile(48, 48, 928, 928);
    QPainterPath squircle;
    squircle.addRoundedRect(tile, 214, 214);

    // الخلفية: كحلي عميق إلى أزرق كهربائي
    QLinearGradient bg(tile.topLeft(), tile.bottomRight());
    bg.setColorAt(0.0, QColor("#2453E6"));
    bg.setColorAt(0.48, QColor("#14267A"));
    bg.setColorAt(1.0, QColor("#0A0F33"));
    g.fillPath(squircle, bg);

    g.save();
    g.setClipPath(squircle);
    // توهج علوي ناعم
    QRadialGradient glowTop(QPointF(330, 160), 620);
    glowTop.setColorAt(0, QColor(120, 170, 255, 120));
    glowTop.setColorAt(1, QColor(120, 170, 255, 0));
    g.fillRect(tile, glowTop);
    // هالة خلف الوجه
    QRadialGradient halo(QPointF(512, 520), 360);
    halo.setColorAt(0, QColor(56, 230, 255, 95));
    halo.setColorAt(0.6, QColor(90, 120, 255, 35));
    halo.setColorAt(1, QColor(90, 120, 255, 0));
    g.fillRect(tile, halo);
    // انعكاس زجاجي في النصف العلوي
    QPainterPath sheen;
    sheen.moveTo(tile.left(), tile.top());
    sheen.lineTo(tile.right(), tile.top());
    sheen.lineTo(tile.right(), 330);
    sheen.cubicTo(700, 470, 320, 380, tile.left(), 520);
    sheen.closeSubpath();
    QLinearGradient sh(0, tile.top(), 0, 520);
    sh.setColorAt(0, QColor(255, 255, 255, 34));
    sh.setColorAt(1, QColor(255, 255, 255, 0));
    g.fillPath(sheen, sh);
    g.restore();

    // حافة داخلية مضيئة
    QLinearGradient rim(tile.topLeft(), tile.bottomRight());
    rim.setColorAt(0, QColor(255, 255, 255, 110));
    rim.setColorAt(0.5, QColor(255, 255, 255, 18));
    rim.setColorAt(1, QColor(255, 255, 255, 40));
    g.setPen(QPen(QBrush(rim), 5));
    g.setBrush(Qt::NoBrush);
    g.drawRoundedRect(tile.adjusted(2.5, 2.5, -2.5, -2.5), 212, 212);

    // طبقة الرسومات المتوهجة
    QImage layer(1024, 1024, QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing);
        const QPainterPath head = headPath();
        QLinearGradient faceGrad(QPointF(380, 260), QPointF(660, 760));
        faceGrad.setColorAt(0, QColor("#7DF9FF"));
        faceGrad.setColorAt(0.55, QColor("#5AA8FF"));
        faceGrad.setColorAt(1, QColor("#B18CFF"));
        const QBrush faceBrush(faceGrad);

        // نقاط المعالم
        QList<QPointF> inner = {
            {440, 468}, {584, 468},              // العينان
            {512, 420}, {512, 340},              // الجبهة
            {512, 560},                          // الأنف
            {462, 640}, {562, 640}, {512, 656},  // الفم
            {420, 560}, {604, 560},              // الوجنتان
            {512, 706},                          // الذقن
            {404, 420}, {620, 420},              // الحاجبان
        };
        QList<QPointF> rimPts;
        const int n = detailed ? 18 : 0;
        for (int i = 0; i < n; ++i)
            rimPts << head.pointAtPercent((i + 0.5) / n);

        if (detailed) {
            // شبكة خفيفة تربط المعالم
            QPen meshPen(QColor(150, 230, 255, 85), 3.2);
            p.setPen(meshPen);
            auto nearest = [&](const QPointF &a, const QList<QPointF> &pts, int k) {
                QList<QPair<double, QPointF>> d;
                for (const QPointF &b : pts) {
                    const double dist = std::hypot(a.x() - b.x(), a.y() - b.y());
                    if (dist > 1)
                        d << qMakePair(dist, b);
                }
                std::sort(d.begin(), d.end(), [](auto x, auto y) { return x.first < y.first; });
                QList<QPointF> out;
                for (int i = 0; i < k && i < d.size(); ++i)
                    out << d[i].second;
                return out;
            };
            for (const QPointF &a : rimPts)
                for (const QPointF &b : nearest(a, inner, 2))
                    p.drawLine(a, b);
            for (const QPointF &a : inner)
                for (const QPointF &b : nearest(a, inner, 3))
                    p.drawLine(a, b);
        }

        // محيط الوجه
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(faceBrush, detailed ? 22 : 40, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(head);

        // العينان والفم
        p.setPen(Qt::NoPen);
        p.setBrush(faceBrush);
        const double eye = detailed ? 20 : 34;
        p.drawEllipse(QPointF(440, 468), eye, eye);
        p.drawEllipse(QPointF(584, 468), eye, eye);
        QPainterPath mouth;
        mouth.moveTo(456, 632);
        mouth.quadTo(512, 676, 568, 632);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(faceBrush, detailed ? 18 : 34, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(mouth);

        if (detailed) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(220, 250, 255, 235));
            for (const QPointF &pt : rimPts)
                p.drawEllipse(pt, 8.5, 8.5);
            for (int i = 2; i < inner.size(); ++i)
                if (i != 5 && i != 6)
                    p.drawEllipse(inner[i], 9.5, 9.5);
        }

        // خط المسح
        const double sy = 548;
        QLinearGradient scan(QPointF(250, sy), QPointF(774, sy));
        scan.setColorAt(0, QColor(80, 240, 255, 0));
        scan.setColorAt(0.5, QColor(160, 252, 255, 255));
        scan.setColorAt(1, QColor(80, 240, 255, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(scan);
        p.drawRoundedRect(QRectF(250, sy - (detailed ? 4 : 9), 524, detailed ? 8 : 18), 4, 4);
    }

    // زوايا التركيز البيضاء
    QImage corners(1024, 1024, QImage::Format_ARGB32_Premultiplied);
    corners.fill(Qt::transparent);
    {
        QPainter p(&corners);
        p.setRenderHint(QPainter::Antialiasing);
        const double a = 196, b = 828, len = detailed ? 128 : 150;
        p.setPen(QPen(QColor(255, 255, 255, 245), detailed ? 34 : 54, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        auto corner = [&](double x, double y, double dx, double dy) {
            QPainterPath c;
            const double r = 46;
            c.moveTo(x, y + dy * len);
            c.lineTo(x, y + dy * r);
            c.quadTo(x, y, x + dx * r, y);
            c.lineTo(x + dx * len, y);
            p.drawPath(c);
        };
        corner(a, a, 1, 1);
        corner(b, a, -1, 1);
        corner(a, b, 1, -1);
        corner(b, b, -1, -1);
    }

    // ظل خفيف للزوايا ثم التوهج ثم الرسم الحاد
    g.save();
    g.setClipPath(squircle);
    g.setOpacity(0.55);
    g.drawImage(QPointF(0, 10), blurred(corners, 6).convertToFormat(QImage::Format_ARGB32_Premultiplied).copy());
    g.restore();
    {
        QImage shadow = blurred(corners, 6);
        QPainter sp(&shadow);
        sp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        sp.fillRect(shadow.rect(), QColor(0, 10, 40, 150));
    }
    g.setOpacity(1.0);
    QImage glow = blurred(layer, 10);
    g.setOpacity(0.95);
    g.drawImage(0, 0, glow);
    g.drawImage(0, 0, glow);
    g.setOpacity(1.0);
    g.drawImage(0, 0, layer);
    g.drawImage(0, 0, corners);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QString out = argc > 1 ? argv[1] : "out";
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512, 1024};
    for (int s : sizes) {
        QImage img(1024, 1024, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter g(&img);
            g.setRenderHint(QPainter::Antialiasing);
            g.setRenderHint(QPainter::SmoothPixmapTransform);
            paintIcon(g, s >= 48);
        }
        QImage scaled = img;
        while (scaled.width() / 2 >= s)
            scaled = scaled.scaled(scaled.width() / 2, scaled.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (scaled.width() != s)
            scaled = scaled.scaled(s, s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled.save(QStringLiteral("%1/icon-%2.png").arg(out).arg(s));
    }
    return 0;
}
