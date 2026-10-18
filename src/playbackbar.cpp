#include "playbackbar.h"

#include "controls.h"

#include "icons.h"
#include "widgets.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTime>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

constexpr double Rates[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
constexpr int Jumps[] = {5, 10, 30};

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/settings.ini");
}

QWidget *group(QWidget *parent, std::initializer_list<QWidget *> widgets, int spacing = 6)
{
    auto *box = new QWidget(parent);
    auto *row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(spacing);
    for (QWidget *w : widgets)
        row->addWidget(w);
    return box;
}

} // namespace

PlaybackBar::PlaybackBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(6);

    // ── السطر الأول: التشغيل، من اليسار إلى اليمين كالخط الزمني ──
    auto *transport = new QWidget(this);
    transport->setLayoutDirection(Qt::LeftToRight);
    auto *row = new QHBoxLayout(transport);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    m_previousFile = button(int(Icons::Name::SkipBack), QStringLiteral("الفيديو السابق في المجلد (PageUp)"));
    m_back = button(int(Icons::Name::Rewind), QString());
    auto *frameBack = button(int(Icons::Name::Previous), QStringLiteral("الإطار السابق ( , )"));
    m_play = new FancyButton(Icons::Name::Play, QStringLiteral("تشغيل"), FancyButton::Kind::Primary, this);
    m_play->setCompact(true);
    m_play->setFocusPolicy(Qt::NoFocus);
    m_play->setFixedSize(44, 34);
    m_play->setToolTip(QStringLiteral("تشغيل / إيقاف مؤقت (المسافة أو K)"));
    auto *frameForward = button(int(Icons::Name::Next), QStringLiteral("الإطار التالي ( . )"));
    m_forward = button(int(Icons::Name::Forward), QString());
    m_nextFile = button(int(Icons::Name::SkipNext), QStringLiteral("الفيديو التالي في المجلد (PageDown)"));
    m_jump = new QComboBox(transport);
    for (int seconds : Jumps)
        m_jump->addItem(QStringLiteral("%1 ث").arg(seconds), seconds);
    m_jump->setFocusPolicy(Qt::NoFocus);
    m_jump->setToolTip(QStringLiteral("مدة القفز لزري ⏪ ⏩\nالأسهم ←/→ تقفز 5 ثوانٍ، وJ/L عشراً، وShift مع الأسهم 30 ثانية"));

    // الموضع الحالي في حقل واحد: يُعرض فيه الوقت، ويُكتب فيه وقت آخر ثم Enter للانتقال
    m_goTo = new QLineEdit(transport);
    m_goTo->setObjectName(QStringLiteral("timeField"));
    m_goTo->setFixedWidth(112);
    m_goTo->setAlignment(Qt::AlignCenter);
    m_goTo->setToolTip(QStringLiteral("الموضع الحالي. انقر واكتب وقتاً ثم Enter للانتقال إليه: 1:23 أو 01:02:03 أو 83 (ثانية) — Ctrl+G"));
    m_goTo->installEventFilter(this);
    // لا يُقبل في الحقل إلا ما يكوّن وقتاً: أرقام لاتينية أو عربية وفواصلها، فلا تُكتب فيه حروف بالخطأ
    m_goTo->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9\u0660-\u0669\u06F0-\u06F9:.,\u060C\u061B\u066B ]*")), m_goTo));
    m_length = new QLabel(transport);
    m_length->setObjectName(QStringLiteral("muted"));

    QStringList rateNames;
    for (double rate : Rates)
        rateNames << (rate == 0.25 ? QStringLiteral("¼×") : rate == 0.5 ? QStringLiteral("½×") : QStringLiteral("%1×").arg(rate));
    m_speed = new SegmentedControl(rateNames, transport);
    m_speed->setCurrentIndex(2);
    m_speed->setToolTip(QStringLiteral("سرعة التشغيل (Shift+< أبطأ، Shift+> أسرع). التحليل لا يتأثر بها"));

    m_mute = button(int(Icons::Name::Volume), QStringLiteral("كتم الصوت (M)"));
    m_volume = new QSlider(Qt::Horizontal, transport);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(80);
    m_volume->setFocusPolicy(Qt::NoFocus);
    m_volume->setToolTip(QStringLiteral("مستوى الصوت (↑/↓)"));
    m_audioGroup = group(transport, {m_mute, m_volume}, 2);

    m_panels = button(int(Icons::Name::Panels), QStringLiteral("إخفاء اللوحات الجانبية (P)"));
    m_fullscreen = button(int(Icons::Name::Fullscreen), QStringLiteral("ملء الشاشة (F)"));
    m_more = button(int(Icons::Name::More), QStringLiteral("المزيد: الجزء المطلوب، التنقل بين الأحداث والعلامات، مدة القفز"));
    m_more->setCheckable(true);

    row->addWidget(m_previousFile);
    row->addSpacing(4);
    row->addWidget(m_back);
    row->addWidget(frameBack);
    row->addWidget(m_play);
    row->addWidget(frameForward);
    row->addWidget(m_forward);
    row->addSpacing(4);
    row->addWidget(m_nextFile);
    row->addSpacing(10);
    row->addWidget(m_goTo);
    row->addWidget(m_length);
    row->addStretch(1);
    row->addWidget(m_speed);
    row->addWidget(m_audioGroup);
    row->addSpacing(6);
    row->addWidget(m_more);
    row->addWidget(m_panels);
    row->addWidget(m_fullscreen);
    layout->addWidget(transport);

    // ── السطر الثاني (مطوي): الجزء المطلوب والتنقل بين الأحداث، يلتف في الشاشات الضيقة ──
    auto *tools = new QWidget(this);
    m_tools = tools;
    auto *flow = new FlowLayout(tools, 16, 6);
    flow->setContentsMargins(0, 0, 0, 0);
    auto timeEdit = [tools](const QString &tip) {
        auto *edit = new QTimeEdit(tools);
        setLtrDisplayFormat(edit, QStringLiteral("HH:mm:ss"));
        edit->setTime(QTime(0, 0));
        edit->setToolTip(tip);
        edit->setObjectName(QStringLiteral("timeRange"));
        edit->setButtonSymbols(QAbstractSpinBox::NoButtons);
        edit->setAlignment(Qt::AlignCenter);
        return edit;
    };
    m_rangeStart = timeEdit(QStringLiteral("بداية الجزء المطلوب تحليله أو تصديره"));
    m_rangeEnd = timeEdit(QStringLiteral("نهاية الجزء؛ 00:00:00 = حتى نهاية الفيديو"));
    m_rangeEnd->setSpecialValueText(QStringLiteral("النهاية"));
    auto textButton = [this, tools](Icons::Name icon, const QString &text, const QString &tip) {
        auto *b = new FancyButton(icon, text, FancyButton::Kind::Secondary, tools);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        return b;
    };
    auto *startHere = textButton(Icons::Name::Previous, QStringLiteral("البداية هنا"), QStringLiteral("بداية الجزء عند الموضع الحالي (I)"));
    auto *endHere = textButton(Icons::Name::Next, QStringLiteral("النهاية هنا"), QStringLiteral("نهاية الجزء عند الموضع الحالي (O)"));
    auto *clearRange = button(int(Icons::Name::Stop), QStringLiteral("إلغاء الجزء: الفيديو كله"));
    m_analyzeRange = textButton(Icons::Name::Film, QStringLiteral("تحليل هذا الجزء"), QStringLiteral("تحليل الجزء المحدد وحده (أو الفيديو كله إن لم يُحدد جزء)"));
    auto *exportRange = textButton(Icons::Name::Export, QStringLiteral("تصدير"), QStringLiteral("تصدير الجزء المحدد كملف فيديو"));
    flow->addWidget(group(tools, {new QLabel(QStringLiteral("الجزء: من"), tools), m_rangeStart, new QLabel(QStringLiteral("إلى"), tools),
                                  m_rangeEnd, startHere, endHere, clearRange}));
    flow->addWidget(group(tools, {m_analyzeRange, exportRange}));

    m_event = new QComboBox(tools);
    m_event->addItem(QStringLiteral("ظهور الشخص المحدد"), int(Event::Person));
    m_event->addItem(QStringLiteral("وجه معروف"), int(Event::Known));
    m_event->addItem(QStringLiteral("تنبيه مراقبة"), int(Event::Alert));
    m_event->addItem(QStringLiteral("تغيّر اللقطة"), int(Event::Shot));
    m_event->addItem(QStringLiteral("علامة"), int(Event::Marker));
    m_event->setFocusPolicy(Qt::NoFocus);
    m_event->setToolTip(QStringLiteral("نوع الحدث الذي يُتنقل بينه بالزرين أو بـ N و Shift+N"));
    auto *previousEvent = button(int(Icons::Name::Previous), QStringLiteral("الحدث السابق (Shift+N)"));
    auto *nextEvent = button(int(Icons::Name::Next), QStringLiteral("الحدث التالي (N)"));
    auto *marker = textButton(Icons::Name::Flag, QStringLiteral("علامة"), QStringLiteral("إضافة علامة بملاحظة عند الموضع الحالي (B)"));
    // السابق يمين التالي في الواجهة العربية، والسهمان يشيران إلى اتجاه الزمن في الخط الزمني
    auto *eventsBox = group(tools, {new QLabel(QStringLiteral("انتقال إلى"), tools), m_event, nextEvent, previousEvent, marker});
    flow->addWidget(eventsBox);
    m_autoNext = new ToggleSwitch(QStringLiteral("تشغيل متتالٍ"), tools);
    m_autoNext->setFocusPolicy(Qt::NoFocus);
    m_autoNext->setToolTip(QStringLiteral("بعد انتهاء الفيديو وتحليله يُفتح الفيديو التالي في المجلد ويُحلَّل"));
    flow->addWidget(m_autoNext);
    m_jump->setParent(tools);
    auto *keys = button(int(Icons::Name::Keyboard), QStringLiteral("كل الاختصارات (F1)"));
    keys->setParent(tools);
    flow->addWidget(group(tools, {new QLabel(QStringLiteral("القفز"), tools), m_jump, keys}));
    layout->addWidget(tools);

    // الإعدادات المحفوظة
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    m_volume->setValue(settings.value(QStringLiteral("playback/volume"), 80).toInt());
    m_muted = settings.value(QStringLiteral("playback/muted"), false).toBool();
    const int jump = settings.value(QStringLiteral("playback/jump"), 10).toInt();
    m_jump->setCurrentIndex(std::max(0, m_jump->findData(jump)));
    m_autoNext->setChecked(settings.value(QStringLiteral("playback/autoNext"), false).toBool());
    setEventKind(Event(std::clamp(settings.value(QStringLiteral("playback/event"), 0).toInt(), 0, int(Event::Marker))));
    setToolsVisible(settings.value(QStringLiteral("playback/tools"), false).toBool());
    updateVolumeButton();
    setAudioAvailable(false);
    setPlaying(false);
    updateTime();

    auto updateJumpTips = [this] {
        const int seconds = m_jump->currentData().toInt();
        m_back->setToolTip(QStringLiteral("رجوع %1 ثانية").arg(seconds));
        m_forward->setToolTip(QStringLiteral("تقدم %1 ثانية").arg(seconds));
    };
    updateJumpTips();
    connect(m_jump, &QComboBox::currentIndexChanged, this, [this, updateJumpTips] {
        updateJumpTips();
        save();
    });
    connect(m_play, &QAbstractButton::clicked, this, &PlaybackBar::playToggled);
    connect(m_back, &QAbstractButton::clicked, this, [this] { emit jumpRequested(-jumpMs()); });
    connect(m_forward, &QAbstractButton::clicked, this, [this] { emit jumpRequested(jumpMs()); });
    connect(frameBack, &QAbstractButton::clicked, this, [this] { emit frameStepRequested(-1); });
    connect(frameForward, &QAbstractButton::clicked, this, [this] { emit frameStepRequested(1); });
    connect(m_previousFile, &QAbstractButton::clicked, this, [this] { emit fileRequested(-1); });
    connect(m_nextFile, &QAbstractButton::clicked, this, [this] { emit fileRequested(1); });
    connect(m_goTo, &QLineEdit::returnPressed, this, [this] {
        const qint64 ms = parseTime(m_goTo->text());
        if (ms < 0) {
            m_goTo->selectAll();
            return;
        }
        m_goTo->clearFocus();
        emit seekRequested(m_duration > 0 ? std::min(ms, m_duration - 1) : ms);
    });
    connect(m_speed, &SegmentedControl::currentChanged, this, [this] { emit rateChanged(rate()); });
    connect(m_mute, &QAbstractButton::clicked, this, &PlaybackBar::toggleMute);
    connect(m_volume, &QSlider::valueChanged, this, [this] {
        if (m_muted && m_volume->value() > 0)
            m_muted = false;
        updateVolumeButton();
        save();
        emit volumeChanged(volume(), m_muted);
    });
    connect(m_more, &QAbstractButton::toggled, this, [this](bool on) {
        setToolsVisible(on);
        save();
    });
    connect(m_panels, &QAbstractButton::clicked, this, &PlaybackBar::panelsRequested);
    connect(m_fullscreen, &QAbstractButton::clicked, this, &PlaybackBar::fullscreenRequested);
    connect(keys, &QAbstractButton::clicked, this, &PlaybackBar::shortcutsRequested);
    connect(startHere, &QAbstractButton::clicked, this, &PlaybackBar::rangeStartHere);
    connect(endHere, &QAbstractButton::clicked, this, &PlaybackBar::rangeEndHere);
    connect(clearRange, &QAbstractButton::clicked, this, &PlaybackBar::rangeCleared);
    connect(m_analyzeRange, &QAbstractButton::clicked, this, &PlaybackBar::analyzeRangeRequested);
    connect(exportRange, &QAbstractButton::clicked, this, &PlaybackBar::exportRangeRequested);
    connect(previousEvent, &QAbstractButton::clicked, this, [this] { emit eventRequested(-1); });
    connect(nextEvent, &QAbstractButton::clicked, this, [this] { emit eventRequested(1); });
    connect(marker, &QAbstractButton::clicked, this, &PlaybackBar::markerRequested);
    connect(m_event, &QComboBox::currentIndexChanged, this, [this] { save(); });
    connect(m_autoNext, &QAbstractButton::toggled, this, [this](bool on) {
        save();
        emit autoNextChanged(on);
    });
}

