#include "playengine_p.hpp"

template<class T>
SIA findEnum(const QString &mpv) -> T
{
    auto items = EnumInfo<T>::items();
    for (auto item : items) {
        if (mpv == item.data.property)
            return item.value;
    }
    return EnumInfo<T>::default_();
}

auto reg_play_engine() -> void
{
    qRegisterMetaType<PlayEngine::State>("State");
    qRegisterMetaType<Mrl>("Mrl");
    qRegisterMetaType<QVector<int>>("QVector<int>");
    qRegisterMetaType<StreamList>("StreamList");
    qRegisterMetaType<AudioFormat>("AudioFormat");
    qmlRegisterType<EditionChapterObject>();
    qmlRegisterType<VideoInfoObject>();
    qmlRegisterType<AvTrackInfoObject>();
    qmlRegisterType<VideoFormatInfoObject>();
    qmlRegisterType<VideoHwAccInfoObject>();
    qmlRegisterType<AudioFormatInfoObject>();
    qmlRegisterType<AudioInfoObject>();
    qmlRegisterType<CodecInfoObject>();
    qmlRegisterType<MediaInfoObject>();
    qmlRegisterType<SubtitleInfoObject>();
    qmlRegisterType<PlayEngine>("bomi", 1, 0, "Engine");
}

class OptionList {
public:
    OptionList(char join = ',')
        : m_join(join) { }
    auto add(const QByteArray &key, const QByteArray &value,
             bool quote = false) -> void
    {
        if (!m_data.isEmpty())
            m_data.append(m_join);
        m_data.append(key);
        m_data.append('=');
        if (quote)
            m_data.append('"');
        m_data.append(value);
        if (quote)
            m_data.append('"');
    }
    auto add(const QByteArray &key, void *value) -> void
        { add(key, address_cast<QByteArray>(value)); }
    auto add(const QByteArray &key, double value) -> void
        { add(key, QByteArray::number(value)); }
    auto add(const QByteArray &key, int value) -> void
        { add(key, QByteArray::number(value)); }
    auto add(const QByteArray &key, bool value) -> void
        { add(key, value ? "yes"_b : "no"_b); }
    auto get() const -> const QByteArray& { return m_data; }
    auto data() const -> const char* { return m_data.data(); }
private:
    QByteArray m_data;
    char m_join;
};

PlayEngine::Data::Data(PlayEngine *engine)
    : p(engine) { }

auto PlayEngine::Data::af(const MrlState *s) const -> QByteArray
{
    OptionList af(':');
    af.add("dummy:address"_b, ac);
    af.add("use_scaler"_b, (int)s->audio_tempo_scaler());
    af.add("layout"_b, (int)s->audio_channel_layout());
    return af.get();
}

auto PlayEngine::Data::vf(const MrlState *s) const -> QByteArray
{
    OptionList vf(':');
    vf.add("noformat:address"_b, vfilter);
    vf.add("swdec_deint"_b, s->d->deint_swdec.toString().toLatin1());
    vf.add("hwdec_deint"_b, s->d->deint_hwdec.toString().toLatin1());
    return vf.get();
}

auto PlayEngine::Data::vo(const MrlState *s) const -> QByteArray
{
    return "opengl-cb:" + videoSubOptions(s);
}

auto PlayEngine::Data::videoSubOptions(const MrlState *s) const -> QByteArray
{
    static const QByteArray shader =
            "const mat4 c_matrix = mat4(__C_MATRIX__); "
            "vec4 custom_shader(vec4 color) { return c_matrix * color; }";
    auto customShader = [&] (const QMatrix4x4 &c_matrix) -> QByteArray {
        QByteArray mat;
        for (int c = 0; c < 4; ++c) {
            mat += "vec4(";
            for (int r = 0; r < 4; ++r) {
                mat += QByteArray::number(c_matrix(r, c), 'e');
                mat += ',';
            }
            mat[mat.size()-1] = ')';
            mat += ',';
        }
        mat.chop(1);
        auto cs = shader;
        cs.replace("__C_MATRIX__", mat);
        return '%' + QByteArray::number(cs.length()) + '%' + cs;
    };

    OptionList opts(':');

    auto addScale = [&] (const char *opt, Interpolator scale) {
        int lanczos = 0;
        switch (scale) {
        case Interpolator::Lanczos2:
            lanczos = 2;
            break;
        case Interpolator::Lanczos3:
            lanczos = 3;
            break;
        case Interpolator::Lanczos4:
            lanczos = 4;
            break;
        default:
            break;
        }
        if (lanczos) {
            opts.add(opt, "lanczos"_b);
            opts.add("scale-radius", lanczos);
        } else
            opts.add(opt, _EnumData(scale));
    };
    addScale("scale", s->video_interpolator());
    addScale("cscale", s->video_chroma_upscaler());
    opts.add("dither-depth", "auto"_b);
    opts.add("dither", _EnumData(s->video_dithering()));
    if (vfilter->isSkipping())
        opts.add("frame-queue-size", 1);
    else
        opts.add("frame-queue-size", 3);
    opts.add("frame-drop-mode", "clear"_b);
    opts.add("fancy-downscaling", s->video_hq_downscaling());
    opts.add("sigmoid-upscaling", s->video_hq_upscaling());
    opts.add("custom-shader", customShader(c_matrix));
    return opts.get();
}

