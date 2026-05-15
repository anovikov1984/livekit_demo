#include "livekitplayer.h"

#include "livekit/remote_participant.h"
#include "livekit/remote_track_publication.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <cstdio>
#include <cstring>

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
  shutdownPlayback();
}

void LiveKitPlayer::startPlayback(const QString &bearerJwt,
                                  const QJsonObject &descriptor) {
  receivingDesired_.store(true);
  stopRequested_.store(false);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (room_) {
      resumeReceivingLocked();
      emit statusChanged(QStringLiteral("Resuming on existing room connection..."));
      return;
    }
  }

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

void LiveKitPlayer::pauseReceiving() {
  receivingDesired_.store(false);
  frameInFlight_.store(false);

  if (currentReply_) {
    currentReply_->disconnect();
    currentReply_->abort();
    currentReply_->deleteLater();
    currentReply_ = nullptr;
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pauseReceivingLocked();
  }

  fpsFrameCount_ = 0;
}

void LiveKitPlayer::shutdownPlayback() {
  receivingDesired_.store(false);
  stopRequested_.store(true);
  frameInFlight_.store(false);

  if (currentReply_) {
    currentReply_->disconnect();
    currentReply_->abort();
    currentReply_->deleteLater();
    currentReply_ = nullptr;
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    destroyRoomLocked();
  }

  fpsFrameCount_ = 0;
}

void LiveKitPlayer::onTokenReply(QNetworkReply *reply) {
  if (reply != currentReply_) {
    reply->deleteLater();
    return;
  }
  currentReply_ = nullptr;
  reply->deleteLater();

  if (stopRequested_.load() || !receivingDesired_.load()) {
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

  if (stopRequested_.load() || !receivingDesired_.load()) {
    return;
  }

  const QString wssUrl = httpsToWss(apiUrl);
  worker_ = std::thread(&LiveKitPlayer::connectWorker, this, wssUrl, apiUrl,
                        token);
}

void LiveKitPlayer::onTrackPublished(
    livekit::Room & /* room */, const livekit::TrackPublishedEvent &event) {
  if (!event.publication || !event.participant ||
      event.publication->kind() != livekit::TrackKind::KIND_VIDEO) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!room_ || stopRequested_.load()) {
    return;
  }
  trySubscribePublication(event.publication, event.participant);
}

void LiveKitPlayer::onTrackSubscribed(
    livekit::Room & /* room */, const livekit::TrackSubscribedEvent &event) {
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
    applyVideoDimensions(event.publication);
  }

  const std::string participantIdentity = event.participant->identity();
  const std::string trackName = event.track->name();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load() || !room_) {
      return;
    }
    activeParticipantIdentity_ = participantIdentity;
    activeTrackName_ = trackName;
    activePublication_ = event.publication;
    if (event.publication) {
      activeTrackSource_ = event.publication->source();
    } else if (event.track->source()) {
      activeTrackSource_ = *event.track->source();
    }
    ensureVideoCallbackRegisteredLocked();
  }

  emit statusChanged(QStringLiteral("Playing track '%1' from '%2'")
                         .arg(QString::fromStdString(trackName),
                              QString::fromStdString(participantIdentity)));
}

void LiveKitPlayer::onTrackUnsubscribed(
    livekit::Room & /* room */, const livekit::TrackUnsubscribedEvent & /* event */) {
  emit statusChanged(QStringLiteral("Video track unsubscribed"));
}

