#ifndef PLAYENGINE_P_HPP
#define PLAYENGINE_P_HPP

#include "playengine.hpp"
#include "mpv_helper.hpp"
#include "mpv_property.hpp"
#include "mrlstate_p.hpp"
#include "avinfoobject.hpp"
#include "streamtrack.hpp"
#include "historymodel.hpp"
#include "tmp/type_test.hpp"
#include "tmp/type_info.hpp"
#include "misc/autoloader.hpp"
#include "misc/log.hpp"
#include "misc/tmp.hpp"
#include "misc/dataevent.hpp"
#include "misc/youtubedl.hpp"
#include "misc/osdstyle.hpp"
#include "misc/speedmeasure.hpp"
#include "misc/yledl.hpp"
#include "misc/charsetdetector.hpp"
#include "audio/audiocontroller.hpp"
#include "audio/audioformat.hpp"
#include "video/hwacc.hpp"
#include "video/deintoption.hpp"
#include "video/videoformat.hpp"
#include "video/videorenderer.hpp"
#include "video/videoprocessor.hpp"
#include "video/videocolor.hpp"
#include "subtitle/submisc.hpp"
#include "subtitle/subtitle.hpp"
#include "subtitle/subtitlerenderer.hpp"
#include "enum/deintmode.hpp"
#include "enum/colorspace.hpp"
#include "enum/colorrange.hpp"
#include "enum/audiodriver.hpp"
#include "enum/channellayout.hpp"
#include "enum/interpolator.hpp"
#include "enum/autoselectmode.hpp"
#include "enum/dithering.hpp"
#include "opengl/openglframebufferobject.hpp"
#include <libmpv/client.h>
#include <libmpv/opengl_cb.h>
#include <functional>

#ifdef bool
#undef bool
#endif

DECLARE_LOG_CONTEXT(Engine)

enum EventType {
    UserType = QEvent::User, StateChange, WaitingChange,
    PreparePlayback,EndPlayback, StartPlayback, NotifySeek,
    SyncMrlState,
    EventTypeMax
};

enum MpvState {
    MpvStopped, MpvLoading, MpvRunning
};

static constexpr const int UpdateEventBegin = QEvent::User + 1000;

static const QVector<StreamType> streamTypes
    = { StreamAudio, StreamVideo, StreamSubtitle };

struct Observation {
    int event;
    const char *name = nullptr;
    std::function<void(void)> post; // post from mpv to qt
    std::function<void(QEvent*)> handle; // handle posted event
};

extern auto initialize_vdpau() -> void;
extern auto finalize_vdpau() -> void;
extern auto initialize_vaapi() -> void;
extern auto finalize_vaapi() -> void;

class PlayEngine::Thread : public QThread {
public:
    Thread(PlayEngine *engine): engine(engine) {}
private:
    PlayEngine *engine = nullptr;
    auto run() -> void { engine->exec(); }
};


struct StreamData {
    StreamData() = default;
    StreamData(const char *pid, ExtType ext)
        : pid(pid), ext(ext) { }
    const char *pid = nullptr;
    ExtType ext;
    int reserved = -1;
    QStringList priority;
    Autoloader autoloader;
};

SCIA s2ms(double s) -> int { return s*1000 + 0.5; }

struct PlayEngine::Data {
    Data(PlayEngine *engine);
    PlayEngine *p = nullptr;
    PlayEngine::Thread thread{p};

    VideoRenderer *vr = nullptr;
    AudioController *ac = nullptr;
    SubtitleRenderer *sr = nullptr;
    VideoProcessor *vfilter = nullptr;

    PlayEngine::Waitings waitings = PlayEngine::NoWaiting;
    PlayEngine::State state = PlayEngine::Stopped;
    PlayEngine::ActivationState hwacc = PlayEngine::Unavailable;
    PlayEngine::Snapshot snapshot = PlayEngine::NoSnapshot;

    Mrl mrl;
    MrlState params;

    QTemporaryDir confDir;
    QTemporaryFile customShader;
    QVector<Observation> observations;

    struct {
        MediaInfoObject media;
        VideoInfoObject video;
        AudioInfoObject audio;
        SubtitleInfoObject subtitle;
        QVector<EditionChapterPtr> chapters, editions;
        EditionChapterObject chapter, edition;
    } info;

    MetaData metaData;
    QString mediaName;