auto PlayEngine::Data::updateColorMatrix() -> void
{
    c_matrix = QMatrix4x4();
    if (params.video_effects() & VideoEffect::Invert)
        c_matrix = QMatrix4x4(-1, 0, 0, 1,
                              0, -1, 0, 1,
                              0, 0, -1, 1,
                              0, 0,  0, 1);
    auto eq = params.video_color();
    if (params.video_effects() & VideoEffect::Gray)
        eq.setSaturation(-100);
    c_matrix *= eq.matrix();
    if (params.video_effects() & VideoEffect::Remap) {
        const float a = 255.0 / (235.0 - 16.0);
        const float b = -16.0 / 255.0 * a;
        c_matrix *= QMatrix4x4(a, 0, 0, b,
                               0, a, 0, b,
                               0, 0, a, b,
                               0, 0, 0, 1);
    }
}

auto PlayEngine::Data::updateVideoSubOptions() -> void
{
    tellmpv_async("vo_cmdline", videoSubOptions(&params));
}

auto PlayEngine::Data::tellmpv(const QByteArray &cmd) -> void
{
    if (handle)
        check(mpv_command_string(handle, cmd.constData()),
              "Cannaot execute: %%", cmd);
}

auto PlayEngine::Data::tellmpv_async(const QByteArray &cmd,
                   std::initializer_list<QByteArray> &&list) -> void
{
    QVector<const char*> args(list.size()+2, nullptr);
    auto it = args.begin();
    *it++ = cmd.constData();
    for (auto &one : list)
        *it++ = one.constData();
    if (handle)
        check(mpv_command_async(handle, 0, args.data()), "Cannot execute: %%", cmd);
}

auto PlayEngine::Data::tellmpv(const QByteArray &cmd,
             std::initializer_list<QByteArray> &&list) -> void
{
    QVector<const char*> args(list.size()+2, nullptr);
    auto it = args.begin();
    *it++ = cmd.constData();
    for (auto &one : list)
        *it++ = one.constData();
    if (handle)
        check(mpv_command(handle, args.data()), "Cannot execute: %%", cmd);
}

auto PlayEngine::Data::loadfile(const Mrl &mrl) -> void
{
    QString file = mrl.isLocalFile() ? mrl.toLocalFile() : mrl.toString();
    if (file.isEmpty())
        return;
    OptionList opts;
    opts.add("pause"_b, p->isPaused() || hasImage);
    tellmpv("loadfile"_b, file.toLocal8Bit(), "replace"_b, opts.get());
}

auto PlayEngine::Data::updateMediaName(const QString &name) -> void
{
    mediaName = name;
    QString category;
    if (mrl.isLocalFile())
        category = tr("File");
    else if (mrl.isDvd())
        category = u"DVD"_q;
    else if (mrl.isBluray())
        category = tr("Blu-ray");
    else
        category = u"URL"_q;
    const QString display = name.isEmpty() ? mrl.displayName() : name;
    info.media.setName(category % ": "_a % display);
}