FancyButton *PlaybackBar::button(int icon, const QString &tip, int width)
{
    auto *b = new FancyButton(Icons::Name(icon), tip, FancyButton::Kind::Secondary, this);
    b->setCompact(true);
    b->setFocusPolicy(Qt::NoFocus); // الأسهم والمسافة للتشغيل لا لتنقل التركيز بين الأزرار
    b->setToolTip(tip);
    b->setFixedSize(width, 30);
    return b;
}

void PlaybackBar::save() const
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("playback/volume"), m_volume->value());
    settings.setValue(QStringLiteral("playback/muted"), m_muted);
    settings.setValue(QStringLiteral("playback/jump"), m_jump->currentData().toInt());
    settings.setValue(QStringLiteral("playback/autoNext"), m_autoNext->isChecked());
    settings.setValue(QStringLiteral("playback/event"), m_event->currentData().toInt());
    settings.setValue(QStringLiteral("playback/tools"), toolsVisible());
}

void PlaybackBar::setPlaying(bool playing)
{
    m_play->setIconName(playing ? Icons::Name::Pause : Icons::Name::Play);
    m_play->setText(playing ? QStringLiteral("إيقاف مؤقت") : QStringLiteral("تشغيل"));
}

void PlaybackBar::setPosition(qint64 ms)
{
    m_position = std::max<qint64>(0, ms);
    updateTime();
}

