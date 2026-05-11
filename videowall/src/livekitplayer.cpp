#include "livekitplayer.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
constexpr const char *kCreateTokenUrl =
    "https://api-staging.lumix.ai/v1/live-view/lwebrtc/create-token/"
    "65687f0364d1bb3b7b207c5c/6953cff92a13ade0364679ec/"
    "6953ecc355947949135d3e08";

QString httpsToWss(const QString &url) {
  if (url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
    return QStringLiteral("wss://") +
           url.mid(QStringLiteral("https://").size());
  }
  if (url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
    return QStringLiteral("ws://") +
           url.mid(QStringLiteral("http://").size());
  }
  return url;
}

constexpr int kMinFrameDimension = 1;

// Copy one I420 plane out of the SDK frame into a tightly-packed destination
// buffer (stride == planeW). Used per-plane on the SDK reader thread.
void copyPlaneTight(std::uint8_t *dst, const livekit::VideoPlaneInfo &src,
                    int planeW, int planeH) {
  const std::uint8_t *srcPtr =
      reinterpret_cast<const std::uint8_t *>(src.data_ptr);
  const int srcStride = static_cast<int>(src.stride);
  if (srcStride == planeW) {
    std::memcpy(dst, srcPtr, static_cast<std::size_t>(planeW) * planeH);
  } else {
    for (int row = 0; row < planeH; ++row) {
      std::memcpy(dst + row * planeW, srcPtr + row * srcStride,
                  static_cast<std::size_t>(planeW));
    }
  }
}
} // namespace

std::atomic_bool LiveKitPlayer::sdkInitialized_{false};
std::atomic<int> LiveKitPlayer::requestedWidth_{0};
std::atomic<int> LiveKitPlayer::requestedHeight_{0};

void LiveKitPlayer::setRequestedDimensions(int width, int height) {
  requestedWidth_.store(width > 0 ? width : 0);
  requestedHeight_.store(height > 0 ? height : 0);
}

LiveKitPlayer::LiveKitPlayer(QObject *parent) : QObject(parent) {
  static std::once_flag metaTypeFlag;
  std::call_once(metaTypeFlag,
                 []() { qRegisterMetaType<YuvFrame>("YuvFrame"); });
}

LiveKitPlayer::~LiveKitPlayer() {
  stopPlayback();
}

void LiveKitPlayer::startPlayback(const QString &bearerJwt,
                                  const QJsonObject &descriptor) {
  stopPlayback();
  stopRequested_.store(false);

  if (!nam_) {
    nam_ = new QNetworkAccessManager(this);
  }

  emit statusChanged(QStringLiteral("Requesting LiveKit token..."));

  QNetworkRequest req{QUrl(QString::fromLatin1(kCreateTokenUrl))};
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setRawHeader("Authorization",
                   QByteArray("Bearer ") + bearerJwt.toUtf8());
  req.setRawHeader("Accept", "application/json, text/plain, */*");

  const QByteArray body =
      QJsonDocument(descriptor).toJson(QJsonDocument::Compact);

  currentReply_ = nam_->post(req, body);
  QNetworkReply *reply = currentReply_;
  connect(reply, &QNetworkReply::finished, this,
          [this, reply]() { onTokenReply(reply); });
}

void LiveKitPlayer::stopPlayback() {
  stopRequested_.store(true);

  if (currentReply_) {
    currentReply_->disconnect();
    currentReply_->abort();
    currentReply_->deleteLater();
    currentReply_ = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (room_) {
      clearActiveVideoCallbackLocked();
      room_.reset();
    }
  }

  if (worker_.joinable()) {
    worker_.join();
  }
}

