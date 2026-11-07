#include "MpvItem.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

// State shared between the item (GUI thread) and its renderer (render thread).
// The renderer keeps this alive, so mpv outlives the render context even if the
// item is destroyed first.
struct MpvCore
{
    mpv_handle *mpv = nullptr;
    mpv_render_context *render = nullptr; // created/freed by the renderer, GL context current

    std::mutex mutex; // guards `item` for use from mpv's callback threads
    MpvItem *item = nullptr;

    ~MpvCore()
    {
        if (render)
            qWarning() << "mpv render context was not freed before shutdown";
        if (mpv)
            mpv_terminate_destroy(mpv);
    }
};

namespace {

void onMpvWakeup(void *ctx)
{
    auto *core = static_cast<MpvCore *>(ctx);
    std::lock_guard lock(core->mutex);
    if (core->item)
        QMetaObject::invokeMethod(core->item, &MpvItem::processEvents, Qt::QueuedConnection);
}

void onMpvRenderUpdate(void *ctx)
{
    auto *core = static_cast<MpvCore *>(ctx);
    std::lock_guard lock(core->mutex);
    if (core->item)
        QMetaObject::invokeMethod(core->item, &QQuickItem::update, Qt::QueuedConnection);
}

QVariant nodeToVariant(const mpv_node &n)
{
    switch (n.format) {
    case MPV_FORMAT_STRING:
        return QString::fromUtf8(n.u.string);
    case MPV_FORMAT_FLAG:
        return n.u.flag != 0;
    case MPV_FORMAT_INT64:
        return static_cast<qlonglong>(n.u.int64);
    case MPV_FORMAT_DOUBLE:
        return n.u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        QVariantList list;
        for (int i = 0; i < n.u.list->num; ++i)
            list.append(nodeToVariant(n.u.list->values[i]));
        return list;
    }
    case MPV_FORMAT_NODE_MAP: {
        QVariantMap map;
        for (int i = 0; i < n.u.list->num; ++i)
            map.insert(QString::fromUtf8(n.u.list->keys[i]), nodeToVariant(n.u.list->values[i]));
        return map;
    }
    default:
        return {};
    }
}

// Converts mpv's track-list into maps with id/type/title/lang/selected/external (+codec, label).
QVariantList convertTracks(const QVariant &node)
{
    QVariantList out;
    const QVariantList items = node.toList();
    for (const QVariant &item : items) {
        const QVariantMap src = item.toMap();
        const QString type = src.value("type").toString();
        if (type != "video" && type != "audio" && type != "sub")
            continue;
        const int id = src.value("id").toInt();
        const QString title = src.value("title").toString();
        const QString lang = src.value("lang").toString();
        const QString codec = src.value("codec").toString();
        const bool external = src.value("external").toBool();

        QString label = !title.isEmpty() ? title : (!lang.isEmpty() ? lang : QStringLiteral("Track %1").arg(id));
        if (!title.isEmpty() && !lang.isEmpty())
            label += QStringLiteral(" [%1]").arg(lang);
        if (!codec.isEmpty())
            label += QStringLiteral(" - %1").arg(codec);
        if (external)
            label += QStringLiteral(" (external)");

        QVariantMap t;
        t["id"] = id;
        t["type"] = type;
        t["title"] = title;
        t["lang"] = lang;
        t["selected"] = src.value("selected").toBool();
        t["external"] = external;
        t["codec"] = codec;
        t["label"] = label;
        out.append(t);
    }
    return out;
}

// Maps a Qt key event to an mpv key name (as used by the "keypress" command). Empty = not mappable.
QString mpvKeyName(int key, Qt::KeyboardModifiers mods, const QString &text)
{
    QString base;
    bool special = true;

    switch (key) {
    case Qt::Key_Space: base = "SPACE"; break;
    case Qt::Key_Return:
    case Qt::Key_Enter: base = "ENTER"; break;
    case Qt::Key_Escape: base = "ESC"; break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: base = "TAB"; break;
    case Qt::Key_Backspace: base = "BS"; break;
    case Qt::Key_Delete: base = "DEL"; break;
    case Qt::Key_Insert: base = "INS"; break;
    case Qt::Key_Home: base = "HOME"; break;
    case Qt::Key_End: base = "END"; break;
    case Qt::Key_PageUp: base = "PGUP"; break;
    case Qt::Key_PageDown: base = "PGDWN"; break;
    case Qt::Key_Left: base = "LEFT"; break;
    case Qt::Key_Right: base = "RIGHT"; break;
    case Qt::Key_Up: base = "UP"; break;
    case Qt::Key_Down: base = "DOWN"; break;
    case Qt::Key_Menu: base = "MENU"; break;
    case Qt::Key_Pause: base = "PAUSE"; break;
    case Qt::Key_Print: base = "PRINT"; break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
            base = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
        else
            special = false;
        break;
    }

    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);
    const bool meta = mods.testFlag(Qt::MetaModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier) || key == Qt::Key_Backtab;

    if (!special) {
        // Printable key. For plain/shifted input the produced character already carries the shift state.
        QString ch;
        if (!ctrl && !alt && !text.isEmpty() && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7f) {
            ch = text.left(1);
        } else if (key >= 0x20 && key <= 0x7e) {
            const QChar c(key);
            ch = (c.isLetter() && !shift) ? QString(c.toLower()) : QString(c);
        }
        if (ch.isEmpty())
            return {};
        if (ch == QLatin1String("#"))
            ch = QStringLiteral("SHARP");
        QString name;
        if (ctrl) name += "Ctrl+";
        if (alt) name += "Alt+";
        if (meta) name += "Meta+";
        return name + ch;
    }

    QString name;
    if (shift) name += "Shift+";
    if (ctrl) name += "Ctrl+";
    if (alt) name += "Alt+";
    if (meta) name += "Meta+";
    return name + base;
}

