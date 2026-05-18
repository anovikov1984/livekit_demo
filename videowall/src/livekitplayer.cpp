#include "livekitplayer.h"

#include "livekit/remote_participant.h"
#include "livekit/remote_track_publication.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <cstdio>
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

  watchdog_ = new QTimer(this);
  watchdog_->setInterval(3000);
  connect(watchdog_, &QTimer::timeout, this, &LiveKitPlayer::onWatchdogTick);
}

qint64 LiveKitPlayer::elapsedMs() const {
  if (playbackStart_.time_since_epoch().count() == 0) {
    return 0;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - playbackStart_)
      .count();
}

void LiveKitPlayer::startWatchdog() {
  if (watchdog_) {
    watchdog_->start();
  }
}

void LiveKitPlayer::stopWatchdog() {
  if (watchdog_) {
    watchdog_->stop();
  }
}

void LiveKitPlayer::onWatchdogTick() {
  if (!receivingDesired_.load()) {
    stopWatchdog();
    return;
  }

  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }

  const auto frames = framesReceived_.load();
  const auto lastFrameMs = lastFrameMonoMs_.load();
  const auto nowMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const auto sinceFrameMs = lastFrameMs > 0 ? (nowMs - lastFrameMs) : -1;

  // Quiet path: frames are flowing and the most recent one is fresh.
  if (frames > 0 && sinceFrameMs >= 0 && sinceFrameMs < 2000) {
    return;
  }

  qWarning(
      "[slot=%d cam=%s +%lldms] WATCHDOG stuck: room=%d cbReg=%d state=%s "
      "frames=%llu sinceFrameMs=%lld part=%s track=%s",
      slotIndex_,
      cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
      static_cast<long long>(elapsedMs()),
      room_ ? 1 : 0, videoCallbackRegistered_ ? 1 : 0,
      qUtf8Printable(connectionStateToString(lastLoggedConnectionState_.load())),
      static_cast<unsigned long long>(frames),
      static_cast<long long>(sinceFrameMs),
      activeParticipantIdentity_.empty() ? "-"
                                         : activeParticipantIdentity_.c_str(),
      activeTrackName_.empty() ? "-" : activeTrackName_.c_str());

  // Recovery: if connected but not yet receiving, re-scan for a video pub.
  // Catches both "TrackPublishedEvent had null publication" and "publisher
  // joined after our initial scan" cases.
  if (room_ && !videoCallbackRegistered_ && !stopRequested_.load()) {
    scanAndSubscribeExistingTracksLocked();
  }
}

LiveKitPlayer::~LiveKitPlayer() {
  shutdownPlayback();
}

void LiveKitPlayer::startPlayback(const QString &bearerJwt,
                                  const QJsonObject &descriptor) {
  receivingDesired_.store(true);
  stopRequested_.store(false);
  framesReceived_.store(0);
  lastFrameMonoMs_.store(0);
  firstFrameLogged_.store(false);
  playbackStart_ = std::chrono::steady_clock::now();

  cameraLabel_ = descriptor.value(QStringLiteral("cameraId")).toString();

  qInfo("[slot=%d cam=%s +0ms] startPlayback",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_));

  startWatchdog();

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
  qInfo("[slot=%d cam=%s +%lldms] pauseReceiving",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()));
  stopWatchdog();
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
  qInfo("[slot=%d cam=%s +%lldms] shutdownPlayback",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()));
  stopWatchdog();
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

  qInfo("[slot=%d cam=%s +%lldms] token reply httpStatus=%d bytes=%lld",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()), httpStatus,
        static_cast<long long>(payload.size()));

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
  qInfo("[slot=%d cam=%s +%lldms] dispatching connectWorker apiUrl=%s wss=%s",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()), qUtf8Printable(apiUrl),
        qUtf8Printable(wssUrl));
  worker_ = std::thread(&LiveKitPlayer::connectWorker, this, wssUrl, apiUrl,
                        token);
}