void LiveKitPlayer::onTokenReply(QNetworkReply *reply) {
  if (reply != currentReply_) {
    reply->deleteLater();
    return;
  }
  currentReply_ = nullptr;
  reply->deleteLater();

  if (stopRequested_.load()) {
    return;
  }

  const int httpStatus =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray payload = reply->readAll();

  if (reply->error() != QNetworkReply::NoError) {
    emit errorOccurred(QStringLiteral("Token HTTP error (%1): %2 %3")
                           .arg(httpStatus)
                           .arg(reply->errorString())
                           .arg(QString::fromUtf8(payload)));
    return;
  }
  if (httpStatus < 200 || httpStatus >= 300) {
    emit errorOccurred(QStringLiteral("Token HTTP %1: %2")
                           .arg(httpStatus)
                           .arg(QString::fromUtf8(payload)));
    return;
  }

  QJsonParseError perr{};
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &perr);
  if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
    emit errorOccurred(QStringLiteral("Invalid token JSON: %1")
                           .arg(perr.errorString()));
    return;
  }
  const QJsonObject obj = doc.object();
  const QString token = obj.value(QStringLiteral("token")).toString();
  const QString apiUrl = obj.value(QStringLiteral("apiUrl")).toString();
  if (token.isEmpty() || apiUrl.isEmpty()) {
    emit errorOccurred(QStringLiteral(
        "Token response missing 'token' or 'apiUrl'"));
    return;
  }

  const QString wssUrl = httpsToWss(apiUrl);
  worker_ = std::thread(&LiveKitPlayer::connectWorker, this, wssUrl, apiUrl,
                        token);
}

void LiveKitPlayer::onTrackSubscribed(
    livekit::Room &room, const livekit::TrackSubscribedEvent &event) {
  if (!event.track || event.track->kind() != livekit::TrackKind::KIND_VIDEO ||
      event.participant == nullptr) {
    return;
  }

  if (event.publication) {
    fprintf(stderr,
            "[publication] sid=%s simulcast=%s published=%ux%u mime=%s\n",
            event.publication->sid().c_str(),
            event.publication->simulcasted() ? "true" : "false",
            event.publication->width(), event.publication->height(),
            event.publication->mimeType().c_str());

    emit statusChanged(QStringLiteral(
        "Publication: sid=%1, simulcast=%2, published=%3x%4, mime=%5")
        .arg(QString::fromStdString(event.publication->sid()))
        .arg(event.publication->simulcasted() ? "true" : "false")
        .arg(event.publication->width())
        .arg(event.publication->height())
        .arg(QString::fromStdString(event.publication->mimeType())));

    emit mimeTypeReceived(event.publication->mimeType());

    const int reqW = requestedWidth_.load();
    const int reqH = requestedHeight_.load();
    if (reqW > 0 && reqH > 0) {
      event.publication->setVideoQuality(livekit::RemoteVideoQuality::Low);
      event.publication->setVideoDimensions(
          static_cast<std::uint32_t>(reqW), static_cast<std::uint32_t>(reqH));
      emit statusChanged(
          QStringLiteral("Requested video dimensions: %1x%2").arg(reqW).arg(reqH));
    }
  }


  livekit::VideoStream::Options options;
  options.capacity = 2;
  options.format = livekit::VideoBufferType::I420;

  const std::string participantIdentity = event.participant->identity();
  const std::string trackName = event.track->name();

  room.setOnVideoFrameCallback(
      participantIdentity, trackName,
      [this](const livekit::VideoFrame &frame, std::int64_t /* timestampUs */) {
        if (!stopRequested_.load()) {
          emitFrameFromLiveKit(frame);
        }
      },
      options);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    activeParticipantIdentity_ = participantIdentity;
    activeTrackName_ = trackName;
  }

  emit statusChanged(QStringLiteral("Playing track '%1' from '%2'")
                         .arg(QString::fromStdString(trackName),
                              QString::fromStdString(participantIdentity)));
}

void LiveKitPlayer::onTrackUnsubscribed(
    livekit::Room &room, const livekit::TrackUnsubscribedEvent & /* event */) {
  std::lock_guard<std::mutex> lock(mutex_);
  clearActiveVideoCallbackLocked();
  Q_UNUSED(room);
  emit statusChanged(QStringLiteral("Video track unsubscribed"));
}

void LiveKitPlayer::onDisconnected(
    livekit::Room & /* room */, const livekit::DisconnectedEvent & /* event */) {
  emit statusChanged(QStringLiteral("Disconnected from LiveKit room"));
}

void LiveKitPlayer::onConnectionStateChanged(
    livekit::Room & /* room */,
    const livekit::ConnectionStateChangedEvent &event) {
  emit statusChanged(QStringLiteral("Connection state: %1")
                         .arg(connectionStateToString(event.state)));
}

void LiveKitPlayer::onTrackSubscriptionFailed(
    livekit::Room & /* room */,
    const livekit::TrackSubscriptionFailedEvent &event) {
  emit errorOccurred(
      QStringLiteral("Track subscription failed for SID %1: %2")
          .arg(QString::fromStdString(event.track_sid),
               QString::fromStdString(event.error)));
}

