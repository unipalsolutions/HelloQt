#pragma once

#include <QWidget>

class FancyButton;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QTimeEdit;
class SegmentedControl;
class ToggleSwitch;

// أدوات تشغيل الفيديو تحت العرض في سطر واحد: التشغيل والقفز والإطار، الموضع (يُكتب فيه وقت للانتقال)، السرعة والصوت.
// زر «المزيد» يُظهر سطراً ثانياً: الجزء المطلوب (من وإلى)، والتنقل بين الأحداث والعلامات، ومدة القفز.
// الشريط يرسل طلبات فقط، والنافذة تنفذها
class PlaybackBar : public QWidget
{
    Q_OBJECT

public:
    // أنواع الأحداث التي يُتنقل بينها بـ N و Shift+N
    enum class Event { Person, Known, Alert, Shot, Marker };

    explicit PlaybackBar(QWidget *parent = nullptr);

    QTimeEdit *rangeStartEdit() const { return m_rangeStart; }
    QTimeEdit *rangeEndEdit() const { return m_rangeEnd; }

    void setPlaying(bool playing);
    void setPosition(qint64 ms);
    void setDuration(qint64 ms);
    void setFps(double fps);
    double rate() const;
    void setRate(double rate);
    void changeRate(int direction); // درجة أبطأ (-1) أو أسرع (+1)
    qint64 jumpMs() const;          // مدة القفز المختارة لزري القفز
    void setAudioAvailable(bool available);
    int volume() const;
    bool isMuted() const;
    void toggleMute();
    void changeVolume(int delta);
    void setFileNavigation(bool previous, bool next);
    bool autoNext() const;
    void setAnalysisRunning(bool running);
    Event eventKind() const;
    void setEventKind(Event kind);
    void setFullscreen(bool fullscreen);
    void setPanelsHidden(bool hidden);
    void focusGoTo();
    bool toolsVisible() const;
    void setToolsVisible(bool visible);
    // الشاشات القصيرة جداً تطوي السطر الثاني دون أن يتغير اختيار المستخدم؛ «…» يفتحه متى شاء
    void setToolsSuppressed(bool suppressed);

    // «1:23» أو «01:02:03.5» أو «83» ثانية، بالأرقام العربية أو اللاتينية؛ سالب إن لم يُفهم
    static qint64 parseTime(const QString &text);
    // الزمن بأعشار الثانية: 00:01:23.4
    static QString formatPrecise(qint64 ms);

signals:
    void playToggled();
    void jumpRequested(qint64 deltaMs);
    void frameStepRequested(int frames);
    void seekRequested(qint64 ms);
    void rateChanged(double rate);
    void volumeChanged(int volume, bool muted);
    void fileRequested(int direction);
    void autoNextChanged(bool enabled);
    void eventRequested(int direction);
    void markerRequested();
    void rangeStartHere();
    void rangeEndHere();
    void rangeCleared();
    void analyzeRangeRequested();
    void exportRangeRequested();
    void fullscreenRequested();
    void panelsRequested();
    void shortcutsRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    FancyButton *button(int icon, const QString &tip, int width = 34);
    void updateTime();
    void updateVolumeButton();
    void save() const;

    FancyButton *m_play = nullptr;
    FancyButton *m_previousFile = nullptr;
    FancyButton *m_nextFile = nullptr;
    FancyButton *m_back = nullptr;
    FancyButton *m_forward = nullptr;
    FancyButton *m_mute = nullptr;
    FancyButton *m_fullscreen = nullptr;
    FancyButton *m_panels = nullptr;
    FancyButton *m_analyzeRange = nullptr;
    FancyButton *m_more = nullptr;
    QWidget *m_tools = nullptr; // السطر الثاني المطوي
    bool m_toolsPreferred = false;
    QComboBox *m_jump = nullptr;
    SegmentedControl *m_speed = nullptr;
    QComboBox *m_event = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_length = nullptr; // «/ مدة الفيديو» بجانب الموضع
    QLineEdit *m_goTo = nullptr; // الموضع الحالي، ويُكتب فيه وقت للانتقال إليه
    QTimeEdit *m_rangeStart = nullptr;
    QTimeEdit *m_rangeEnd = nullptr;
    ToggleSwitch *m_autoNext = nullptr;
    QWidget *m_audioGroup = nullptr;
    qint64 m_position = 0;
    qint64 m_duration = 0;
    double m_fps = 25.0;
    bool m_muted = false;
};
