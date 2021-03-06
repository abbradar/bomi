#include "mainwindow_p.hpp"
#include "app.hpp"
#include "misc/trayicon.hpp"
#include "misc/stepactionpair.hpp"
#include "video/kernel3x3.hpp"
#include "video/deintoption.hpp"
#include "video/videoformat.hpp"
#include "audio/audionormalizeroption.hpp"
#include "subtitle/subtitleview.hpp"
#include "subtitle/subtitlemodel.hpp"
#include "subtitle/subtitle_parser.hpp"
#include "dialog/mbox.hpp"
#include "dialog/openmediafolderdialog.hpp"
#include "dialog/subtitlefinddialog.hpp"
#include "avinfoobject.hpp"

MainWindow::Data::Data(MainWindow *p)
    : p(p)
{
    preferences.initialize();
    preferences.load();
}

template<class List>
auto MainWindow::Data::updateListMenu(Menu &menu, const List &list,
                                      int current, const QString &group) -> void
{
    if (group.isEmpty())
        menu.setEnabled(!list.isEmpty());
    if (!list.isEmpty()) {
        menu.g(group)->clear();
        for (auto it = list.begin(); it != list.end(); ++it) {
            auto act = menu.addActionNoKey(it->name(), true, group);
            act->setData(it->id());
            if (current == it->id())
                act->setChecked(true);
        }
    } else if (!group.isEmpty()) // partial in menu
        menu.g(group)->clear();
}

auto MainWindow::Data::initContextMenu() -> void
{
    auto addContextMenu = [this] (Menu &menu) { contextMenu.addMenu(&menu); };
    addContextMenu(menu(u"open"_q));
    contextMenu.addSeparator();
    addContextMenu(menu(u"play"_q));
    addContextMenu(menu(u"video"_q));
    addContextMenu(menu(u"audio"_q));
    addContextMenu(menu(u"subtitle"_q));
    contextMenu.addSeparator();
    addContextMenu(menu(u"tool"_q));
    addContextMenu(menu(u"window"_q));
    contextMenu.addSeparator();
    contextMenu.addAction(menu(u"help"_q)[u"about"_q]);
    contextMenu.addAction(menu[u"exit"_q]);
#ifdef Q_OS_MAC
////    qt_mac_set_dock_menu(&menu);
    QMenuBar *mb = cApp.globalMenuBar();
    qDeleteAll(mb->actions());
    auto addMenuBar = [this, mb] (Menu &menu)
        {mb->addMenu(menu.copied(mb));};
    addMenuBar(menu(u"open"_q));
    addMenuBar(menu(u"play"_q));
    addMenuBar(menu(u"video"_q));
    addMenuBar(menu(u"audio"_q));
    addMenuBar(menu(u"subtitle"_q));
    addMenuBar(menu(u"tool"_q));
    addMenuBar(menu(u"window"_q));
    addMenuBar(menu(u"help"_q));
#endif
}

auto MainWindow::Data::initWidget() -> void
{
    view = new MainQuickView(p);
    auto format = view->requestedFormat();
    if (OpenGLLogger::isAvailable())
        format.setOption(QSurfaceFormat::DebugContext);
    view->setFormat(format);
    view->setPersistentOpenGLContext(true);
    view->setPersistentSceneGraph(true);

    auto widget = createWindowContainer(view, p);
    auto l = new QVBoxLayout;
    l->addWidget(widget);
    l->setMargin(0);
    p->setLayout(l);
    p->setFocusProxy(widget);
    p->setFocus();
//        widget->setFocus();

    subtitleView = new SubtitleView(p);
    p->setAcceptDrops(true);
    p->resize(400, 300);
    p->setMinimumSize(QSize(400, 300));

    connect(view, &QQuickView::sceneGraphInitialized, p, [this] () {
        auto context = view->openglContext();
        if (cApp.isOpenGLDebugLoggerRequested())
            glLogger.initialize(context);
        sgInit = true;
        e.initializeGL(context);
        emit p->sceneGraphInitialized();
    }, Qt::DirectConnection);
    connect(view, &QQuickView::sceneGraphInvalidated, p, [this] () {
        sgInit = false;
        auto context = QOpenGLContext::currentContext();
        glLogger.finalize(context);
        e.finalizeGL(context);
    }, Qt::DirectConnection);
    desktop = cApp.desktop();
    auto reset = [this] () {
        if (!desktop->isVirtualDesktop())
            virtualDesktopSize = QSize();
        else {
            const int count = desktop->screenCount();
            QRect rect = desktop->availableGeometry(0);
            for (int i=1; i<count; ++i)
                rect |= desktop->availableGeometry(i);
            virtualDesktopSize = rect.size();
        }
    };
    connect(desktop, &QDesktopWidget::resized, reset);
    connect(desktop, &QDesktopWidget::screenCountChanged, reset);
    connect(desktop, &QDesktopWidget::workAreaResized, reset);
    reset();
}