QString toMpvPath(const QString &path)
{
    if (path.contains(QLatin1String("://"))) {
        const QUrl url(path);
        if (url.isLocalFile())
            return QDir::toNativeSeparators(url.toLocalFile());
    }
    return path;
}

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(name)) : nullptr;
}

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(std::shared_ptr<MpvCore> core) : m_core(std::move(core))
    {
        // Called on the render thread with the GL context current.
        mpv_opengl_init_params glInit{};
        glInit.get_proc_address = getProcAddress;
        glInit.get_proc_address_ctx = nullptr;

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&m_core->render, m_core->mpv, params) < 0) {
            qCritical() << "mpv_render_context_create failed";
            m_core->render = nullptr;
            return;
        }
        mpv_render_context_set_update_callback(m_core->render, onMpvRenderUpdate, m_core.get());

        std::lock_guard lock(m_core->mutex);
        if (m_core->item)
            QMetaObject::invokeMethod(m_core->item, &MpvItem::onRenderContextReady, Qt::QueuedConnection);
    }

    ~MpvRenderer() override
    {
        // Render thread, GL context current: must free before mpv_terminate_destroy (in ~MpvCore).
        if (m_core->render) {
            mpv_render_context_set_update_callback(m_core->render, nullptr, nullptr);
            mpv_render_context_free(m_core->render);
            m_core->render = nullptr;
        }
    }

    void render() override
    {
        if (!m_core->render)
            return;
        QOpenGLFramebufferObject *fbo = framebufferObject();

        mpv_opengl_fbo mpvFbo{};
        mpvFbo.fbo = static_cast<int>(fbo->handle());
        mpvFbo.w = fbo->width();
        mpvFbo.h = fbo->height();
        mpvFbo.internal_format = 0;
        int flipY = 0; // QQuickFramebufferObject already presents the FBO the right way up

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpvFbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_core->render, params);

        // mpv changes GL state behind Qt's back.
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    std::shared_ptr<MpvCore> m_core;
};

} // namespace

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent), m_core(std::make_shared<MpvCore>())
{
    m_core->item = this;
    m_core->mpv = mpv_create();
    if (!m_core->mpv) {
        qCritical() << "mpv_create failed";
        return;
    }
    mpv_handle *mpv = m_core->mpv;

    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "auto-safe");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", "all=info");
    mpv_set_option_string(mpv, "input-default-bindings", "yes");
    mpv_set_option_string(mpv, "osd-level", "1"); // OSD is drawn by libmpv through the render API
    mpv_set_option_string(mpv, "resume-playback", "yes");
    mpv_set_option_string(mpv, "save-position-on-quit", "yes");
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/watch_later";
        QDir().mkpath(dir);
        mpv_set_option_string(mpv, "watch-later-dir", QDir::toNativeSeparators(dir).toUtf8().constData());
    }

    if (mpv_initialize(mpv) < 0) {
        qCritical() << "mpv_initialize failed";
        return;
    }

    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(mpv, 0, "sub-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 0, "idle-active", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(mpv, onMpvWakeup, m_core.get());
}

MpvItem::~MpvItem()
{
    saveWatchLater();
    // Stop callback threads from touching this item. The renderer (if still alive)
    // keeps m_core, and thus mpv, alive until it has freed the render context.
    if (m_core->mpv)
        mpv_set_wakeup_callback(m_core->mpv, nullptr, nullptr);
    std::lock_guard lock(m_core->mutex);
    m_core->item = nullptr;
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    return new MpvRenderer(m_core);
}

void MpvItem::onRenderContextReady()
{
    m_renderReady = true;
    if (!m_pendingFile.isEmpty())
        loadFile(std::exchange(m_pendingFile, QString()));
}

void MpvItem::loadFile(const QString &path)
{
    if (!m_core->mpv)
        return;
    if (!m_renderReady) {
        m_pendingFile = path;
        return;
    }
    saveWatchLater(); // remember where we were in the file being replaced
    command({"loadfile", toMpvPath(path)});
}

void MpvItem::togglePause()
{
    command({"cycle", "pause"});
}

void MpvItem::seek(double seconds)
{
    command({"seek", QString::number(seconds, 'f', 3), "absolute"});
}

void MpvItem::setVolume(double volume)
{
    setPropertyDouble("volume", std::clamp(volume, 0.0, 130.0));
}