void LiveKitPlayer::onTrackPublished(
    livekit::Room & /* room */, const livekit::TrackPublishedEvent &event) {
  const char *sid = event.publication ? event.publication->sid().c_str() : "?";
  const int kind =
      event.publication ? static_cast<int>(event.publication->kind()) : -1;
  const char *pid =
      event.participant ? event.participant->identity().c_str() : "?";
  qInfo("[slot=%d cam=%s +%lldms] onTrackPublished pub=%p part=%p pid=%s "
        "sid=%s kind=%d",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()),
        static_cast<void *>(event.publication.get()),
        static_cast<void *>(event.participant), pid, sid, kind);

  if (!event.participant) {
    return;
  }

  std::shared_ptr<livekit::RemoteTrackPublication> publication =
      event.publication;
  if (!publication) {
    // SDK sometimes delivers a null publication in TrackPublishedEvent; try to
    // recover by scanning the participant's known publications.
    const auto pubs = event.participant->trackPublications();
    for (const auto &entry : pubs) {
      if (entry.second &&
          entry.second->kind() == livekit::TrackKind::KIND_VIDEO) {
        publication = entry.second;
        break;
      }
    }
    if (publication) {
      qInfo("[slot=%d cam=%s +%lldms] onTrackPublished: recovered video pub "
            "sid=%s from participant pid=%s",
            slotIndex_,
            cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
            static_cast<long long>(elapsedMs()), publication->sid().c_str(),
            event.participant->identity().c_str());
    } else {
      qInfo("[slot=%d cam=%s +%lldms] onTrackPublished: null pub, no video "
            "pub on participant pid=%s (watchdog will retry)",
            slotIndex_,
            cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
            static_cast<long long>(elapsedMs()),
            event.participant->identity().c_str());
      return;
    }
  }

  if (publication->kind() != livekit::TrackKind::KIND_VIDEO) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!room_ || stopRequested_.load()) {
    qInfo("[slot=%d cam=%s +%lldms] onTrackPublished: skip room=%d stopReq=%d",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()),
          room_ ? 1 : 0, stopRequested_.load() ? 1 : 0);
    return;
  }
  trySubscribePublication(publication, event.participant);
}

void LiveKitPlayer::onTrackSubscribed(
    livekit::Room & /* room */, const livekit::TrackSubscribedEvent &event) {
  qInfo("[slot=%d camera=%s] onTrackSubscribed track=%p part=%p pub=%p",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<void *>(event.track.get()),
        static_cast<void *>(event.participant),
        static_cast<void *>(event.publication.get()));
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
    livekit::Room & /* room */, const livekit::TrackUnsubscribedEvent &event) {
  qInfo("[slot=%d cam=%s +%lldms] onTrackUnsubscribed track=%p part=%p",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()),
        static_cast<void *>(event.track.get()),
        static_cast<void *>(event.participant));
  emit statusChanged(QStringLiteral("Video track unsubscribed"));
}

void LiveKitPlayer::onParticipantConnected(
    livekit::Room & /* room */,
    const livekit::ParticipantConnectedEvent &event) {
  qInfo("[slot=%d cam=%s +%lldms] onParticipantConnected part=%p pid=%s",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()),
        static_cast<void *>(event.participant),
        event.participant ? event.participant->identity().c_str() : "?");

  if (!event.participant) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!room_ || stopRequested_.load() || !receivingDesired_.load()) {
    return;
  }
  const auto pubs = event.participant->trackPublications();
  for (const auto &entry : pubs) {
    if (entry.second &&
        entry.second->kind() == livekit::TrackKind::KIND_VIDEO) {
      qInfo("[slot=%d cam=%s +%lldms] onParticipantConnected: existing video "
            "pub sid=%s, subscribing",
            slotIndex_,
            cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
            static_cast<long long>(elapsedMs()), entry.second->sid().c_str());
      trySubscribePublication(entry.second, event.participant);
      return;
    }
  }
}

void LiveKitPlayer::onDisconnected(
    livekit::Room & /* room */, const livekit::DisconnectedEvent & /* event */) {
  logConnectionState(livekit::ConnectionState::Disconnected);
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
  logConnectionState(event.state);
}

void LiveKitPlayer::logConnectionState(livekit::ConnectionState state) {
  const livekit::ConnectionState prev =
      lastLoggedConnectionState_.exchange(state);
  if (prev == state) {
    return;
  }
  const QString stateStr = connectionStateToString(state);
  qInfo("LiveKitPlayer[slot=%d camera=%s] connection state: %s -> %s",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        qUtf8Printable(connectionStateToString(prev)),
        qUtf8Printable(stateStr));
  emit statusChanged(QStringLiteral("Connection state: %1").arg(stateStr));
}