auto PlayEngine::Data::onLoad() -> void
{
    auto file = getmpv<QString>("stream-open-filename");
    t.local = localCopy();
    auto local = t.local.data();

    Mrl mrl(file);
    local->set_mrl(mrl.toUnique());
    local->d->disc = mrl.isDisc();
    local->set_resume_position(-1);
    local->set_edition(-1);
    local->set_device(QString());
    local->set_video_tracks(StreamList());
    local->set_audio_tracks(StreamList());
    local->set_sub_tracks(StreamList());
    local->set_sub_tracks_inclusive(StreamList());
    const auto found = history->getState(local);
    auto setFiles = [&] (const char *name, const char *nid,
            const StreamList &list) {
        QStringList files; int id = -1;
        for (auto &track : list) {
            if (!track.isExclusive())
                continue;
            if (track.isExternal())
                files.push_back(track.file());
            if (track.isSelected())
                id = track.id();
        }
        if (!files.isEmpty())
            setmpv_async(name, files);
        if (id != -1)
            setmpv_async(nid, id);
    };

    if (found && local->audio_tracks().isValid())
        setFiles("file-local-options/audio-file", "file-local-options/aid", local->audio_tracks());
    else {
        QMutexLocker locker(&mutex);
        setmpv_async("file-local-options/audio-file", autoloadFiles(StreamAudio));
    }
    QVector<SubComp> loads;
    if (found && local->sub_tracks().isValid()) {
        setFiles("file-local-options/sub-file", "file-local-options/sid", local->sub_tracks());
        loads = restoreInclusiveSubtitles(local->sub_tracks_inclusive());
    } else {
        QMutexLocker locker(&mutex);
        QStringList files, encs;
        _R(files, loads) = autoloadSubtitle(local);
        if (!files.isEmpty()) {
            setmpv_async("options/subcp", assEncodings[files.front()].toLatin1());
            setmpv_async("file-local-options/sub-file", files);
        }
    }

    local->set_last_played_date_time(QDateTime::currentDateTime());
    local->set_device(mrl.device());
    local->set_mrl(mrl);

    int edition = -1, start = -1;
    if (resume && mrl.isUnique() && !mrl.isImage()) {
        edition = local->edition();
        start = local->resume_position();
    }
    if (mrl.isDisc()) {
        file = mrl.titleMrl(edition >= 0 ? edition : -1).toString();
        t.start = start;
    } else {
        if (edition >= 0)
            setmpv_async("file-local-options/edition", edition);
        if (start > 0)
            setmpv_async("file-local-options/start", QString::number(start * 1e-3, 'f'));
        t.start = -1;
    }
    const auto deint = local->video_deinterlacing() != DeintMode::None;
    setmpv_async("options/sub-visibility", !local->sub_hidden());
    setmpv_async("options/volume", mpVolume());
    setmpv_async("options/mute", local->audio_muted() ? "yes"_b : "no"_b);
    setmpv_async("options/audio-delay", local->audio_sync() * 1e-3);
    setmpv_async("options/sub-delay", local->sub_sync() * 1e-3);
    setmpv_async("options/audio-channels", ChannelLayoutInfo::data(local->audio_channel_layout()));
    setmpv_async("options/deinterlace", deint ? "yes"_b : "no"_b);
    setmpv_async("options/af-set", af(local));
    setmpv_async("options/vf-set", vf(local));
    setmpv_async("options/vo", vo(local));
    setmpv_async("options/colormatrix", _EnumData(local->video_space()).option);
    setmpv_async("options/colormatrix-input-range", _EnumData(local->video_range()).option);

    const auto cache = local->d->cache.get(mrl);
    t.caching = cache > 0;
    if (t.caching) {
        setmpv_async("file-local-options/cache", cache);
        setmpv_async("file-local-options/cache-initial", local->d->cache.playback(cache));
        setmpv_async("file-local-options/cache-seek-min", local->d->cache.seeking(cache));
    } else
        setmpv_async("file-local-options/cache", "no"_b);


    if (file.startsWith("http://"_a) || file.startsWith("https://"_a)) {
        file = QUrl(file).toString(QUrl::FullyEncoded);
        if (yle && yle->supports(file)) {
            if (yle->run(file))
                setmpv_async("stream-open-filename", yle->url().toLocal8Bit());
        } else if (youtube && youtube->run(file)) {
            setmpv_async("file-local-options/cookies", true);
            setmpv_async("file-local-options/cookies-file", youtube->cookies().toLocal8Bit());
            setmpv_async("file-local-options/user-agent", youtube->userAgent().toLocal8Bit());
            setmpv_async("stream-open-filename", youtube->url().toLocal8Bit());
        } else
            setmpv_async("stream-open-filename", file.toLocal8Bit());
    }
    tellmpv("ignore");
    _PostEvent(p, SyncMrlState, t.local, loads);
    t.local.clear();
}