auto MainWindow::Data::initEngine() -> void
{
    connect(&e, &PlayEngine::mrlChanged,
            p, [=] (const Mrl &mrl) { updateMrl(mrl); });
    connect(&e, &PlayEngine::stateChanged, p,
            [this] (PlayEngine::State state) {
        stateChanging = true;
        if (state == PlayEngine::Error) {
            waiter.stop();
            showMessageBox(tr("Error!\nCannot open the media."));
        }
        const auto playing = e.isPlaying();
        menu(u"play"_q)[u"pause"_q]->setText(playing ? tr("Pause") : tr("Play"));
        menu(u"video"_q)(u"track"_q).setEnabled(playing);
        menu(u"audio"_q)(u"track"_q).setEnabled(playing);
        menu(u"subtitle"_q)(u"track"_q).setEnabled(playing);
        const auto disable = pref().disable_screensaver
                             && state == PlayEngine::Playing;
        cApp.setScreensaverDisabled(disable);
        updateStaysOnTop();
        stateChanging = false;
    });
    connect(&e, &PlayEngine::waitingChanged, p, [=] (auto w) {
        if (w) {
            waiter.start();
        } else {
            waiter.stop();
            this->showMessageBox(QString());
        }
    });
    connect(&e, &PlayEngine::tick,
            p, [=] (int time) { if (ab.check(time)) e.seek(ab.a()); });

    connect(&e, &PlayEngine::started, p, [=] (const Mrl &mrl) { setOpen(mrl); });
    connect(&e, &PlayEngine::finished, p, [=] (const Mrl &/*mrl*/, bool eof) {
        if (eof) {
            const auto next = playlist.nextMrl();
            if (!next.isEmpty())
                load(next, true);
        }
    });
    connect(e.video()->renderer(), &VideoFormatInfoObject::sizeChanged,
            p, [this] (const QSize &size) {
        if (pref().fit_to_video && !size.isEmpty())
            setVideoSize(size);
    });
}

auto MainWindow::Data::initItems() -> void
{
    connect(&recent, &RecentInfo::openListChanged,
            p, [=] (const QList<Mrl> &list) { updateRecentActions(list); });
    connect(&hider, &QTimer::timeout,
            p, [this] () { view->setCursorVisible(false); });
    connect(&history, &HistoryModel::playRequested,
            p, [this] (const Mrl &mrl) { openMrl(mrl); });
    connect(&playlist, &PlaylistModel::playRequested,
            p, [this] (int row) { openMrl(playlist.at(row)); });
    connect(&playlist, &PlaylistModel::finished, p, [this] () {
        if (menu(u"tool"_q)[u"auto-exit"_q]->isChecked()) p->exit();
        if (menu(u"tool"_q)[u"auto-shutdown"_q]->isChecked()) cApp.shutdown();
    });
    connect(&e, &PlayEngine::subtitleModelsChanged,
            subtitleView, &SubtitleView::setModels);

    auto showSize = [this] {
        const auto num = [] (qreal n) { return _N(qRound(n)); };
        const auto w = num(e.screen()->width()), h = num(e.screen()->height());
        showMessage(w % u'×'_q % h, &pref().osd_theme.message.show_on_resized);
    };
    connect(e.screen(), &QQuickItem::widthChanged, p, showSize);
    connect(e.screen(), &QQuickItem::heightChanged, p, showSize);
}

auto MainWindow::Data::initTimers() -> void
{
    hider.setSingleShot(true);

    waiter.setInterval(500);
    waiter.setSingleShot(true);
    connect(&waiter, &QTimer::timeout, p, [=] () { updateWaitingMessage(); });

    initializer.setSingleShot(true);
    connect(&initializer, &QTimer::timeout,
            p, [=] () { applyPref(); cApp.runCommands(); });
    initializer.start(1);
}

auto MainWindow::Data::updateWaitingMessage() -> void
{
    QString message;
    if (e.isWaiting())
        message = tr("%1 ...\nPlease wait for a while.").arg(e.waitingText());
    showMessageBox(message);
}

