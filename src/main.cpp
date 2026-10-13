#include "faceengine.h"
#include "imageio.h"
#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QScreen>
#include <QStyle>
#include <QTextStream>

#include <algorithm>

// HelloQt --self-test <صورة1> <صورة2> <ملف النتائج>
// فحص بلا واجهة: يسجّل أكبر وجه في الصورة الأولى كما في قاعدة البيانات،
// ثم يكتب أعلى تشابه بينه وبين وجوه الصورة الثانية
static int runSelfTest(const QString &first, const QString &second, const QString &outputPath)
{
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text))
        return 2;
    QTextStream out(&output);

    FaceEngine engine;
    QString error;
    if (!engine.load(FaceEngine::defaultModelsDir(), &error)) {
        out << "error=" << error << "\n";
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    const std::vector<AnalyzedFace> a = engine.analyze(ImageIO::loadImage(first), true);
    const std::vector<AnalyzedFace> b = engine.analyze(ImageIO::loadImage(second), true);
    out << "faces_first=" << a.size() << "\n";
    out << "faces_second=" << b.size() << "\n";
    if (a.empty() || b.empty())
        return 1;

    const auto largest = std::max_element(a.begin(), a.end(), [](const AnalyzedFace &x, const AnalyzedFace &y) {
        return x.detection.box.area() < y.detection.box.area();
    });
    const std::vector<std::vector<float>> templates = engine.enrollmentEmbeddings(largest->aligned);
    float best = -1.0f;
    for (const AnalyzedFace &face : b) {
        for (const std::vector<float> &t : templates)
            best = std::max(best, FaceEngine::cosine(t, face.embedding));
    }
    out << "best_similarity=" << best << "\n";
    out << "elapsed_ms=" << timer.elapsed() << "\n";
    return 0;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HelloQt"));
    QApplication::setLayoutDirection(Qt::RightToLeft);
    Theme::apply(app);
    qRegisterMetaType<FaceItem>();
    qRegisterMetaType<QList<FaceItem>>();

    const QStringList args = app.arguments();
    if (args.size() >= 5 && args.at(1) == QLatin1String("--self-test"))
        return runSelfTest(args.at(2), args.at(3), args.at(4));

    MainWindow window;
    const QRect available = app.primaryScreen()->availableGeometry();
    const QSize size = QSize(1280, 800).boundedTo(available.size() * 0.95);
    window.setGeometry(QStyle::alignedRect(Qt::RightToLeft, Qt::AlignCenter, size, available));
    window.show();
    if (args.size() == 2 && QFile::exists(args.at(1)))
        window.openFile(args.at(1));

    return app.exec();
}