    HistoryModel *history = nullptr;

    struct {
        bool caching = false;
        int start = -1;
        QSharedPointer<MrlState> local;
    } t; // thread local

    double fps = 1.0;
    bool hasImage = false, seekable = false, hasVideo = false;
    bool pauseAfterSkip = false, resume = false, hwdec = false;
    bool quit = false, initialized = false;

    QByteArray hwcdc;

    struct { int size = 0, used = 0; } cache;

    int avSync = 0;
    int updateEventMax = ::UpdateEventBegin;
    int time_s = 0, begin_s = 0, end_s = 0, duration_s = 0;
    int duration = 0, begin = 0, position = 0, edition = -1;

    mpv_handle *handle = nullptr;
    mpv_opengl_cb_context *gl = nullptr;
    QMatrix4x4 c_matrix;

    YouTubeDL *youtube = nullptr;
    YleDL *yle = nullptr;

    QMap<QString, QString> assEncodings;

    std::array<StreamData, StreamUnknown> streams = []() {
        std::array<StreamData, StreamUnknown> strs;
        strs[StreamVideo] = { "vid", VideoExt };
        strs[StreamAudio] = { "aid", AudioExt };
        strs[StreamSubtitle] = { "sid", SubtitleExt };
        return strs;
    }();


    struct {
        quint64 drawn = 0, dropped = 0, delayed = 0;
        SpeedMeasure<quint64> measure{5, 20};
    } frames;

    struct { QImage screen, video; } ss;
    int hookId = 0;
    QMap<QByteArray, std::function<void(void)>> hooks;
    QPoint mouse;
    QMutex mutex;

    auto updateState(State s) -> void;
    auto setWaitings(Waitings w, bool set) -> void;
    auto clearTimings() -> void;

    auto updateChapter(int n) {
        info.chapter.invalidate();
        for (auto chapter : info.chapters) {
            chapter->setRate(p->rate(chapter->time()));
            if (chapter->number() == n)
                info.chapter.set(chapter->m);
        }
        emit p->chapterChanged();
    }

    auto setInclusiveSubtitles(const QVector<SubComp> &loaded) -> void
        { setInclusiveSubtitles(&params, loaded); }
    auto setInclusiveSubtitles(MrlState *s, const QVector<SubComp> &loaded) -> void
        { sr->setComponents(loaded); s->set_sub_tracks_inclusive(sr->toTrackList()); }
    auto syncInclusiveSubtitles() -> void
        { params.set_sub_tracks_inclusive(sr->toTrackList()); }
    static auto restoreInclusiveSubtitles(const StreamList &tracks) -> QVector<SubComp>;
    auto audio_add(const QString &file, bool select) -> void
        { tellmpv_async("audio_add", file.toLocal8Bit(), select ? "select"_b : "auto"_b); }
    auto sub_add(const QString &file, const QString &enc, bool select) -> void;
    auto autoselect(const MrlState *s, QVector<SubComp> &loads) -> void;
    auto autoloadFiles(StreamType type) -> QStringList;
    auto autoloadSubtitle(const MrlState *s) -> T<QStringList, QVector<SubComp>>;

    auto af(const MrlState *s) const -> QByteArray;
    auto vf(const MrlState *s) const -> QByteArray;
    auto vo(const MrlState *s) const -> QByteArray;
    auto videoSubOptions(const MrlState *s) const -> QByteArray;
    auto updateVideoSubOptions() -> void;
    auto updateColorMatrix() -> void;
    auto renderVideoFrame(OpenGLFramebufferObject *fbo) -> void;
    auto displaySize() const { return info.video.renderer()->size(); }
    auto post(State state) -> void { _PostEvent(p, StateChange, state); }
    auto post(Waitings w, bool set) -> void { _PostEvent(p, WaitingChange, w, set); }
    auto exec() -> void;

    auto mpVolume() const -> double
        { return params.audio_volume() * params.audio_amplifier() * 1e-3; }
    template<class T>
    SIA qbytearray_from(const T &t) -> tmp::enable_if_t<tmp::is_arithmetic<T>(),
        QByteArray> { return QByteArray::number(t); }
    SIA qbytearray_from(const QByteArray &t) -> QByteArray { return t; }
    SIA qbytearray_from(const QString &t) -> QByteArray { return t.toLocal8Bit(); }