void PlaybackBar::setDuration(qint64 ms)
{
    m_duration = std::max<qint64>(0, ms);
    updateTime();
}

void PlaybackBar::setFps(double fps)
{
    m_fps = fps > 1.0 ? fps : 25.0;
    updateTime();
}

void PlaybackBar::updateTime()
{
    // لا يُستبدل ما يكتبه المستخدم أثناء الكتابة
    if (!m_goTo->hasFocus())
        m_goTo->setText(formatPrecise(m_position));
    // أعشار الثانية تُحذف من المدة إلا في المقاطع القصيرة جداً كي لا تظهر مدتها صفراً
    const QString length = formatPrecise(m_duration);
    m_length->setText(QStringLiteral("/ %1").arg(m_duration > 0 && m_duration < 10000 ? length : length.chopped(2)));
    m_length->setToolTip(QStringLiteral("مدة الفيديو · الإطار الحالي %1").arg(qint64(std::floor(double(m_position) * m_fps / 1000.0 + 1e-6))));
}

bool PlaybackBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_goTo) {
        if (event->type() == QEvent::FocusIn)
            QTimer::singleShot(0, m_goTo, &QLineEdit::selectAll);
        else if (event->type() == QEvent::FocusOut)
            QTimer::singleShot(0, this, &PlaybackBar::updateTime);
    }
    return QWidget::eventFilter(watched, event);
}