auto PlayEngine::Data::onFinish() -> void
{
    t.local = localCopy();
    t.local->set_resume_position(position);
    t.local->set_last_played_date_time(QDateTime::currentDateTime());
    t.local->set_edition(edition);
}

auto PlayEngine::Data::hook() -> void
{
    hook("on_load", [=] () { onLoad(); });
    hook("on_finish", [=] () { onFinish(); });
}

auto PlayEngine::Data::observe() -> void
{
    observeType<bool>("pause", [=] (bool p) {
        if (p)
            post(Paused);
        else if (!getmpv<bool>("idle"))
            post(Playing);
    });
    observeType<bool>("core-idle", [=] (bool i) { if (!i) post(Playing); });
    observeType<bool>("paused-for-cache", [=] (bool b) { post(Buffering, b); });
    observeType<bool>("seeking", [=] (bool s) { post(Seeking, s); });

    observe("cache-used", cache.used, [=] () {
        return t.caching ? getmpv<int>("cache-used") : 0;
    }, &PlayEngine::cacheUsedChanged);
    observe("cache-size", cache.size, [=] () {
        return t.caching ? getmpv<int>("cache-size") : 0;
    }, &PlayEngine::cacheSizeChanged);
    observeTime("time-pos", position, &PlayEngine::tick);
    observeTime("time-start", begin, &PlayEngine::beginChanged);
    observeTime("length", duration, &PlayEngine::durationChanged);
    observeTime("avsync", avSync, &PlayEngine::avSyncChanged);
    observe("seekable", seekable, &PlayEngine::seekableChanged);


    observe("chapter-list", [=] () {
        const auto array = getmpv<QVariant>("chapter-list").toList();
        QVector<ChapterData> data(array.size());
        for (int i=0; i<array.size(); ++i) {
            const auto map = array[i].toMap();
            data[i].number = i;
            data[i].time = s2ms(map[u"time"_q].toDouble());
            data[i].name = map[u"title"_q].toString();
            if (data[i].name.isEmpty())
                data[i].name = _MSecToString(data[i].time, u"hh:mm:ss.zzz"_q);
        }
        return data;
    }, [=] (QEvent *event) {
        auto data = _MoveData<QVector<ChapterData>>(event);
        info.chapters.resize(data.size());
        for (int i = 0; i < data.size(); ++i) {
            if (!info.chapters[i].data())
                info.chapters[i]  = ChapterPtr::create();
            info.chapters[i]->set(data[i]);
        }
        emit p->chaptersChanged();
        updateChapter(getmpv<int>("chapter"));
    });
    observeType<int>("chapter", [=] (int n) { updateChapter(n); });
    observe("track-list", [=] () {
        return toTracks(getmpv<QVariant>("track-list"));
    }, [=] (QEvent *event) {
        auto strms = _MoveData<QVector<StreamList>>(event);
        params.set_video_tracks(strms[StreamVideo]);
        params.set_audio_tracks(strms[StreamAudio]);
        params.set_sub_tracks(strms[StreamSubtitle]);
    });

    for (auto type : streamTypes)
        observeType<int>(streams[type].pid,
                         [=] (int id) { params.select(type, id); });
    observe("metadata", metaData, [=] () {
        const auto list = getmpv<QVariant>("metadata").toList();
        MetaData metaData;
        for (int i=0; i+1<list.size(); i+=2) {
            const auto key = list[i].toString();
            const auto value = list[i+1].toString();
            if (key == "title"_a)
                metaData.m_title = value;
            else if (key == "artist"_a)
                metaData.m_artist = value;
            else if (key == "album"_a)
                metaData.m_album = value;
            else if (key == "genre"_a)
                metaData.m_genre = value;
            else if (key == "date"_a)
                metaData.m_date = value;
        }
        metaData.m_mrl = params.mrl();
        metaData.m_duration = s2ms(getmpv<double>("length"));
        return metaData;
    }, &PlayEngine::metaDataChanged);
    observeType<QString>("media-title", [=] (QString &&t)
        { updateMediaName(params.mrl().isYouTube() ? params.mrl().toString() : t); });

    observeType<QString>("video-codec", [=] (QString &&c) { info.video.codec()->parse(c); });
    observeType<double>("fps", [=] (double fps) {
        info.video.input()->setFps(fps);
        info.video.output()->setFps(fps);
        sr->setFPS(fps);
    });
    observeType<int>("width", [=] (int w) {
        auto input = info.video.input();
        input->setWidth(w);
        input->setBppSize(input->size());
    });
    observeType<int>("height", [=] (int h) {
        auto input = info.video.input();
        input->setHeight(h);
        input->setBppSize(input->size());
    });
    observeType<int>("video-bitrate", [=] (int bps) { info.video.input()->setBitrate(bps); });
    observeType<QString>("video-format", [=] (QString &&f) { info.video.input()->setType(f); });
    QRegularExpression rx(uR"(Video decoder: ([^\n]*))"_q);
    auto decoderOutput = [=] (const char *name) -> QString {
        auto m = rx.match(getmpvosd(name));
        if (!m.hasMatch() || m.capturedRef(1) == "unknown"_a)
            return u"Autoselect"_q;
        return m.captured(1);
    };

    auto setParams = [] (VideoFormatInfoObject *info, const QVariantMap &p,
                         const QString &wkey, const QString &hkey) {
        const auto type = p[u"pixelformat"_q].toString();
        const auto w = p[wkey].toInt(), h = p[hkey].toInt();
        info->setType(type);
        info->setSize({w, h});
        info->setBppSize({p[u"w"_q].toInt(), p[u"h"_q].toInt()},
                         p[u"average-bpp"_q].toInt());
        info->setDepth(p[u"plane-depth"_q].toInt());
    };
    observeType<QVariant>("video-params", [=] (QVariant &&var) {
        const auto params = var.toMap();
        auto &video = info.video;
        auto info = video.output();
        setParams(info, params, u"w"_q, u"h"_q);
        info->setRange(findEnum<ColorRange>(decoderOutput("colormatrix-input-range")));
        info->setSpace(findEnum<ColorSpace>(decoderOutput("colormatrix")));
        auto hwState = [&] () {
            if (!hwdec)
                return Deactivated;
            static QVector<QString> types = { u"vaapi"_q, u"vdpau"_q, u"vda"_q };
            const auto codec = video.codec()->type();
            if (!HwAcc::supports(codec))
                return Unavailable;
            if (types.contains(info->type().toLower()))
                return Activated;
            if (!hwcdc.contains(codec.toLatin1()))
                return Unavailable;
            return Deactivated;
        };
        auto hwacc = video.hwacc();
        hwacc->setState(hwState());
        const auto hwdec = getmpv<QString>("hwdec");
        hwacc->setDriver(hwdec == "no"_a ? QString() : hwdec);
    });
    observeType<QVariant>("video-out-params", [=] (QVariant &&var) {
        const auto params = var.toMap();
        auto info = this->info.video.renderer();
        setParams(info, params, u"dw"_q, u"dh"_q);
        info->setRange(findEnum<ColorRange>(params[u"colorlevels"_q].toString()));
        info->setSpace(findEnum<ColorSpace>(params[u"colormatrix"_q].toString()));
    });

    observeType<QString>("audio-codec", [=] (QString &&c) { info.audio.codec()->parse(c); });
    observeType<QString>("audio-format", [=] (QString &&f) { info.audio.input()->setType(f); });
    observeType<int>("audio-bitrate", [=] (int bps) { info.audio.input()->setBitrate(bps); });
    observeType<int>("audio-samplerate", [=] (int s) { info.audio.input()->setSampleRate(s, false); });
    observeType<int>("audio-channels", [=] (int n)
        { info.audio.input()->setChannels(QString::number(n) % "ch"_a, n); });
    observeType<QString>("audio-device", [=] (QString &&d) { info.audio.setDevice(d); });

    for (const auto &ob : observations) {
        if (ob.name)
            mpv_observe_property(handle, ob.event, ob.name, MPV_FORMAT_NONE);
    }
}

