// النافذة الرئيسية: السجلات والتقارير والنسخ الاحتياطي

#include "mainwindow_p.h"

using namespace MainWindowHelpers;

void MainWindow::showAlertLog()
{
    AlertLogDialog dialog(&m_database, this);
    connect(&dialog, &AlertLogDialog::showPersonRequested, this, &MainWindow::showPerson);
    connect(&dialog, &AlertLogDialog::openFileRequested, this, [this](const QString &path, qint64 positionMs) {
        if (!m_ready || m_videoRunning || m_analyzing) {
            Toast::show(this, QStringLiteral("انتظر حتى ينتهي التحليل الحالي"), Toast::Kind::Warning);
            return;
        }
        m_navGroup->button(0)->click();
        m_lastDirectory = QFileInfo(path).absolutePath();
        openSighting(path, positionMs);
    });
    dialog.exec();
}

void MainWindow::addRecordsMenu(QMenu *menu)
{
    const auto icon = [](Icons::Name name) { return Icons::icon(name, Theme::Text, 18); };
    menu->addAction(icon(Icons::Name::Alert), QStringLiteral("سجل التنبيهات…"), this, [this] {
        showFromTray();
        showAlertLog();
    });
    menu->addAction(icon(Icons::Name::Users), QStringLiteral("الزوار المجهولون…"), this, [this] {
        showFromTray();
        showVisitors();
    });
    menu->addAction(icon(Icons::Name::Film), QStringLiteral("تسجيلات الكاميرات (يوم بيوم)…"), this, [this] {
        showFromTray();
        showRecordings();
    });
    menu->addAction(icon(Icons::Name::Export), QStringLiteral("تقرير يومي…"), this, [this] {
        showFromTray();
        showDailyReport();
    });
    menu->addSeparator();
    menu->addAction(icon(Icons::Name::Info), QStringLiteral("سجل الأحداث (الانقطاع والتسجيل والأعطال)…"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppLog::folder()));
    });
}

void MainWindow::showVisitors()
{
    VisitorsDialog dialog(&m_database, this);
    connect(&dialog, &VisitorsDialog::openFileRequested, this, [this](const QString &path, qint64 positionMs) {
        if (!m_ready || m_videoRunning || m_analyzing) {
            Toast::show(this, QStringLiteral("انتظر حتى ينتهي التحليل الحالي (أو أوقف الكاميرا)"), Toast::Kind::Warning);
            return;
        }
        m_navGroup->button(0)->click();
        m_lastDirectory = QFileInfo(path).absolutePath();
        openSighting(path, positionMs);
    });
    connect(&dialog, &VisitorsDialog::assignRequested, this, [this, &dialog](qint64 visitorId) {
        const Visitor visitor = m_database.visitor(visitorId);
        std::vector<float> embedding = m_database.visitorEmbedding(visitorId);
        if (visitor.id <= 0 || embedding.size() != FaceEngine::EmbeddingSize) {
            Toast::show(&dialog, QStringLiteral("تعذر قراءة بصمة الزائر"), Toast::Kind::Warning);
            return;
        }
        const QString personId = choosePerson(QString());
        if (personId.isEmpty())
            return;
        // البصمة متوسط كل إطارات ظهوره، فهي أثبت من صورة واحدة
        if (!m_database.assignVisitor(visitorId, personId, visitor.image, {embedding}, visitor.quality)) {
            Toast::show(&dialog, QStringLiteral("تعذر نقل الزائر إلى الشخص"), Toast::Kind::Warning);
            return;
        }
        Toast::show(&dialog, QStringLiteral("أصبح الزائر #%1 «%2»، وانتقل ظهوره إلى سجله")
                                 .arg(visitorId)
                                 .arg(m_database.person(personId).name));
        dialog.reload();
    });
    dialog.exec();
}

void MainWindow::openSighting(const QString &path, qint64 positionMs)
{
    if (positionMs < 0 || !isVideoFile(path)) {
        openFile(path);
        return;
    }
    // التحليل يبدأ من لحظة الظهور مباشرة، لا من أول الفيديو (تسجيل الكاميرا قد يطول 15 دقيقة)
    m_rangeStart->setTime(QTime(0, 0).addMSecs(int(positionMs)));
    m_rangeEnd->setTime(QTime(0, 0));
    startVideo(path);
    Toast::show(this, QStringLiteral("ظهر الشخص عند \u2066%1\u2069 من الفيديو؛ التحليل يبدأ منها، وامسح «الجزء: من» لتحليله كله")
                          .arg(ImageIO::formatTime(positionMs)),
                Toast::Kind::Info);
}

void MainWindow::openDatabaseFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_database.directory()));
}

void MainWindow::backupDatabase()
{
    const QString name = QStringLiteral("ملامح-نسخة-احتياطية-%1.malamih").arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("حفظ نسخة احتياطية"), QDir(m_lastDirectory).filePath(name),
                                                      QStringLiteral("نسخ ملامح الاحتياطية (*.malamih)"));
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_database.backup(path, &error);
    QApplication::restoreOverrideCursor();
    if (ok)
        Toast::show(this, QStringLiteral("حُفظت النسخة الاحتياطية (%1 شخص، %2 ميغابايت)")
                              .arg(m_database.personCount())
                              .arg(QFileInfo(path).size() / 1048576.0, 0, 'f', 1));
    else
        QMessageBox::warning(this, QStringLiteral("نسخة احتياطية"), QStringLiteral("تعذر إنشاء النسخة الاحتياطية:\n%1").arg(error));
}

