#include "livekitplayer.h"

#include "livekit/remote_participant.h"
#include "livekit/remote_track_publication.h"
#include "logger.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

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

LiveKitPlayer::~LiveKitPlayer() { shutdownPlayback(); }

void LiveKitPlayer::startPlayback(const QString &bearerJwt,
                                  const QJsonObject &descriptor) {
  receivingDesired_.store(true);
  stopRequested_.store(false);
  descriptorCameraId_ =
      descriptor.value(QStringLiteral("cameraId")).toString().toStdString();
  bearerJwt_ = bearerJwt;
  descriptor_ = descriptor;
  autoReconnectAttempt_ = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (room_ && !streams_.empty()) {
      for (const auto &s : streams_) {
        applyVideoDimensions(s.publication);
        if (s.publication) {
          s.publication->setSubscribed(true);
        }
        registerFrameCallbackLocked(s.participantIdentity, s.trackName);
        logEvent("LK", "RESUBSCRIBE participant=%s track=%s",
                 s.participantIdentity.c_str(), s.trackName.c_str());
      }
      emit statusChanged(QStringLiteral("Resuming %1 track(s)")
                             .arg(static_cast<int>(streams_.size())));
      return;
    }
    if (room_) {
      // We have an open room but never cached any tracks (publisher
      // didn't join, or every track got SDK-unsubscribed before pause).
      // The cached room is useless on retry; tear it down so the fresh-
      // fetch path below re-requests a token and connects again.
      logEvent("LK", "RESTART tearing down stale room (no cached streams)");
      room_->setDelegate(nullptr);
      room_.reset();
      streams_.clear();
    }
  }

  performTokenFetchAndConnect();
}

void LiveKitPlayer::performTokenFetchAndConnect() {
  if (stopRequested_.load() || !receivingDesired_.load()) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }

  if (!nam_) {
    nam_ = new QNetworkAccessManager(this);
  }

  emit statusChanged(QStringLiteral("Requesting LiveKit token..."));

  QNetworkRequest req{QUrl(QString::fromLatin1(kCreateTokenUrl))};
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setRawHeader("Authorization",
                   QByteArray("Bearer ") + bearerJwt_.toUtf8());
  req.setRawHeader("Accept", "application/json, text/plain, */*");

  const QByteArray body =
      QJsonDocument(descriptor_).toJson(QJsonDocument::Compact);

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

  std::lock_guard<std::mutex> lock(mutex_);
  if (room_) {
    for (const auto &s : streams_) {
      room_->clearOnVideoFrameCallback(s.participantIdentity, s.trackName);
      if (s.publication) {
        s.publication->setSubscribed(false);
      }
      logEvent("LK", "UNSUBSCRIBE participant=%s track=%s",
               s.participantIdentity.c_str(), s.trackName.c_str());
    }
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

  std::lock_guard<std::mutex> lock(mutex_);
  if (room_) {
    for (const auto &s : streams_) {
      room_->clearOnVideoFrameCallback(s.participantIdentity, s.trackName);
      logEvent("LK", "UNSUBSCRIBE (shutdown) participant=%s track=%s",
               s.participantIdentity.c_str(), s.trackName.c_str());
    }
    room_->setDelegate(nullptr);
    room_.reset();
    logEvent("LK", "ROOM_TEARDOWN");
  }
  streams_.clear();
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
    emit errorOccurred(
        QStringLiteral("Token response missing 'token' or 'apiUrl'"));
    return;
  }

  if (stopRequested_.load() || !receivingDesired_.load()) {
    return;
  }

  const QString wssUrl = httpsToWss(apiUrl);
  logEvent("LK", "TOKEN_OK apiUrl=%s wssUrl=%s tokenLen=%d",
           apiUrl.toStdString().c_str(), wssUrl.toStdString().c_str(),
           token.size());
  if (worker_.joinable()) {
    worker_.join();
  }
  worker_ =
      std::thread(&LiveKitPlayer::connectWorker, this, wssUrl, apiUrl, token);
}