void LiveKitPlayer::onDisconnected(
    livekit::Room & /* room */, const livekit::DisconnectedEvent & /* event */) {
  std::lock_guard<std::mutex> lock(mutex_);
  videoCallbackRegistered_ = false;
  activePublication_.reset();
  activeParticipantIdentity_.clear();
  activeTrackName_.clear();
  activeTrackSource_ = livekit::TrackSource::SOURCE_UNKNOWN;
  room_.reset();
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

void LiveKitPlayer::applyVideoDimensions(
    const std::shared_ptr<livekit::RemoteTrackPublication> &publication) {
  if (!publication) {
    return;
  }
  const int reqW = requestedWidth_.load();
  const int reqH = requestedHeight_.load();
  if (reqW > 0 && reqH > 0) {
    publication->setVideoQuality(livekit::RemoteVideoQuality::Low);
    publication->setVideoDimensions(static_cast<std::uint32_t>(reqW),
                                      static_cast<std::uint32_t>(reqH));
    emit statusChanged(
        QStringLiteral("Requested video dimensions: %1x%2").arg(reqW).arg(reqH));
  }
}

void LiveKitPlayer::trySubscribePublication(
    const std::shared_ptr<livekit::RemoteTrackPublication> &publication,
    livekit::RemoteParticipant *participant) {
  if (!publication || !participant || !room_) {
    return;
  }
  if (publication->kind() != livekit::TrackKind::KIND_VIDEO) {
    return;
  }

  activeParticipantIdentity_ = participant->identity();
  activeTrackName_ = publication->name();
  activeTrackSource_ = publication->source();
  activePublication_ = publication;

  if (!receivingDesired_.load()) {
    return;
  }

  applyVideoDimensions(publication);
  publication->setSubscribed(true);
}

void LiveKitPlayer::scanAndSubscribeExistingTracksLocked() {
  if (!room_) {
    return;
  }
  for (const auto &participant : room_->remoteParticipants()) {
    if (!participant) {
      continue;
    }
    for (const auto &entry : participant->trackPublications()) {
      const auto &publication = entry.second;
      if (!publication ||
          publication->kind() != livekit::TrackKind::KIND_VIDEO) {
        continue;
      }
      trySubscribePublication(publication, participant.get());
      return;
    }
  }
}

void LiveKitPlayer::resumeReceivingLocked() {
  if (!room_) {
    return;
  }
  scanAndSubscribeExistingTracksLocked();
  if (auto publication = activePublication_.lock()) {
    if (!publication->subscribed()) {
      applyVideoDimensions(publication);
      publication->setSubscribed(true);
    }
  }
}

void LiveKitPlayer::pauseReceivingLocked() {
  if (auto publication = activePublication_.lock()) {
    publication->setSubscribed(false);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
}

void LiveKitPlayer::ensureVideoCallbackRegisteredLocked() {
  if (videoCallbackRegistered_ || !room_ ||
      activeParticipantIdentity_.empty() || activeTrackName_.empty()) {
    return;
  }

  livekit::VideoStream::Options options;
  options.capacity = 2;
  options.format = livekit::VideoBufferType::I420;

  room_->setOnVideoFrameCallback(
      activeParticipantIdentity_, activeTrackName_,
      [this](const livekit::VideoFrame &frame, std::int64_t /* timestampUs */) {
        if (receivingDesired_.load() && !stopRequested_.load()) {
          emitFrameFromLiveKit(frame);
        }
      },
      options);
  videoCallbackRegistered_ = true;
}

void LiveKitPlayer::clearActiveVideoCallbackLocked() {
  if (!room_) {
    return;
  }
  if (!activeParticipantIdentity_.empty() && !activeTrackName_.empty()) {
    room_->clearOnVideoFrameCallback(activeParticipantIdentity_,
                                     activeTrackName_);
  }
  videoCallbackRegistered_ = false;
  activeParticipantIdentity_.clear();
  activeTrackName_.clear();
  activeTrackSource_ = livekit::TrackSource::SOURCE_UNKNOWN;
  activePublication_.reset();
}

void LiveKitPlayer::destroyRoomLocked() {
  if (!room_) {
    return;
  }

  if (auto publication = activePublication_.lock()) {
    publication->setSubscribed(false);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }

  clearActiveVideoCallbackLocked();
  room_->setDelegate(nullptr);
  room_.reset();
}

void LiveKitPlayer::onRoomConnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopRequested_.load() || !room_) {
    return;
  }
  if (receivingDesired_.load()) {
    resumeReceivingLocked();
  }
  emit statusChanged(QStringLiteral("Connected. Waiting for remote video track..."));
}

void LiveKitPlayer::emitFrameFromLiveKit(const livekit::VideoFrame &frame) {
  if (!receivingDesired_.load() || stopRequested_.load()) {
    return;
  }
  if (frame.width() < kMinFrameDimension ||
      frame.height() < kMinFrameDimension || frame.data() == nullptr) {
    return;
  }
  if (frame.type() != livekit::VideoBufferType::I420) {
    return;
  }

  if (frameInFlight_.exchange(true)) {
    return;
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
  const auto elapsed =
      std::chrono::duration<double>(now - fpsWindowStart_).count();
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

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (room_) {
      QMetaObject::invokeMethod(this, &LiveKitPlayer::onRoomConnected,
                                Qt::QueuedConnection);
      return;
    }
  }

  livekit::RoomOptions options;
  options.auto_subscribe = false;
  options.dynacast = false;

  auto tryConnect = [&](const QString &url) -> std::unique_ptr<livekit::Room> {
    if (stopRequested_.load()) {
      return nullptr;
    }
    emit statusChanged(QStringLiteral("Connecting to LiveKit at %1...").arg(url));
    auto room = std::make_unique<livekit::Room>();
    room->setDelegate(this);
    const bool ok =
        room->Connect(url.toStdString(), token.toStdString(), options);
    if (!ok || stopRequested_.load()) {
      room->setDelegate(nullptr);
      return nullptr;
    }
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
      room->setDelegate(nullptr);
      return;
    }
    room_ = std::move(room);
  }

  QMetaObject::invokeMethod(this, &LiveKitPlayer::onRoomConnected,
                            Qt::QueuedConnection);
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