void MainWindow::restoreDatabase()
{
    if (m_analyzing || m_videoRunning) {
        Toast::show(this, QStringLiteral("أوقف التحليل الجاري أولاً"), Toast::Kind::Warning);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("استعادة نسخة احتياطية"), m_lastDirectory,
                                                      QStringLiteral("نسخ ملامح الاحتياطية (*.malamih *.db);;كل الملفات (*)"));
    if (path.isEmpty())
        return;
    m_lastDirectory = QFileInfo(path).absolutePath();
    const QPair<qint64, qint64> counts = FaceDatabase::inspectBackup(path);
    if (counts.first < 0) {
        QMessageBox::warning(this, QStringLiteral("استعادة"), QStringLiteral("هذا الملف ليس نسخة احتياطية صالحة من ملامح."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("استعادة نسخة احتياطية"),
        QStringLiteral("ستُستبدل القاعدة الحالية (%1 شخص، %2 صورة) بالنسخة الاحتياطية (%3 شخص، %4 صورة).\n\n"
                       "تُحفظ القاعدة الحالية بجانبها باسم faces.db.before-restore تحسباً. هل تريد المتابعة؟")
            .arg(m_database.personCount())
            .arg(m_database.photoCount())
            .arg(counts.first)
            .arg(counts.second));
    if (answer != QMessageBox::Yes)
        return;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_database.restore(path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("استعادة"), QStringLiteral("تعذرت الاستعادة، وبقيت القاعدة كما كانت:\n%1").arg(error));
        return;
    }
    refreshMatches();
    Toast::show(this, QStringLiteral("استُعيدت القاعدة: %1 شخص").arg(m_database.personCount()));
}

void MainWindow::showRecordings()
{
    RecordingsDialog dialog(&m_database, m_watched, this);
    connect(&dialog, &RecordingsDialog::openFileRequested, this, [this](const QString &path, qint64 positionMs) {
        if (!m_ready || m_videoRunning || m_analyzing) {
            Toast::show(this, QStringLiteral("انتظر حتى ينتهي التحليل الحالي (أو أوقف الكاميرا)"), Toast::Kind::Warning);
            return;
        }
        m_navGroup->button(0)->click();
        m_lastDirectory = QFileInfo(path).absolutePath();
        openSighting(path, positionMs);
    });
    dialog.exec();
}

void MainWindow::showDailyReport()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("تقرير يومي"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    auto *hint = new QLabel(QStringLiteral("تقرير يوم من الكاميرات: من ظهر ومتى وأمام أي كاميرا، وتنبيهات المراقبة، والزوار المجهولون، "
                                           "وساعات التسجيل، وأحداث اليوم (الانقطاع والأعطال). يُحفظ PDF للطباعة والإرسال أو HTML يُفتح في أي متصفح."),
                            &dialog);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("muted"));
    layout->addWidget(hint);
    auto *row = new QHBoxLayout;
    auto *date = new QDateEdit(QDate::currentDate(), &dialog);
    date->setCalendarPopup(true);
    date->setDisplayFormat(QStringLiteral("yyyy/MM/dd"));
    date->setMaximumDate(QDate::currentDate());
    row->addWidget(new QLabel(QStringLiteral("اليوم:"), &dialog));
    row->addWidget(date);
    row->addStretch(1);
    layout->addLayout(row);
    auto *buttons = new QHBoxLayout;
    auto *pdf = new FancyButton(Icons::Name::Export, QStringLiteral("حفظ PDF…"), FancyButton::Kind::Primary, &dialog);
    auto *html = new FancyButton(Icons::Name::Export, QStringLiteral("حفظ HTML…"), FancyButton::Kind::Secondary, &dialog);
    auto *close = new FancyButton(Icons::Name::Reject, QStringLiteral("إغلاق"), FancyButton::Kind::Secondary, &dialog);
    buttons->addWidget(pdf);
    buttons->addWidget(html);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    const auto save = [this, &dialog, date](const QString &extension) {
        const QDate day = date->date();
        const QString initial = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                    .filePath(QStringLiteral("تقرير-الكاميرات-%1.%2").arg(day.toString(Qt::ISODate), extension));
        const QString path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("حفظ التقرير"), initial,
                                                          extension == QLatin1String("pdf") ? QStringLiteral("PDF (*.pdf)")
                                                                                            : QStringLiteral("HTML (*.html)"));
        if (path.isEmpty())
            return;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = DailyReport::write(path, m_database, m_watched, day, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            Toast::show(&dialog, QStringLiteral("تعذر حفظ التقرير: %1").arg(error), Toast::Kind::Warning);
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        dialog.accept();
    };
    connect(pdf, &QAbstractButton::clicked, &dialog, [save] { save(QStringLiteral("pdf")); });
    connect(html, &QAbstractButton::clicked, &dialog, [save] { save(QStringLiteral("html")); });
    connect(close, &QAbstractButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
}