    auto tellmpv(const QByteArray &cmd) -> void;
    auto tellmpv_async(const QByteArray &cmd,
                       std::initializer_list<QByteArray> &&list) -> void;
    auto tellmpv(const QByteArray &cmd,
                 std::initializer_list<QByteArray> &&list) -> void;
    template<class... Args>
    auto tellmpv(const QByteArray &cmd, const Args &... args) -> void
        { tellmpv(cmd, {qbytearray_from(args)...}); }
    template<class... Args>
    auto tellmpv_async(const QByteArray &cmd, const Args &... args) -> void
        { tellmpv_async(cmd, {qbytearray_from(args)...}); }

    auto loadfile(const Mrl &mrl) -> void;
    auto updateMediaName(const QString &name = QString()) -> void;
    template <class T>
    auto setmpv_async(const char *name, const T &value) -> void;
    template <class T>
    auto setmpv(const char *name, const T &value) -> void;
    auto setmpv(const char *name, const QString &value) -> void
        { setmpv(name, value.toLocal8Bit()); }
    auto toTracks(const QVariant &var) -> QVector<StreamList>;
    template<class T>
    auto getmpv(const char *name) -> T;
    template<class T>
    auto getmpv(const char *name, T &val) -> bool;
    auto refresh() -> void {tellmpv_async("frame_step"); tellmpv("frame_back_step");}
    static auto error(int err) -> const char* { return mpv_error_string(err); }
    auto isSuccess(int error) -> bool { return error == MPV_ERROR_SUCCESS; }
    template<class T>
    auto post(mpv_event *event, T &val, const T &fallback = T()) -> void;
    template<class... Args>
    auto check(int err, const char *msg, const Args &... args) -> bool;
    template<class... Args>
    auto fatal(int err, const char *msg, const Args &... args) -> void;
    auto getmpvosd(const char *name) -> QString {
        auto buf = mpv_get_property_osd_string(handle, name);
        auto ret = QString::fromLatin1(buf);
        mpv_free(buf);
        return ret;
    }

    auto observation(int event) -> const Observation&
    {
        Q_ASSERT(UpdateEventBegin <= event && event < updateEventMax);
        Q_ASSERT(event == observations[event - UpdateEventBegin].event);
        return observations[event - UpdateEventBegin];
    }
    auto newUpdateEvent() -> int { return updateEventMax++; }
    auto observe(const char *name, std::function<void(void)> &&post) -> int
    {
        const int event = newUpdateEvent();
        Observation ob;
        ob.event = event;
        ob.name = name;
        ob.post = post;
        ob.handle = [] (QEvent*) {};
        observations.append(ob);
        Q_ASSERT(observations.size() == updateEventMax - UpdateEventBegin);
        return event;
    }
    template<class Get>
    auto observe(const char *name, Get get,
                 std::function<void(QEvent*)> &&handle) -> int
    {
        const int event = newUpdateEvent();
        Observation ob;
        ob.event = event;
        ob.name = name;
        ob.post = [=] () { _PostEvent(p, event, get()); };
        ob.handle = handle;
        observations.append(ob);
        Q_ASSERT(observations.size() == updateEventMax - UpdateEventBegin);
        return event;
    }
    template<class T, class Set>
    auto observeType(const char *name, Set set) -> int
        { return observe(name, [=] () { return getmpv<T>(name); }, [=] (QEvent *e) { set(_MoveData<T>(e)); }); }

    template<class T, class Get, class Notify>
    auto observe(const char *name, T &t, Get get, Notify notify) -> int
    {
        return observe<Get>(name, get, [=, &t] (QEvent *e) {
            if (t != _GetData<T>(e)) {
                _TakeData(e, t);
                if (initialized)
                    notify();
            }
        });
    }

    template<class T, class Get>
    auto observe(const char *name, T &t, Get get, void(PlayEngine::*sig)()) -> int
        { return observe<T, Get>(name, t, get, [=] () { emit (p->*sig)(); }); }

    template<class T, class Get, class S>
    auto observe(const char *name, T &t, Get get, void(PlayEngine::*sig)(S)) -> int
        { return observe<T, Get>(name, t, get, [=, &t] () { emit (p->*sig)(t); }); }