bool PlaybackBar::toolsVisible() const
{
    return m_toolsPreferred;
}

void PlaybackBar::setToolsSuppressed(bool suppressed)
{
    const bool visible = !suppressed && m_toolsPreferred;
    m_tools->setVisible(visible);
    const QSignalBlocker blocker(m_more);
    m_more->setChecked(visible);
}

void PlaybackBar::setToolsVisible(bool visible)
{
    m_toolsPreferred = visible;
    m_tools->setVisible(visible);
    if (m_more->isChecked() != visible) {
        const QSignalBlocker blocker(m_more);
        m_more->setChecked(visible);
    }
}

double PlaybackBar::rate() const
{
    return Rates[std::clamp(m_speed->currentIndex(), 0, int(std::size(Rates)) - 1)];
}

void PlaybackBar::setRate(double rate)
{
    int best = 0;
    for (int i = 0; i < m_speed->count(); ++i) {
        if (std::abs(Rates[i] - rate) < std::abs(Rates[best] - rate))
            best = i;
    }
    m_speed->setCurrentIndex(best);
}

void PlaybackBar::changeRate(int direction)
{
    m_speed->setCurrentIndex(std::clamp(m_speed->currentIndex() + (direction > 0 ? 1 : -1), 0, m_speed->count() - 1));
}