auto MainWindow::Data::openWith(const OpenMediaInfo &mode,
                                const QList<Mrl> &mrls) -> void
{
    if (mrls.isEmpty())
        return;
    const auto mrl = mrls.first();
    auto checkAndPlay = [this] (const Mrl &mrl) {
        if (mrl != e.mrl())
            return false;
        if (!e.isPlaying())
            load(mrl);
        return true;
    };
    if (!checkAndPlay(mrl)) {
        Playlist pl;
        switch (mode.behavior) {
        case OpenMediaBehavior::Append:
            pl.append(mrls);
            break;
        case OpenMediaBehavior::ClearAndAppend:
            playlist.clear();
            pl.append(mrls);
            break;
        case OpenMediaBehavior::NewPlaylist:
            playlist.clear();
            pl = generatePlaylist(mrl);
            break;
        }
        playlist.merge(pl);
        load(mrl, mode.start_playback);
        if (!mrl.isDvd())
            recent.stack(mrl);
    }
    if (!p->isVisible())
        p->show();
}

auto MainWindow::Data::setVideoSize(const QSize &video) -> void
{
    if (p->isFullScreen() || p->isMaximized())
        return;
    // patched by Handrake
    const QSizeF screen = screenSize();
    const QSizeF vs(e.screen()->width(), e.screen()->height());
    const QSize size = (p->size() - vs.toSize() + video);
    if (size != p->size()) {
        p->resize(size);
        int dx = 0;
        const int rightDiff = screen.width() - (p->x() + p->width());
        if (rightDiff < 10) {
            if (rightDiff < 0)
                dx = screen.width() - p->x() - size.width();
            else
                dx = p->width() - size.width();
        }
        if (dx) {
            int x = p->x() + dx;
            if (x < 0)
                x = 0;
            p->move(x, p->y());
        }
    }
}

auto MainWindow::Data::trigger(QAction *action) -> void
{
    if (!action)
        return;
    if (view->topLevelItem()->isVisible()) {
        if (unblockedActions.isEmpty()) {
            unblockedActions += menu(u"window"_q).actions();
            qSort(unblockedActions);
        }
        const auto it = qBinaryFind(_C(unblockedActions), action);
        if (it == unblockedActions.cend())
            return;
    }
    action->trigger();
}

auto MainWindow::Data::readyToHideCursor() -> void
{
    if (pref().hide_cursor
            && (p->isFullScreen() || !pref().hide_cursor_fs_only))
        hider.start(pref().hide_cursor_delay_sec * 1000);
    else
        cancelToHideCursor();
}

auto MainWindow::Data::commitData() -> void
{
    static bool first = true;
    if (first) {
        recent.setLastPlaylist(playlist.list());
        recent.setLastMrl(e.mrl());
        e.shutdown();
        if (!p->isFullScreen())
            updateWindowPosState();
        as.playlist_visible = playlist.isVisible();
        as.playlist_shuffled = playlist.isShuffled();
        as.playlist_repeat = playlist.repeat();
        as.history_visible = history.isVisible();
        if (subFindDlg)
            as.sub_find_lang_code = subFindDlg->selectedLangCode();
        as.save();
        e.waitUntilTerminated();
        cApp.processEvents();
        first = false;
    }
}

auto MainWindow::Data::showTimeLine() -> void
{
    if (player && pref().osd_theme.timeline.show_on_seeking)
        QMetaObject::invokeMethod(player, "showTimeLine");
}
auto MainWindow::Data::showMessageBox(const QVariant &msg) -> void
{
    if (player)
        QMetaObject::invokeMethod(player, "showMessageBox", Q_ARG(QVariant, msg));
}
auto MainWindow::Data::showOSD(const QVariant &msg) -> void
{
    if (player)
        QMetaObject::invokeMethod(player, "showOSD", Q_ARG(QVariant, msg));
}

auto MainWindow::Data::updateWindowSizeState() -> void
{
    if (!p->isFullScreen() && !p->isMinimized() && p->isVisible())
        as.win_size = p->size();
}

auto MainWindow::Data::screenSize() const -> QSize
{
    if (desktop->isVirtualDesktop())
        return virtualDesktopSize;
    return desktop->availableGeometry(p).size();
}

auto MainWindow::Data::updateWindowPosState() -> void
{
    if (!p->isFullScreen() && !p->isMinimized() && p->isVisible()) {
        const auto screen = screenSize();
        as.win_pos.rx() = qBound(0.0, p->x()/(double)screen.width(), 1.0);
        as.win_pos.ry() = qBound(0.0, p->y()/(double)screen.height(), 1.0);
    }
}