void LiveKitPlayer::connectWorker(QString wssUrl, QString httpsUrl,
                                  QString token) {
  static std::once_flag sdkInitFlag;
  std::call_once(sdkInitFlag, []() {
    if (livekit::initialize(livekit::LogLevel::Info,
                            livekit::LogSink::kConsole)) {
      sdkInitialized_.store(true);
    }
  });

  livekit::RoomOptions options;
  options.auto_subscribe = true;
  options.dynacast = false;

  auto tryConnect = [&](const QString &url) -> std::unique_ptr<livekit::Room> {
    if (stopRequested_.load()) {
      return nullptr;
    }
    emit statusChanged(
        QStringLiteral("Connecting to LiveKit at %1...").arg(url));
    logEvent("LK", "CONNECT_ATTEMPT url=%s", url.toStdString().c_str());
    auto room = std::make_unique<livekit::Room>();
    room->setDelegate(this);
    const bool ok =
        room->Connect(url.toStdString(), token.toStdString(), options);
    logEvent("LK", "CONNECT_RESULT url=%s ok=%d stopRequested=%d",
             url.toStdString().c_str(), ok ? 1 : 0,
             stopRequested_.load() ? 1 : 0);
    if (!ok || stopRequested_.load()) {
      room->setDelegate(nullptr);
      return nullptr;
    }
    return room;
  };

  auto room = tryConnect(wssUrl);
  if (!room && !stopRequested_.load() && wssUrl != httpsUrl) {
    emit statusChanged(QStringLiteral("wss connect failed; retrying https..."));
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

void LiveKitPlayer::onRoomConnected() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load() || !room_) {
      return;
    }
    const auto participants = room_->remoteParticipants();
    logEvent("LK", "ROOM_CONNECTED initialParticipants=%zu",
             participants.size());
    for (const auto &participant : participants) {
      if (!participant) {
        logEvent("LK", "  initialParticipant=<null entry>");
        continue;
      }
      logEvent("LK", "  initialParticipant id=%s pubs=%zu",
               participant->identity().c_str(),
               participant->trackPublications().size());
      subscribeParticipantLocked(participant.get());
    }
  }
  emit statusChanged(
      QStringLiteral("Connected. Waiting for remote video tracks..."));

  // Periodic room snapshot so we can see participants/tracks even if no
  // delegate events fire (helps diagnose silent stalls).
  QTimer::singleShot(2000, this, &LiveKitPlayer::logRoomSnapshot);
  // Publisher-join check: poll every 1 s, up to 5 attempts. Stops early
  // if a core-<cameraId> joins or any video gets subscribed.
  publisherCheckAttempt_ = 0;
  QTimer::singleShot(1000, this, &LiveKitPlayer::publisherJoinCheck);
}

void LiveKitPlayer::publisherJoinCheck() {
  constexpr int kMaxAttempts = 5;
  constexpr int kIntervalMs = 1000;

  bool shouldRetry = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load() || !room_) {
      return;
    }
    publisherCheckAttempt_ += 1;

    if (!streams_.empty()) {
      return;
    }
    bool corePublisherPresent = false;
    for (const auto &participant : room_->remoteParticipants()) {
      if (!participant) continue;
      if (participant->identity().rfind("core-", 0) == 0) {
        corePublisherPresent = true;
        break;
      }
    }
    if (corePublisherPresent) {
      return;
    }

    logEvent("LK",
             "PUBLISHER_TIMEOUT cameraId=%s attempt=%d/%d afterMs=%d "
             "(no core-<cameraId> joined; backend routing issue)",
             descriptorCameraId_.c_str(), publisherCheckAttempt_, kMaxAttempts,
             publisherCheckAttempt_ * kIntervalMs);

    if (publisherCheckAttempt_ >= kMaxAttempts) {
      // Try auto-reconnect (fresh token + new room) before giving up.
      // attemptAutoReconnectLocked emits the final errorOccurred itself on
      // GIVE_UP, so we don't emit anything here.
      attemptAutoReconnectLocked();
    } else {
      shouldRetry = true;
    }
  }
  if (shouldRetry) {
    QTimer::singleShot(kIntervalMs, this, &LiveKitPlayer::publisherJoinCheck);
  }
}

void LiveKitPlayer::attemptAutoReconnectLocked() {
  constexpr int kMaxAutoReconnects = 2;
  constexpr int kRetryDelayMs = 2000;

  if (autoReconnectAttempt_ >= kMaxAutoReconnects) {
    logEvent("LK", "AUTO_RECONNECT_GIVE_UP cameraId=%s afterAttempts=%d",
             descriptorCameraId_.c_str(), autoReconnectAttempt_);
    emit errorOccurred(
        QStringLiteral("Gave up on cameraId=%1 after %2 auto-reconnects")
            .arg(QString::fromStdString(descriptorCameraId_))
            .arg(autoReconnectAttempt_));
    return;
  }

  autoReconnectAttempt_ += 1;
  logEvent("LK", "AUTO_RECONNECT cameraId=%s attempt=%d/%d",
           descriptorCameraId_.c_str(), autoReconnectAttempt_,
           kMaxAutoReconnects);
  emit statusChanged(
      QStringLiteral("Auto-reconnect %1/%2 for cameraId=%3")
          .arg(autoReconnectAttempt_)
          .arg(kMaxAutoReconnects)
          .arg(QString::fromStdString(descriptorCameraId_)));

  if (room_) {
    for (const auto &s : streams_) {
      room_->clearOnVideoFrameCallback(s.participantIdentity, s.trackName);
    }
    room_->setDelegate(nullptr);
    room_.reset();
  }
  streams_.clear();
  publisherCheckAttempt_ = 0;

  QTimer::singleShot(kRetryDelayMs, this,
                     &LiveKitPlayer::performTokenFetchAndConnect);
}