void MpvItem::toggleMute()
{
    command({"cycle", "mute"});
}

void MpvItem::setAudioTrack(int id)
{
    setPropertyString("aid", id > 0 ? QString::number(id) : QStringLiteral("no"));
}

void MpvItem::setSubTrack(int id)
{
    setPropertyString("sid", id > 0 ? QString::number(id) : QStringLiteral("no"));
}

void MpvItem::addSubtitle(const QString &path)
{
    if (!m_hasMedia)
        return;
    command({"sub-add", toMpvPath(path), "select"});
}

void MpvItem::adjustSubDelay(double delta)
{
    setPropertyDouble("sub-delay", std::round((m_subDelay + delta) * 10.0) / 10.0);
}

void MpvItem::resetSubDelay()
{
    setPropertyDouble("sub-delay", 0.0);
}

bool MpvItem::sendKey(int qtKey, int modifiers, const QString &text)
{
    const QString name = mpvKeyName(qtKey, Qt::KeyboardModifiers::fromInt(modifiers), text);
    if (name.isEmpty())
        return false;
    sendKeyName(name);
    return true;
}

void MpvItem::sendKeyName(const QString &mpvKey)
{
    command({"keypress", mpvKey});
}

void MpvItem::saveWatchLater()
{
    if (!m_core->mpv || m_shutdown || !m_hasMedia)
        return;
    const char *cmd[] = {"write-watch-later-config", nullptr};
    mpv_command(m_core->mpv, cmd); // synchronous: must finish before shutdown
}

void MpvItem::command(const QStringList &args)
{
    if (!m_core->mpv || m_shutdown)
        return;
    std::vector<QByteArray> bytes;
    bytes.reserve(static_cast<size_t>(args.size()));
    for (const QString &a : args)
        bytes.push_back(a.toUtf8());
    std::vector<const char *> argv;
    argv.reserve(bytes.size() + 1);
    for (const QByteArray &b : bytes)
        argv.push_back(b.constData());
    argv.push_back(nullptr);
    mpv_command_async(m_core->mpv, 0, argv.data());
}

void MpvItem::setPropertyString(const char *name, const QString &value)
{
    if (!m_core->mpv || m_shutdown)
        return;
    const QByteArray utf8 = value.toUtf8();
    const char *str = utf8.constData();
    mpv_set_property_async(m_core->mpv, 0, name, MPV_FORMAT_STRING, &str);
}

void MpvItem::setPropertyDouble(const char *name, double value)
{
    if (!m_core->mpv || m_shutdown)
        return;
    mpv_set_property_async(m_core->mpv, 0, name, MPV_FORMAT_DOUBLE, &value);
}

void MpvItem::processEvents()
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv)
        return;

    while (true) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;

        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto *prop = static_cast<mpv_event_property *>(ev->data);
            const QByteArray name(prop->name);
            if (name == "time-pos") {
                m_position = prop->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(prop->data) : 0.0;
                emit positionChanged();
            } else if (name == "duration") {
                m_duration = prop->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(prop->data) : 0.0;
                emit durationChanged();
            } else if (name == "pause") {
                if (prop->format == MPV_FORMAT_FLAG) {
                    m_paused = *static_cast<int *>(prop->data) != 0;
                    emit pausedChanged();
                    emit playingChanged();
                }
            } else if (name == "volume") {
                m_volume = prop->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(prop->data) : 100.0;
                emit volumeChanged();
            } else if (name == "mute") {
                if (prop->format == MPV_FORMAT_FLAG) {
                    m_muted = *static_cast<int *>(prop->data) != 0;
                    emit mutedChanged();
                }
            } else if (name == "track-list") {
                m_tracks = prop->format == MPV_FORMAT_NODE
                               ? convertTracks(nodeToVariant(*static_cast<mpv_node *>(prop->data)))
                               : QVariantList();
                emit tracksChanged();
            } else if (name == "sub-delay") {
                m_subDelay = prop->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(prop->data) : 0.0;
                emit subDelayChanged();
            } else if (name == "media-title") {
                m_mediaTitle = prop->format == MPV_FORMAT_STRING
                                   ? QString::fromUtf8(*static_cast<char **>(prop->data))
                                   : QString();
                emit mediaTitleChanged();
            } else if (name == "idle-active") {
                if (prop->format == MPV_FORMAT_FLAG) {
                    m_hasMedia = *static_cast<int *>(prop->data) == 0;
                    emit hasMediaChanged();
                    emit playingChanged();
                }
            } else if (name == "hwdec-current") {
                const char *value = prop->format == MPV_FORMAT_STRING ? *static_cast<char **>(prop->data) : nullptr;
                qInfo() << "hwdec-current:" << (value && *value ? value : "(none)");
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            const auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            if (ef->reason == MPV_END_FILE_REASON_ERROR)
                qWarning() << "playback error:" << mpv_error_string(ef->error);
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            qInfo() << "mpv shutdown";
            m_shutdown = true;
            emit shutdown();
            return;
        default:
            break;
        }
    }
}