auto MainWindow::Data::openDir(const QString &dir) -> void
{
    OpenMediaFolderDialog dlg(p);
    dlg.setFolder(dir);
    if (dlg.exec()) {
        const auto list = dlg.playlist();
        if (!list.isEmpty()) {
            playlist.setList(list);
            load(list.first());
            recent.stack(list.first());
        }
    }
}

auto MainWindow::Data::openMrl(const Mrl &mrl) -> void
{
    if (mrl == e.mrl()) {
        if (!e.isRunning())
            load(mrl);
    } else {
        if (playlist.rowOf(mrl) < 0)
            playlist.setList(generatePlaylist(mrl));
        load(mrl);
        if (!mrl.isDvd())
            recent.stack(mrl);
    }
}

auto MainWindow::Data::generatePlaylist(const Mrl &mrl) const -> Playlist
{
    if (!mrl.isLocalFile() || !pref().enable_generate_playlist)
        return Playlist(mrl);
    const auto mode = pref().generate_playlist;
    const QFileInfo file(mrl.toLocalFile());
    const QDir dir = file.dir();
    const auto filter = _ToNameFilter(pref().exclude_images ? VideoExt | AudioExt : MediaExt);
    if (mode == GeneratePlaylist::Folder) {
        Playlist pl;
        const auto files = dir.entryList(filter, QDir::Files, QDir::Name);
        for (int i=0; i<files.size(); ++i)
            pl.push_back(dir.absoluteFilePath(files[i]));
        return pl;
    }
    const auto files = dir.entryInfoList(filter, QDir::Files, QDir::Name);
    const auto fileName = file.fileName();
    Playlist list;
    bool prefix = false, suffix = false;
    auto it = files.cbegin();
    for(; it != files.cend(); ++it) {
        static QRegEx rxs(uR"((\D*)\d+(.*))"_q);
        const auto ms = rxs.match(fileName);
        if (!ms.hasMatch())
            continue;
        static QRegEx rxt(uR"((\D*)\d+(.*))"_q);
        const auto mt = rxt.match(it->fileName());
        if (!mt.hasMatch())
            continue;
        if (!prefix && !suffix) {
            if (ms.capturedRef(1) == mt.capturedRef(1))
                prefix = true;
            else if (ms.capturedRef(2) == mt.capturedRef(2))
                suffix = true;
            else
                continue;
        } else if (prefix) {
            if (ms.capturedRef(1) != mt.capturedRef(1))
                continue;
        } else if (suffix) {
            if (ms.capturedRef(2) != mt.capturedRef(2))
                continue;
        }
        list.append(it->absoluteFilePath());
    }
    if (list.isEmpty())
        return Playlist(mrl);
    return list;
}

auto MainWindow::Data::showMessage(const QString &msg, const bool *force) -> void
{
    if (force) {
        if (!*force)
            return;
    } else if (!pref().osd_theme.message.show_on_action)
        return;
    if (!dontShowMsg)
        showOSD(msg);
}

SIA _SignN(int value, bool sign) -> QString
    { return sign ? _NS(value) : _N(value); }

SIA _SignN(double value, bool sign, int n = 1) -> QString
    { return sign ? _NS(value, n) : _N(value, n); }

void MainWindow::Data::showMessage(const QString &cmd, int value,
                             const QString &unit, bool sign)
{
    showMessage(cmd, _SignN(value, sign) + unit);
}

void MainWindow::Data::showMessage(const QString &cmd, double value,
                             const QString &unit, bool sign)
{
    showMessage(cmd, _SignN(value, sign) + unit);
}