void LiveKitPlayer::onTrackSubscriptionFailed(
    livekit::Room & /* room */,
    const livekit::TrackSubscriptionFailedEvent &event) {
  qWarning("[slot=%d cam=%s +%lldms] onTrackSubscriptionFailed sid=%s err=%s",
           slotIndex_,
           cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
           static_cast<long long>(elapsedMs()), event.track_sid.c_str(),
           event.error.c_str());
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
    qInfo("[slot=%d cam=%s +%lldms] trySubscribe: skip pub=%d part=%d room=%d",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()),
          publication ? 1 : 0, participant ? 1 : 0, room_ ? 1 : 0);
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
    qInfo("[slot=%d cam=%s +%lldms] trySubscribe: skip receivingDesired=0 "
          "sid=%s",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()), publication->sid().c_str());
    return;
  }

  qInfo("[slot=%d cam=%s +%lldms] trySubscribe: setSubscribed(true) sid=%s "
        "pid=%s name=%s",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()), publication->sid().c_str(),
        participant->identity().c_str(), publication->name().c_str());
  applyVideoDimensions(publication);
  publication->setSubscribed(true);
}

void LiveKitPlayer::scanAndSubscribeExistingTracksLocked() {
  if (!room_) {
    qInfo("[slot=%d camera=%s] scanExisting: room_ is null", slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_));
    return;
  }
  const auto participants = room_->remoteParticipants();
  qInfo("[slot=%d camera=%s] scanExisting: %zu participant(s)", slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        participants.size());
  for (const auto &participant : participants) {
    if (!participant) {
      continue;
    }
    const auto pubs = participant->trackPublications();
    qInfo("[slot=%d camera=%s] scanExisting: participant=%s pubs=%zu",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          participant->identity().c_str(), pubs.size());
    for (const auto &entry : pubs) {
      const auto &publication = entry.second;
      if (!publication ||
          publication->kind() != livekit::TrackKind::KIND_VIDEO) {
        continue;
      }
      qInfo("[slot=%d camera=%s] scanExisting: subscribing sid=%s", slotIndex_,
            cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
            publication->sid().c_str());
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
    qInfo("[slot=%d cam=%s +%lldms] ensureVideoCallback: skip cbReg=%d room=%d "
          "pid=%s track=%s",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()),
          videoCallbackRegistered_ ? 1 : 0, room_ ? 1 : 0,
          activeParticipantIdentity_.empty() ? "-"
                                             : activeParticipantIdentity_.c_str(),
          activeTrackName_.empty() ? "-" : activeTrackName_.c_str());
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
  qInfo("[slot=%d cam=%s +%lldms] videoCallback registered pid=%s track=%s",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()),
        activeParticipantIdentity_.c_str(), activeTrackName_.c_str());
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
  const std::size_t partCount =
      room_ ? room_->remoteParticipants().size() : 0;
  qInfo("[slot=%d cam=%s +%lldms] onRoomConnected room=%d participants=%zu "
        "stopReq=%d recvDesired=%d",
        slotIndex_,
        cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
        static_cast<long long>(elapsedMs()),
        room_ ? 1 : 0, partCount,
        stopRequested_.load() ? 1 : 0,
        receivingDesired_.load() ? 1 : 0);
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

  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  framesReceived_.fetch_add(1);
  lastFrameMonoMs_.store(nowMs);
  if (!firstFrameLogged_.exchange(true)) {
    qInfo("[slot=%d cam=%s +%lldms] FIRST FRAME %dx%d",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()), frame.width(), frame.height());
  }

  if (frameInFlight_.exchange(true)) {
    return;
  }

  const int w = frame.width();
  const int h = frame.height();

  // Move the SDK frame into a shared holder so the GL thread can read its
  // plane pointers directly. The `const_cast` is safe here because the SDK's
  // reader loop (`VideoStream::read(VideoFrameEvent &ev)`) hands us a const
  // reference to a non-const local that it overwrites on the next iteration;
  // a moved-from VideoFrame is valid for the move-assignment that follows.
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
    qInfo("[slot=%d cam=%s +%lldms] worker: Connect() -> %s",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()), qUtf8Printable(url));
    emit statusChanged(QStringLiteral("Connecting to LiveKit at %1...").arg(url));
    auto room = std::make_unique<livekit::Room>();
    room->setDelegate(this);
    const bool ok =
        room->Connect(url.toStdString(), token.toStdString(), options);
    qInfo("[slot=%d cam=%s +%lldms] worker: Connect returned ok=%d url=%s",
          slotIndex_,
          cameraLabel_.isEmpty() ? "?" : qUtf8Printable(cameraLabel_),
          static_cast<long long>(elapsedMs()), ok ? 1 : 0,
          qUtf8Printable(url));
    if (!ok || stopRequested_.load()) {
      room->setDelegate(nullptr);
      return nullptr;
    }
    logConnectionState(livekit::ConnectionState::Connected);
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