void LiveKitPlayer::clearActiveVideoCallbackLocked() {
  if (!room_) {
    return;
  }
  if (!activeParticipantIdentity_.empty() && !activeTrackName_.empty()) {
    room_->clearOnVideoFrameCallback(activeParticipantIdentity_, activeTrackName_);
  }
  activeParticipantIdentity_.clear();
  activeTrackName_.clear();
}

void LiveKitPlayer::emitFrameFromLiveKit(const livekit::VideoFrame &frame) {
  if (frame.width() < kMinFrameDimension ||
      frame.height() < kMinFrameDimension || frame.data() == nullptr) {
    return;
  }
  if (frame.type() != livekit::VideoBufferType::I420) {
    return; // we requested I420; ignore anything else
  }

  if (frameInFlight_.exchange(true)) {
    return; // previous frame not yet consumed — drop this one
  }

  const auto planes = frame.planeInfos();
  if (planes.size() < 3) {
    frameInFlight_.store(false);
    return;
  }

  const int w = frame.width();
  const int h = frame.height();
  const int cw = w / 2;
  const int ch = h / 2;

  auto y = framePool_->acquire(static_cast<std::size_t>(w) * h);
  auto u = framePool_->acquire(static_cast<std::size_t>(cw) * ch);
  auto v = framePool_->acquire(static_cast<std::size_t>(cw) * ch);

  copyPlaneTight(y->data(), planes[0], w, h);
  copyPlaneTight(u->data(), planes[1], cw, ch);
  copyPlaneTight(v->data(), planes[2], cw, ch);

  YuvFrame out{
      std::const_pointer_cast<const std::vector<std::uint8_t>>(std::move(y)),
      std::const_pointer_cast<const std::vector<std::uint8_t>>(std::move(u)),
      std::const_pointer_cast<const std::vector<std::uint8_t>>(std::move(v)),
      w, h};

  emit frameReady(out);

  ++fpsFrameCount_;
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration<double>(now - fpsWindowStart_).count();
  if (elapsed >= 1.0) {
    const double fps = fpsFrameCount_ / elapsed;
    fpsFrameCount_ = 0;
    fpsWindowStart_ = now;
    emit statusChanged(QStringLiteral("Receiving %1x%2 @ %3 fps")
                           .arg(frame.width())
                           .arg(frame.height())
                           .arg(fps, 0, 'f', 1));
  }
}

void LiveKitPlayer::connectWorker(QString wssUrl, QString httpsUrl,
                                  QString token) {
  static std::once_flag sdkInitFlag;
  std::call_once(sdkInitFlag, []() {
    if (livekit::initialize(livekit::LogLevel::Info, livekit::LogSink::kConsole)) {
      sdkInitialized_.store(true);
    }
  });

  livekit::RoomOptions options;
  options.auto_subscribe = true;
  options.dynacast = false;

  auto tryConnect = [&](const QString &url) -> std::unique_ptr<livekit::Room> {
    if (stopRequested_.load()) return nullptr;
    emit statusChanged(QStringLiteral("Connecting to LiveKit at %1...").arg(url));
    auto room = std::make_unique<livekit::Room>();
    room->setDelegate(this);
    const bool ok =
        room->Connect(url.toStdString(), token.toStdString(), options);
    if (!ok) return nullptr;
    return room;
  };

  auto room = tryConnect(wssUrl);
  if (!room && !stopRequested_.load() && wssUrl != httpsUrl) {
    emit statusChanged(
        QStringLiteral("wss connect failed; retrying https..."));
    room = tryConnect(httpsUrl);
  }

  if (!room) {
    if (!stopRequested_.load()) {
      emit errorOccurred(QStringLiteral("Failed to connect to LiveKit room"));
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load()) {
      return;
    }
    room_ = std::move(room);
  }

  emit statusChanged(QStringLiteral("Connected. Waiting for remote video track..."));
}

QString LiveKitPlayer::connectionStateToString(livekit::ConnectionState state) {
  switch (state) {
  case livekit::ConnectionState::Connected:
    return QStringLiteral("Connected");
  case livekit::ConnectionState::Disconnected:
    return QStringLiteral("Disconnected");
  case livekit::ConnectionState::Reconnecting:
    return QStringLiteral("Reconnecting");
  default:
    return QStringLiteral("Unknown");
  }
}