auto MainWindow::Data::applyPref() -> void
{
    preferences.save();
    const Pref &p = preferences;

    youtube.setUserAgent(p.yt_user_agent);
    youtube.setProgram(p.yt_program);
    yle.setProgram(p.yle_program);
    history.setRememberImage(p.remember_image);
    history.setPropertiesToRestore(p.restore_properties);
    SubtitleParser::setMsPerCharactor(p.ms_per_char);
    cApp.setHeartbeat(p.use_heartbeat ? p.heartbeat_command : QString(),
                      p.heartbeat_interval);
    cApp.setMprisActivated(p.use_mpris2);

    menu.retranslate();
    menu.setShortcuts(p.shortcuts);
    menu(u"play"_q)(u"speed"_q).s()->setStep(p.speed_step);
    menu(u"play"_q)(u"seek"_q).s(u"seek1"_q)->setStep(p.seek_step1_sec * 1000);
    menu(u"play"_q)(u"seek"_q).s(u"seek2"_q)->setStep(p.seek_step2_sec * 1000);
    menu(u"play"_q)(u"seek"_q).s(u"seek3"_q)->setStep(p.seek_step3_sec * 1000);
    menu(u"subtitle"_q)(u"position"_q).s()->setStep(p.sub_pos_step);
    menu(u"subtitle"_q)(u"sync"_q).s()->setStep(p.sub_sync_step_sec * 1000);
    menu(u"video"_q)(u"color"_q).s(u"brightness"_q)->setStep(p.brightness_step);
    menu(u"video"_q)(u"color"_q).s(u"contrast"_q)->setStep(p.contrast_step);
    menu(u"video"_q)(u"color"_q).s(u"saturation"_q)->setStep(p.saturation_step);
    menu(u"video"_q)(u"color"_q).s(u"hue"_q)->setStep(p.hue_step);
    menu(u"audio"_q)(u"sync"_q).s()->setStep(p.audio_sync_step_sec * 1000);
    menu(u"audio"_q)(u"volume"_q).s()->setStep(p.volume_step);
    menu(u"audio"_q)(u"amp"_q).s()->setStep(p.amp_step);
    menu.resetKeyMap();

    theme.osd()->set(p.osd_theme);
    theme.playlist()->set(p.playlist_theme);
    reloadSkin();
    if (tray)
        tray->setVisible(p.enable_system_tray);

    auto cache = [&] () {
        CacheInfo cache;
        cache.local = p.cache_local;
        cache.network = p.cache_network;
        cache.disc = p.cache_disc;
        cache.min_playback = p.cache_min_playback / 100.;
        cache.min_seeking = p.cache_min_seeking / 100.;
        cache.remotes = p.network_folders;
        return cache;
    };
    auto conv = [&p] (const DeintCaps &caps) {
        DeintOption option;
        option.method = caps.method();
        option.doubler = caps.doubler();
        if (caps.hwdec()) {
            if (!caps.supports(DeintDevice::GPU)
                    && !caps.supports(DeintDevice::OpenGL))
                return DeintOption();
            if (caps.supports(DeintDevice::GPU)
                    && p.hwdeints.contains(caps.method()))
                option.device = DeintDevice::GPU;
            else
                option.device = DeintDevice::OpenGL;
        } else
            option.device = caps.supports(DeintDevice::OpenGL)
                            ? DeintDevice::OpenGL : DeintDevice::CPU;
        return option;
    };
    const auto chardet = p.sub_enc_autodetection ? -1 : p.sub_enc_accuracy * 1e-2;

    e.lock();
    e.setResume_locked(p.remember_stopped);
    e.setCache_locked(cache());
    e.setPriority_locked(p.audio_priority, p.sub_priority);
    e.setAutoloader_locked(p.audio_autoload, p.sub_autoload_v2);

    e.setHwAcc_locked(p.enable_hwaccel, p.hwaccel_codecs);
    e.setDeintOptions_locked(conv(p.deint_swdec), conv(p.deint_hwdec));

    e.setAudioDevice_locked(p.audio_device);
    e.setVolumeNormalizerOption_locked(p.audio_normalizer);
    e.setChannelLayoutMap_locked(p.channel_manipulation);
    e.setClippingMethod_locked(p.clipping_method);

    e.setSubtitleStyle_locked(p.sub_style);
    e.setAutoselectMode_locked(p.sub_enable_autoselect, p.sub_autoselect, p.sub_ext);
    e.setSubtitleEncoding_locked(p.sub_enc, chardet);
    e.unlock();
    e.reload();
}

auto MainWindow::Data::updateStaysOnTop() -> void
{
    if (p->windowState() & Qt::WindowMinimized)
        return;
    sotChanging = true;
    const auto id = as.win_stays_on_top;
    bool onTop = false;
    if (!p->isFullScreen()) {
        if (id == StaysOnTop::Always)
            onTop = true;
        else if (id == StaysOnTop::None)
            onTop = false;
        else
            onTop = e.isPlaying();
    }
    cApp.setAlwaysOnTop(p, onTop);
    sotChanging = false;
}

auto MainWindow::Data::setVideoSize(double rate) -> void
{
    if (rate < 0) {
        p->setFullScreen(!p->isFullScreen());
    } else {
        if (p->isFullScreen())
            p->setFullScreen(false);
        if (p->isMaximized())
            p->showNormal();
        const QSizeF video = e.videoSizeHint();
        auto area = [] (const QSizeF &s) { return s.width()*s.height(); };
        if (rate == 0.0)
            rate = area(screenSize())*0.15/area(video);
        setVideoSize((video*qSqrt(rate)).toSize());
    }
}