auto PlayEngine::Data::dispatch(mpv_event *event) -> void
{
    switch (event->event_id) {
    case MPV_EVENT_LOG_MESSAGE: {
        thread_local QMap<QByteArray, QByteArray> leftmsg;
        auto message = static_cast<mpv_event_log_message*>(event->data);
        const QByteArray prefix(message->prefix);
        auto &left = leftmsg[prefix];
        left.append(message->text);
        int from = 0;
        for (;;) {
            auto to = left.indexOf('\n', from);
            if (to < 0)
                break;
            log(prefix, left.mid(from, to-from));
            from = to + 1;
        }
        left = left.mid(from);
        break;
    } case MPV_EVENT_CLIENT_MESSAGE: {
        auto message = static_cast<mpv_event_client_message*>(event->data);
        if (message->num_args < 1)
            break;
        if (!qstrcmp(message->args[0], "hook_run") && message->num_args == 3) {
            QByteArray when(message->args[2]);
            Q_ASSERT(hooks.contains(when));
            hooks[when]();
            tellmpv("hook_ack", when);
        }
        break;
    } case MPV_EVENT_IDLE:
        break;
    case MPV_EVENT_START_FILE:
        _PostEvent(p, PreparePlayback);
        post(getmpv<bool>("pause") ? Paused : Playing);
        post(Loading, true);
        break;
    case MPV_EVENT_FILE_LOADED: {
        post(getmpv<bool>("pause") ? Paused : Playing);
        post(Loading, false);
        const auto disc = params.mrl().isDisc();
        if (t.start > 0) {
            tellmpv("seek", t.start * 1e-3, 2);
            t.start = -1;
        }
        const char *listprop = disc ? "disc-titles" : "editions";
        const char *itemprop = disc ? "disc-title"  : "edition";
        const int list = getmpv<int>(listprop);
        QVector<EditionData> editions(list);
        auto name = disc ? tr("Title %1") : tr("Edition %1");
        for (int i = 0; i < list; ++i) {
            editions[i].number = i;
            editions[i].name = name.arg(i+1);
        }
        EditionData edition;
        if (list > 0) {
            const int item = getmpv<int>(itemprop);
            if (0 <= item && item < list)
                edition = editions[item];
        }

        auto strs = toTracks(getmpv<QVariant>("track-list"));
        auto select = [&] (StreamType type) {
            for (auto &p : streams[type].priority) {
                const QRegEx rx(p);
                for (auto &str : strs[type]) {
                    auto m = rx.match(str.language());
                    if (m.hasMatch()) {
                        str.m_selected = true;
                        setmpv_async(streams[type].pid, str.id());
                    }
                }
            }
        };
        select(StreamAudio);
        select(StreamSubtitle);
        tellmpv("ignore");
        _PostEvent(p, StartPlayback, editions, edition);
        break;
    } case MPV_EVENT_END_FILE: {
        post(Loading, false);
        auto ev = static_cast<mpv_event_end_file*>(event->data);
        _PostEvent(p, EndPlayback, t.local, ev->reason, ev->error);
        t.local.clear();
        break;
    } case MPV_EVENT_PROPERTY_CHANGE:
        observation(event->reply_userdata).post();
        break;
    case MPV_EVENT_SET_PROPERTY_REPLY:
        if (!isSuccess(event->error)) {
            auto ptr = reinterpret_cast<MpvAsyncData*>(event->reply_userdata);
            _Debug("Error %%: Couldn't set property %%.",
                   mpv_error_string(event->error), ptr->name);
            delete ptr;
        }
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY: {
        break;
    } case MPV_EVENT_SHUTDOWN:
        quit = true;
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        _PostEvent(p, NotifySeek);
        break;
    default:
        break;
    }

}