void LiveKitPlayer::logRoomSnapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopRequested_.load() || !room_) {
    return;
  }
  const auto participants = room_->remoteParticipants();
  logEvent("LK", "SNAPSHOT participants=%zu streams=%zu", participants.size(),
           streams_.size());
  for (const auto &participant : participants) {
    if (!participant) continue;
    const auto &pubs = participant->trackPublications();
    logEvent("LK", "  participant id=%s pubs=%zu",
             participant->identity().c_str(), pubs.size());
    for (const auto &entry : pubs) {
      const auto &p = entry.second;
      if (!p) {
        logEvent("LK", "    pub sid=%s <null>", entry.first.c_str());
        continue;
      }
      logEvent("LK",
               "    pub sid=%s name='%s' kind=%d source=%d subscribed=%d",
               p->sid().c_str(), p->name().c_str(),
               static_cast<int>(p->kind()), static_cast<int>(p->source()),
               p->subscribed() ? 1 : 0);
    }
  }
  QTimer::singleShot(5000, this, &LiveKitPlayer::logRoomSnapshot);
}

void LiveKitPlayer::onParticipantConnected(
    livekit::Room & /* room */,
    const livekit::ParticipantConnectedEvent &event) {
  logEvent("LK", "PARTICIPANT_JOINED id=%s pubs=%zu",
           event.participant ? event.participant->identity().c_str() : "<null>",
           event.participant ? event.participant->trackPublications().size()
                             : 0);
  if (stopRequested_.load() || !event.participant) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!room_) {
    return;
  }
  subscribeParticipantLocked(event.participant);
}

void LiveKitPlayer::onParticipantDisconnected(
    livekit::Room & /* room */,
    const livekit::ParticipantDisconnectedEvent &event) {
  logEvent("LK", "PARTICIPANT_LEFT id=%s",
           event.participant ? event.participant->identity().c_str()
                             : "<null>");
}

void LiveKitPlayer::onTrackPublished(
    livekit::Room & /* room */, const livekit::TrackPublishedEvent &event) {
  const auto &pub = event.publication;
  logEvent("LK",
           "TRACK_PUBLISHED participant=%s pub=%s kind=%d source=%d "
           "name='%s' subscribed=%d",
           event.participant ? event.participant->identity().c_str() : "<null>",
           pub ? pub->sid().c_str() : "<null>",
           pub ? static_cast<int>(pub->kind()) : -1,
           pub ? static_cast<int>(pub->source()) : -1,
           pub ? pub->name().c_str() : "",
           pub ? (pub->subscribed() ? 1 : 0) : -1);
  if (stopRequested_.load() || !event.participant) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!room_) {
    return;
  }
  // Re-walk the participant in case the SDK delivered TRACK_PUBLISHED with a
  // null publication but the participant map already holds it (seen in the
  // older logs as the "fallback picked pub" path).
  subscribeParticipantLocked(event.participant);
}

void LiveKitPlayer::onTrackSubscriptionFailed(
    livekit::Room & /* room */,
    const livekit::TrackSubscriptionFailedEvent &event) {
  logEvent("LK", "TRACK_SUBSCRIPTION_FAILED sid=%s error=%s",
           event.track_sid.c_str(), event.error.c_str());
  emit errorOccurred(
      QStringLiteral("Track subscription failed for SID %1: %2")
          .arg(QString::fromStdString(event.track_sid),
               QString::fromStdString(event.error)));
}

void LiveKitPlayer::onTrackSubscribed(
    livekit::Room & /* room */, const livekit::TrackSubscribedEvent &event) {
  logEvent("LK",
           "TRACK_SUBSCRIBED participant=%s track=%s pub=%s kind=%d",
           event.participant ? event.participant->identity().c_str()
                             : "<null>",
           event.track ? event.track->name().c_str() : "<null>",
           event.publication ? event.publication->sid().c_str() : "<null>",
           event.publication ? static_cast<int>(event.publication->kind()) : -1);
  if (!event.publication ||
      event.publication->kind() != livekit::TrackKind::KIND_VIDEO ||
      event.participant == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopRequested_.load() || !room_) {
    return;
  }
  registerTrackLocked(event.publication, event.participant->identity());
}