auto MainWindow::Data::load(const Mrl &mrl, bool play) -> void
{
    if (play)
        e.load(mrl);
    else
        e.setMrl(mrl);
}

auto MainWindow::Data::reloadSkin() -> void
{
    player = nullptr;
    view->setSkin(pref().skin_name);
    auto root = view->rootObject();
    if (!root)
        return;
    auto min = root->property("minimumSize").toSize();
    if (min.width() < 400)
        min.rwidth() = 400;
    if (min.height() < 300)
        min.rheight() = 300;
    p->setMinimumSize(min);
    if (p->width() < min.width() || p->height() < min.height())
        p->resize(min);
    if (root->objectName() == "player"_a)
        player = qobject_cast<QQuickItem*>(root);
    if (!player)
        player = root->findChild<QQuickItem*>(u"player"_q);
    if (player) {
        if (auto item = view->findItem(u"playinfo"_q))
            item->setProperty("show", as.playinfo_visible);
        if (auto item = view->findItem(u"logo"_q)) {
            item->setProperty("show", pref().show_logo);
            item->setProperty("color", pref().bg_color);
        }
    }
}

auto MainWindow::Data::updateRecentActions(const QList<Mrl> &list) -> void
{
    Menu &recent = menu(u"open"_q)(u"recent"_q);
    ActionGroup *group = recent.g();
    const int diff = group->actions().size() - list.size();
    if (diff < 0) {
        const auto acts = recent.actions();
        QAction *sprt = acts[acts.size()-2];
        Q_ASSERT(sprt->isSeparator());
        for (int i=0; i<-diff; ++i) {
            QAction *action = new QAction(&recent);
            recent.insertAction(sprt, action);
            recent.g()->addAction(action);
        }
    } else if (diff > 0) {
        auto acts = recent.g()->actions();
        for (int i=0; i<diff; ++i)
            delete acts.takeLast();
    }
    const auto acts = group->actions();
    for (int i=0; i<list.size(); ++i) {
        QAction *act = acts[i];
        act->setData(list[i].location());
        act->setText(list[i].displayName());
        act->setVisible(!list[i].isEmpty());
    }
}

auto MainWindow::Data::updateMrl(const Mrl &mrl) -> void
{
    updateTitle();
    const auto disc = mrl.isDisc();
    playlist.setLoaded(mrl);
    auto action = menu(u"play"_q)[u"disc-menu"_q];
    action->setEnabled(disc);
    action->setVisible(disc);
}

auto MainWindow::Data::updateTitle() -> void
{
    const auto mrl = e.mrl();
    p->setWindowFilePath(QString());
    QString fileName;
    if (!mrl.isEmpty()) {
        if (mrl.isLocalFile()) {
            const QFileInfo file(mrl.toLocalFile());
            filePath = file.absoluteFilePath();
            fileName = file.fileName();
            if (p->isVisible())
                p->setWindowFilePath(filePath);
        } else
            fileName = e.mediaName();
    }
    cApp.setWindowTitle(p, fileName);
}

auto MainWindow::Data::doVisibleAction(bool visible) -> void
{
    this->visible = visible;
    if (visible) {
        if (pausedByHiding && e.isPaused()) {
            e.unpause();
            pausedByHiding = false;
        }
        p->setWindowFilePath(filePath);
#ifndef Q_OS_MAC
        p->setWindowIcon(cApp.defaultIcon());
#endif
    } else {
        const auto &p = pref();
        if (!p.pause_minimized || dontPause)
            return;
        if (!e.isPlaying() || (p.pause_video_only && !e.hasVideo()))
            return;
        pausedByHiding = true;
        e.pause();
    }
}

auto MainWindow::Data::checkWindowState(Qt::WindowStates prev) -> void
{
    prevWinState = prev;
    winState = p->windowState();
    updateWindowSizeState();
    p->setWindowFilePath(filePath);
    dontPause = true;
    p->resetMoving();
    const auto full = p->isFullScreen();
    if (full) {
        cApp.setAlwaysOnTop(p, false);
        p->setVisible(true);
    } else {
        updateStaysOnTop();
        p->setVisible(true);
    }
    readyToHideCursor();
    dontPause = false;
    if (!stateChanging)
        doVisibleAction(winState != Qt::WindowMinimized);
}