auto PlayEngine::Data::process(QEvent *event) -> void
{
    const int type = event->type();
    if (UpdateEventBegin <= type && type < updateEventMax) {
        observation(event->type()).handle(event);
        return;
    }
    switch ((int)event->type()) {
     case StateChange:
        updateState(_GetData<PlayEngine::State>(event));
        break;
    case WaitingChange: {
        bool set = false; Waitings waitings = NoWaiting;
        _TakeData(event, waitings, set);
        setWaitings(waitings, set);
        break;
    } case PreparePlayback: {
        break;
    } case StartPlayback: {
        clearTimings();
        QVector<EditionData> editions; EditionData edition;
        _TakeData(event, editions, edition);
        info.editions.resize(editions.size());
        for (int i = 0; i < info.editions.size(); ++i) {
            if (!info.editions[i])
                info.editions[i] = EditionPtr::create();
            info.editions[i]->set(editions[i]);
        }
        info.edition.set(edition);
        emit p->editionsChanged();
        emit p->editionChanged();
        emit p->started(params.mrl());
        break;
    } case EndPlayback: {
        QSharedPointer<MrlState> last; int reason, error;
        _TakeData(event, last, reason, error);
        auto state = Stopped;
        bool eof = false;
        switch ((mpv_end_file_reason)reason) {
        case MPV_END_FILE_REASON_EOF:
            last->set_resume_position(-1);
            eof = true;
            _Info("Playback reached end-of-file");
            break;
        case MPV_END_FILE_REASON_QUIT:
        case MPV_END_FILE_REASON_STOP:
            _Info("Playback has been terminated by request");
            break;
        case MPV_END_FILE_REASON_ERROR:
            _Error("Playback has been terminated by error: %%", mpv_error_string(error));
            state = Error;
            break;
        }
        updateState(state);
        history->update(last.data(), false);
        emit p->finished(last->mrl(), eof);
        break;
    } case NotifySeek:
        emit p->sought();
        break;
    case SyncMrlState: {
        QSharedPointer<MrlState> ms;
        QVector<SubComp> loads;
        _TakeData(event, ms, loads);
        params.m_mutex = nullptr;
        sr->setComponents(loads);
        mutex.lock();
        params.copyFrom(ms.data());
        params.set_sub_tracks_inclusive(sr->toTrackList());
        mutex.unlock();
        params.m_mutex = &mutex;
        history->update(&params, true);
        break;
    } default:
        break;
    }
}