qint64 PlaybackBar::jumpMs() const
{
    return qint64(m_jump->currentData().toInt()) * 1000;
}

void PlaybackBar::setAudioAvailable(bool available)
{
    m_audioGroup->setVisible(available);
}

int PlaybackBar::volume() const
{
    return m_volume->value();
}

bool PlaybackBar::isMuted() const
{
    return m_muted || m_volume->value() == 0;
}

void PlaybackBar::toggleMute()
{
    m_muted = !m_muted;
    if (!m_muted && m_volume->value() == 0)
        m_volume->setValue(50);
    updateVolumeButton();
    save();
    emit volumeChanged(volume(), isMuted());
}

void PlaybackBar::changeVolume(int delta)
{
    m_muted = false;
    m_volume->setValue(std::clamp(m_volume->value() + delta, 0, 100));
    updateVolumeButton();
    emit volumeChanged(volume(), isMuted());
}

void PlaybackBar::updateVolumeButton()
{
    const bool muted = isMuted();
    m_mute->setIconName(muted ? Icons::Name::Muted : Icons::Name::Volume);
    m_mute->setToolTip(muted ? QStringLiteral("إلغاء الكتم (M)") : QStringLiteral("كتم الصوت (M)"));
}

void PlaybackBar::setFileNavigation(bool previous, bool next)
{
    // زرا الملف السابق والتالي يظهران فقط إن كان في المجلد فيديوهات أخرى
    m_previousFile->setEnabled(previous);
    m_nextFile->setEnabled(next);
    m_previousFile->setVisible(previous || next);
    m_nextFile->setVisible(previous || next);
}

bool PlaybackBar::autoNext() const
{
    return m_autoNext->isChecked();
}

void PlaybackBar::setAnalysisRunning(bool running)
{
    m_analyzeRange->setEnabled(!running);
    m_rangeStart->setEnabled(!running);
    m_rangeEnd->setEnabled(!running);
}

PlaybackBar::Event PlaybackBar::eventKind() const
{
    return Event(m_event->currentData().toInt());
}

void PlaybackBar::setEventKind(Event kind)
{
    m_event->setCurrentIndex(std::max(0, m_event->findData(int(kind))));
}

void PlaybackBar::setFullscreen(bool fullscreen)
{
    m_fullscreen->setToolTip(fullscreen ? QStringLiteral("الخروج من ملء الشاشة (F أو Esc)") : QStringLiteral("ملء الشاشة (F)"));
}

void PlaybackBar::setPanelsHidden(bool hidden)
{
    m_panels->setToolTip(hidden ? QStringLiteral("إظهار اللوحات الجانبية (P)") : QStringLiteral("إخفاء اللوحات الجانبية (P)"));
}

void PlaybackBar::focusGoTo()
{
    m_goTo->setFocus();
    m_goTo->selectAll();
}

qint64 PlaybackBar::parseTime(const QString &text)
{
    QString value = text.trimmed();
    // الأرقام العربية والفاصلة العربية
    for (QChar &c : value) {
        if (c >= QChar(0x0660) && c <= QChar(0x0669))
            c = QChar('0' + (c.unicode() - 0x0660));
        else if (c >= QChar(0x06F0) && c <= QChar(0x06F9))
            c = QChar('0' + (c.unicode() - 0x06F0));
        else if (c == QChar(0x066B) || c == QLatin1Char(','))
            c = QLatin1Char('.');
        else if (c == QChar(0x061B) || c == QChar(0x060C))
            c = QLatin1Char(':');
    }
    static const QRegularExpression pattern(QStringLiteral("^(?:(\\d+):)?(?:(\\d+):)?(\\d+(?:\\.\\d+)?)$"));
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch())
        return -1;
    // «د:ث» أو «س:د:ث» أو ثوانٍ وحدها
    double hours = 0.0, minutes = 0.0;
    if (match.hasCaptured(2)) {
        hours = match.captured(1).toDouble();
        minutes = match.captured(2).toDouble();
    } else if (match.hasCaptured(1)) {
        minutes = match.captured(1).toDouble();
    }
    const double seconds = match.captured(3).toDouble();
    return qint64(std::llround(((hours * 60.0 + minutes) * 60.0 + seconds) * 1000.0));
}

QString PlaybackBar::formatPrecise(qint64 ms)
{
    ms = std::max<qint64>(0, ms);
    const qint64 seconds = ms / 1000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000 / 100);
}