void LiveKitPlayer::onTrackUnsubscribed(
    livekit::Room & /* room */, const livekit::TrackUnsubscribedEvent &event) {
  const std::string participantId =
      event.participant ? event.participant->identity() : std::string{};
  const std::string trackName =
      event.publication ? event.publication->name()
                        : (event.track ? event.track->name() : std::string{});
  logEvent("LK", "SDK_UNSUBSCRIBED participant=%s track=%s",
           participantId.c_str(), trackName.c_str());
  // Note: don't erase from streams_ here. The SDK fires this for our own
  // pauseReceiving-driven setSubscribed(false), and we need streams_ intact
  // so the next Play All can take the resubscribe path. shutdownPlayback
  // clears streams_ when the player is actually torn down.

  emit statusChanged(QStringLiteral("Track unsubscribed: %1")
                         .arg(QString::fromStdString(trackName)));
}

void LiveKitPlayer::onDisconnected(
    livekit::Room & /* room */, const livekit::DisconnectedEvent &event) {
  logEvent("LK", "SDK_DISCONNECTED reason=%d",
           static_cast<int>(event.reason));

  std::lock_guard<std::mutex> lock(mutex_);
  streams_.clear();
  room_.reset();
  emit statusChanged(QStringLiteral("Disconnected from LiveKit room"));
}

void LiveKitPlayer::onConnectionStateChanged(
    livekit::Room & /* room */,
    const livekit::ConnectionStateChangedEvent &event) {
  logEvent("LK", "CONNECTION_STATE %s",
           connectionStateToString(event.state).toStdString().c_str());
  emit statusChanged(QStringLiteral("Connection state: %1")
                         .arg(connectionStateToString(event.state)));
}

void LiveKitPlayer::subscribeParticipantLocked(
    livekit::RemoteParticipant *participant) {
  if (!participant || !room_) {
    return;
  }
  const std::string participantId = participant->identity();
  const auto &pubs = participant->trackPublications();
  logEvent("LK", "SCAN_PARTICIPANT id=%s pubs=%zu", participantId.c_str(),
           pubs.size());
  for (const auto &entry : pubs) {
    const auto &pub = entry.second;
    if (!pub) {
      logEvent("LK", "  scan pub sid=%s <null>", entry.first.c_str());
      continue;
    }
    logEvent("LK", "  scan pub sid=%s name='%s' kind=%d source=%d subscribed=%d",
             pub->sid().c_str(), pub->name().c_str(),
             static_cast<int>(pub->kind()),
             static_cast<int>(pub->source()),
             pub->subscribed() ? 1 : 0);
    if (pub->kind() != livekit::TrackKind::KIND_VIDEO) {
      continue;
    }
    registerTrackLocked(pub, participantId);
  }
}

void LiveKitPlayer::registerTrackLocked(
    const std::shared_ptr<livekit::RemoteTrackPublication> &publication,
    const std::string &participantIdentity) {
  if (!publication || !room_) {
    return;
  }
  const std::string trackName = publication->name();
  for (const auto &s : streams_) {
    if (s.participantIdentity == participantIdentity &&
        s.trackName == trackName) {
      return;
    }
  }

  applyVideoDimensions(publication);
  publication->setSubscribed(true);
  streams_.push_back({publication, participantIdentity, trackName});
  registerFrameCallbackLocked(participantIdentity, trackName);

  logEvent("LK", "SUBSCRIBE participant=%s track=%s mime=%s",
           participantIdentity.c_str(), trackName.c_str(),
           publication->mimeType().c_str());

  emit mimeTypeReceived(publication->mimeType());
  emit statusChanged(QStringLiteral("Playing track '%1' from '%2'")
                         .arg(QString::fromStdString(trackName),
                              QString::fromStdString(participantIdentity)));
}

void LiveKitPlayer::registerFrameCallbackLocked(
    const std::string &participantIdentity, const std::string &trackName) {
  if (!room_ || participantIdentity.empty() || trackName.empty()) {
    return;
  }
  livekit::VideoStream::Options options;
  options.capacity = 2;
  options.format = livekit::VideoBufferType::I420;
  room_->setOnVideoFrameCallback(
      participantIdentity, trackName,
      [this](const livekit::VideoFrame &frame, std::int64_t /* timestampUs */) {
        if (receivingDesired_.load() && !stopRequested_.load()) {
          emitFrameFromLiveKit(frame);
        }
      },
      options);
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
  }
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

  const int w = frame.width();
  const int h = frame.height();

  auto held = std::make_shared<livekit::VideoFrame>(
      std::move(const_cast<livekit::VideoFrame &>(frame)));

  YuvFrame out{std::const_pointer_cast<const livekit::VideoFrame>(std::move(held)),
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