auto PlayEngine::Data::log(const QByteArray &prefix,
                           const QByteArray &text) -> void
{
    Log::write(Log::Info, "[mpv/%%] %%", prefix, text);
    if (text.startsWith("AO: [")) {
        constexpr int from = 5;
        const int to = text.indexOf(']');
        const auto driver = QString::fromLatin1(text.mid(from, to - from));
        QMetaObject::invokeMethod(&info.audio, "setDriver",
                                  Qt::QueuedConnection, Q_ARG(QString, driver));
    }
}

auto PlayEngine::Data::takeSnapshot() -> void
{
    p->clearSnapshots();
    const auto size = displaySize();
    if (size.isEmpty()) {
        emit p->snapshotTaken();
        return;
    }
    OpenGLFramebufferObject fbo(size);
    auto take = [&](bool withOsd) -> QImage {
        QImage image;
        if (withOsd && !params.sub_tracks().isEmpty()) {
            const auto was = getmpv<bool>("sub-visibility");
            if (was != withOsd)
                setmpv("sub-visibility", withOsd);
            render(&fbo);
            if (was != withOsd)
                setmpv("sub-visibility", was);
            return fbo.texture().toImage();
        }
        if (!ss.video.isNull())
            return ss.video;
        render(&fbo);
        return fbo.texture().toImage();
    };
    if (snapshot & VideoOnly)
        ss.video = take(false);
    if (snapshot & VideoWidthOsd)
        ss.screen = take(true);
    emit p->snapshotTaken();
}

auto PlayEngine::Data::renderVideoFrame(OpenGLFramebufferObject *fbo) -> void
{
    const int delay = render(fbo);
    frames.measure.push(++frames.drawn);
    info.video.setDelayedFrames(delay);
    info.video.setDroppedFrames(getmpv<int64_t>("vo-drop-frame-count"));

    _Trace("PlayEngine::Data::renderVideoFrame(): "
           "render queued frame(%%), avgfps: %%",
           fbo->size(), info.video.renderer()->fps());

    if (snapshot) {
        this->takeSnapshot();
        snapshot = NoSnapshot;
    }
}

auto PlayEngine::Data::toTracks(const QVariant &var) -> QVector<StreamList>
{
    QVector<StreamList> streams(3);
    streams[StreamVideo] = { StreamVideo };
    streams[StreamAudio] = { StreamAudio };
    streams[StreamSubtitle] = { StreamSubtitle };
    auto list = var.toList();
    for (auto &var : list) {
        auto track = StreamTrack::fromMpvData(var);
        streams[track.type()].insert(track);
    }
    auto &subs = streams[StreamSubtitle];
    if (!subs.isEmpty()) {
        QMutexLocker locker(&mutex);
        for (auto &track : subs) {
            if (track.isExternal())
                track.m_encoding = assEncodings.take(track.file());
        }
    }
    return streams;
}

auto PlayEngine::Data::restoreInclusiveSubtitles(const StreamList &tracks) -> QVector<SubComp>
{
    Q_ASSERT(tracks.type() == StreamInclusiveSubtitle);
    QVector<SubComp> ret;
    QMap<QString, QMap<QString, SubComp>> subMap;
    for (auto &track : tracks) {
        auto it = subMap.find(track.file());
        if (it == subMap.end()) {
            it = subMap.insert(track.file(), QMap<QString, SubComp>());
            Subtitle sub;
            if (!sub.load(track.file(), track.encoding(), -1))
                continue;
            for (int i = 0; i < sub.size(); ++i)
                it->insert(sub[i].language(), sub[i]);
        }
        auto iit = it->find(track.language());
        if (iit != it->end()) {
            iit->selection() = track.isSelected();
            ret.push_back(*iit);
        }
    }
    return ret;
}