    template<class T>
    auto observe(const char *name, T &t, void(PlayEngine::*sig)()) -> int
        { return observe(name, t, [=, &t] () { T val = t; return getmpv<T>(name, val) ? val : T(); }, sig); }
    template<class T, class S>
    auto observe(const char *name, T &t, void(PlayEngine::*sig)(S)) -> int
        { return observe(name, t, [=, &t] () { T val = t; return getmpv<T>(name, val) ? val : T(); }, sig); }
    auto observeTime(const char *name, int &t, void(PlayEngine::*sig)(int)) -> int
        { return observe<int>(name, t, [=, &t] { return s2ms(getmpv<double>(name)); }, sig); }

    auto observe() -> void;
    auto dispatch(mpv_event *event) -> void;
    auto process(QEvent *event) -> void;
    auto log(const QByteArray &prefix, const QByteArray &text) -> void;
    template<class F>
    auto hook(const QByteArray &when, F &&handler) -> void
    {
        Q_ASSERT(!hooks.contains(when));
        tellmpv("hook_add", when, hookId++, 0);
        hooks[when] = std::move(handler);
    }
    auto hook() -> void;
    auto setMousePos(const QPointF &pos)
    {
        if (!handle || !params.d->disc)
            return false;
        if (!_Change(mouse, vr->mapToVideo(pos).toPoint()))
            return false;
        tellmpv("mouse"_b, mouse.x(), mouse.y());
        return true;
    }
    auto render(OpenGLFramebufferObject *fbo) -> int
    {
        int vp[4] = {0, 0, fbo->width(), fbo->height()};
        return mpv_opengl_cb_render(gl, fbo->id(), vp);
    }
    auto takeSnapshot() -> void;
    auto localCopy() -> QSharedPointer<MrlState>;
    auto onLoad() -> void;
    auto onFinish() -> void;

};

template<class T>
auto PlayEngine::Data::post(mpv_event *event, T &val, const T &fb) -> void
{
    Q_ASSERT(event->event_id == MPV_EVENT_PROPERTY_CHANGE);
    const auto ev = static_cast<mpv_event_property*>(event->data);
    T newVal = val;
    if (!getmpv<T>(ev->name, newVal))
        newVal = fb;
    if (_Change(val, newVal))
        _PostEvent(p, event->reply_userdata, val);
}

template <class T>
auto PlayEngine::Data::setmpv_async(const char *name, const T &value) -> void
{
    if (handle) {
        auto async = mpv_format_trait<T>::set_async(value);
        async->name = QByteArray(name);
        check(mpv_set_property_async(handle, (quint64)async, name,
                                     mpv_format_trait<T>::format, async->data_ptr),
              "Error on set_async %%", async->name);
    }
}

template <class T>
auto PlayEngine::Data::setmpv(const char *name, const T &value) -> void
{
    using trait = mpv_format_trait<T>;
    if (handle) {
        mpv_t<T> data;
        trait::set(data, value);
        check(mpv_set_property(handle, name, trait::format, &data),
              "Error on %%=%%", name, value);
        trait::set_free(data);
    }
}

template<class T>
auto PlayEngine::Data::getmpv(const char *name, T &def) -> bool
{
    using trait = mpv_format_trait<T>;
    mpv_t<T> data;
    if (!handle || !check(mpv_get_property(handle, name, trait::format, &data),
                          "Couldn't get property '%%'.", name))
        return false;
    trait::get(def, data);
    trait::get_free(data);
    return true;
}

template<class T>
auto PlayEngine::Data::getmpv(const char *name) -> T
{
    T t = T(); getmpv<T>(name, t); return t;
}

template<class... Args>
auto PlayEngine::Data::check(int err, const char *msg,
                             const Args &... args) -> bool
{
    if (isSuccess(err))
        return true;
    const auto lv = err == MPV_ERROR_PROPERTY_UNAVAILABLE ? Log::Debug
                                                          : Log::Error;
    if (lv <= Log::maximumLevel())
        Log::write(getLogContext(), lv, "%% %%: %%",
                   lv == Log::Debug ? "Debug" : "Error", error(err),
                   Log::parse(msg, args...));
    return false;
}

template<class... Args>
auto PlayEngine::Data::fatal(int err, const char *msg,
                             const Args &... args) -> void
{
    if (!isSuccess(err))
        Log::write(getLogContext(), Log::Fatal, "Error %%: %%", error(err),
                   Log::parse(msg, args...));
}

#endif // PLAYENGINE_P_HPP