auto PlayEngine::Data::autoloadFiles(StreamType type) -> QStringList
{
    auto &a = streams[type].autoloader;
    if (a.enabled)
        return a.autoload(mrl, streams[type].ext);
    return QStringList();
}

auto PlayEngine::Data::autoselect(const MrlState *s, QVector<SubComp> &loads) -> void
{
    QVector<int> selected;
    QSet<QString> langSet;
    const QFileInfo file(mrl.toLocalFile());
    const QString base = file.completeBaseName();

    for (int i = 0; i<loads.size(); ++i) {
        bool select = false;
        switch (s->d->autoselectMode) {
        case AutoselectMode::Matched: {
            const QFileInfo info(loads[i].fileName());
            select = info.completeBaseName() == base;
            break;
        } case AutoselectMode::EachLanguage: {
            //                  const QString lang = loaded[i].m_comp.language().id();
            const auto lang = loads[i].language();
            if ((select = (!langSet.contains(lang))))
                langSet.insert(lang);
            break;
        }  case AutoselectMode::All:
            select = true;
            break;
        default:
            break;
        }
        if (select)
            selected.append(i);
    }
    if (s->d->autoselectMode == AutoselectMode::Matched
            && !selected.isEmpty() && !s->d->autoselectExt.isEmpty()) {
        for (int i=0; i<selected.size(); ++i) {
            const auto fileName = loads[selected[i]].fileName();
            const auto suffix = QFileInfo(fileName).suffix();
            if (s->d->autoselectExt == suffix.toLower()) {
                const int idx = selected[i];
                selected.clear();
                selected.append(idx);
                break;
            }
        }
    }
    for (int i=0; i<selected.size(); ++i)
        loads[selected[i]].selection() = true;
}

auto PlayEngine::Data::autoloadSubtitle(const MrlState *s) -> T<QStringList, QVector<SubComp>>
{
    const auto subs = autoloadFiles(StreamSubtitle);
    QStringList files;
    QVector<SubComp> loads;
    for (auto &file : subs) {
        const auto enc = s->d->detect(file);
        Subtitle sub;
        if (sub.load(file, enc, -1)) {
            for (int i = 0; i < sub.size(); ++i)
                loads.push_back(sub[i]);
        } else {
            files.push_back(file);
            assEncodings[file] = enc;
        }
    }
    autoselect(s, loads);
    return _T(files, loads);
}

auto PlayEngine::Data::localCopy() -> QSharedPointer<MrlState>
{
    auto s = new MrlState;
    s->blockSignals(true);
    mutex.lock();
    s->copyFrom(&params);
    mutex.unlock();
    return QSharedPointer<MrlState>(s);
}

auto PlayEngine::Data::updateState(State s) -> void
{
    const auto prev = state;
    if (_Change(state, s)) {
        emit p->stateChanged(state);
        auto check = [&] (State s)
            { return !!(state & s) != !!(prev & s); };
        if (check(Paused))
            emit p->pausedChanged();
        if (check(Playing))
            emit p->playingChanged();
        if (check(Stopped))
            emit p->stoppedChanged();
        if (check(Running))
            emit p->runningChanged();
    }
}

auto PlayEngine::Data::setWaitings(Waitings w, bool set) -> void
{
    auto old = p->waiting();
    if (set)
        waitings |= w;
    else
        waitings &= ~w;
    auto new_ = p->waiting();
    if (old != new_)
        emit p->waitingChanged(new_);
}

auto PlayEngine::Data::clearTimings() -> void
{
    frames.measure.reset();
    info.video.setDroppedFrames(0);
    info.video.setDelayedFrames(0);
    info.video.renderer()->setFps(0);
    frames.drawn = 0;
}

auto PlayEngine::Data::sub_add(const QString &file, const QString &enc, bool select) -> void
{
    setmpv_async("options/subcp", enc.toLatin1());
    tellmpv_async("sub_add", file.toLocal8Bit(), select ? "select"_b : "auto"_b);
    mutex.lock();
    assEncodings[file] = enc;
    mutex.unlock();
}
