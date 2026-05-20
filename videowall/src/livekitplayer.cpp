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
#include <QTimer>
#include <QUrl>

#include <cstdarg>
#include <cstdio>
#include <sstream>
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

// Diagnostic logging: every line carries a monotonic millisecond stamp, the
// per-player instance tag (truncated cameraId), and the current thread id, so
// the SDK callback thread, the connect worker, and the Qt main thread can be
// disambiguated when reading the captured stderr stream.
void logLine(const char *tag, const char *fmt, ...) {
  using namespace std::chrono;
  const auto ms =
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count();
  std::ostringstream tid;
  tid << std::this_thread::get_id();
  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "%lld [LKP %s tid=%s] %s\n",
               static_cast<long long>(ms),
               tag ? tag : "?",
               tid.str().c_str(),
               body);
  std::fflush(stderr);
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

LiveKitPlayer::~LiveKitPlayer() {
  shutdownPlayback();
}

void LiveKitPlayer::startPlayback(const QString &bearerJwt,
                                  const QJsonObject &descriptor) {
  // Derive a short per-camera tag for log disambiguation. Falls back to the
  // object pointer if the JSON has no cameraId field.
  if (instanceTag_.empty()) {
    const QString cameraId =
        descriptor.value(QStringLiteral("cameraId")).toString();
    if (!cameraId.isEmpty()) {
      const QString tail =
          cameraId.right(6);
      instanceTag_ = tail.toStdString();
    } else {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%p", static_cast<void *>(this));
      instanceTag_ = buf;
    }
  }

  logLine(instanceTag_.c_str(),
          "startPlayback: receivingDesired<-true room_=%s stopRequested=%s",
          (room_ ? "set" : "null"),
          stopRequested_.load() ? "true" : "false");

  receivingDesired_.store(true);
  stopRequested_.store(false);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (room_) {
      logLine(instanceTag_.c_str(),
              "startPlayback: room_ already set -> resumeReceivingLocked");
      resumeReceivingLocked();
      emit statusChanged(QStringLiteral("Resuming on existing room connection..."));
      return;
    }
  }

  if (!nam_) {
    nam_ = new QNetworkAccessManager(this);
  }

  logLine(instanceTag_.c_str(), "startPlayback: requesting token");
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
  logLine(instanceTag_.c_str(),
          "pauseReceiving: room_=%s reply=%s worker=%s",
          (room_ ? "set" : "null"),
          (currentReply_ ? "live" : "null"),
          (worker_.joinable() ? "joinable" : "none"));
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
  firstFrameLogged_ = false;
}

void LiveKitPlayer::shutdownPlayback() {
  logLine(instanceTag_.c_str(),
          "shutdownPlayback: room_=%s reply=%s worker=%s",
          (room_ ? "set" : "null"),
          (currentReply_ ? "live" : "null"),
          (worker_.joinable() ? "joinable" : "none"));
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
  firstFrameLogged_ = false;
}

void LiveKitPlayer::onTokenReply(QNetworkReply *reply) {
  if (reply != currentReply_) {
    reply->deleteLater();
    return;
  }
  currentReply_ = nullptr;
  reply->deleteLater();

  if (stopRequested_.load() || !receivingDesired_.load()) {
    logLine(instanceTag_.c_str(),
            "onTokenReply: aborted (stopRequested=%s receivingDesired=%s)",
            stopRequested_.load() ? "true" : "false",
            receivingDesired_.load() ? "true" : "false");
    return;
  }

  const int httpStatus =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray payload = reply->readAll();

  logLine(instanceTag_.c_str(),
          "onTokenReply: httpStatus=%d replyErr=%d payloadBytes=%lld",
          httpStatus,
          static_cast<int>(reply->error()),
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
  logLine(instanceTag_.c_str(),
          "onTokenReply: launching connectWorker apiUrl=%s wssUrl=%s tokenLen=%d",
          apiUrl.toStdString().c_str(),
          wssUrl.toStdString().c_str(),
          token.size());
  worker_ = std::thread(&LiveKitPlayer::connectWorker, this, wssUrl, apiUrl,
                        token);
}

void LiveKitPlayer::onTrackPublished(
    livekit::Room & /* room */, const livekit::TrackPublishedEvent &event) {
  const bool pubNull = !event.publication;
  const bool partNull = !event.participant;
  logLine(instanceTag_.c_str(),
          "onTrackPublished: pub=%s part=%s kind=%d source=%d name=%s sid=%s",
          pubNull ? "null" : "ok",
          partNull ? "null" : event.participant->identity().c_str(),
          pubNull ? -1 : static_cast<int>(event.publication->kind()),
          pubNull ? -1 : static_cast<int>(event.publication->source()),
          pubNull ? "" : event.publication->name().c_str(),
          pubNull ? "" : event.publication->sid().c_str());

  // If the publication shared_ptr arrived null but we have a participant,
  // dump what publications the participant currently knows about so we can
  // tell whether the publication is reachable through participant->trackPublications().
  if (!partNull) {
    const auto &pubs = event.participant->trackPublications();
    logLine(instanceTag_.c_str(),
            "onTrackPublished: participant '%s' has trackPublications=%zu",
            event.participant->identity().c_str(), pubs.size());
    for (const auto &entry : pubs) {
      const auto &p = entry.second;
      if (!p) {
        logLine(instanceTag_.c_str(),
                "onTrackPublished:   pub map entry sid=%s value=null",
                entry.first.c_str());
        continue;
      }
      logLine(instanceTag_.c_str(),
              "onTrackPublished:   pub map sid=%s name='%s' kind=%d source=%d subscribed=%s",
              p->sid().c_str(), p->name().c_str(),
              static_cast<int>(p->kind()),
              static_cast<int>(p->source()),
              p->subscribed() ? "true" : "false");
    }
  }

  if (partNull) {
    logLine(instanceTag_.c_str(),
            "onTrackPublished: skip (null participant)");
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!room_ || stopRequested_.load()) {
    logLine(instanceTag_.c_str(),
            "onTrackPublished: DROP (room_=%s stopRequested=%s)",
            room_ ? "set" : "null",
            stopRequested_.load() ? "true" : "false");
    return;
  }

  // The SDK can deliver TrackPublishedEvent with event.publication == nullptr
  // even though the publication is already in the participant's
  // trackPublications() map. Fall back to scanning the participant for an
  // unsubscribed video publication so we don't lose the only notification.
  std::shared_ptr<livekit::RemoteTrackPublication> pub = event.publication;
  if (!pub || pub->kind() != livekit::TrackKind::KIND_VIDEO) {
    for (const auto &entry : event.participant->trackPublications()) {
      const auto &candidate = entry.second;
      if (candidate &&
          candidate->kind() == livekit::TrackKind::KIND_VIDEO &&
          !candidate->subscribed()) {
        pub = candidate;
        logLine(instanceTag_.c_str(),
                "onTrackPublished: fallback picked pub sid=%s from participant map",
                pub->sid().c_str());
        break;
      }
    }
  }
  if (!pub || pub->kind() != livekit::TrackKind::KIND_VIDEO) {
    logLine(instanceTag_.c_str(),
            "onTrackPublished: no video publication available, skipping");
    return;
  }

  logLine(instanceTag_.c_str(),
          "onTrackPublished: -> trySubscribePublication sid=%s",
          pub->sid().c_str());
  trySubscribePublication(pub, event.participant);
}

void LiveKitPlayer::onTrackSubscribed(
    livekit::Room & /* room */, const livekit::TrackSubscribedEvent &event) {
  logLine(instanceTag_.c_str(),
          "onTrackSubscribed: track=%s part=%s pub=%s",
          event.track ? "ok" : "null",
          event.participant ? event.participant->identity().c_str() : "null",
          event.publication ? event.publication->sid().c_str() : "null");
  if (!event.track || event.track->kind() != livekit::TrackKind::KIND_VIDEO ||
      event.participant == nullptr) {
    logLine(instanceTag_.c_str(),
            "onTrackSubscribed: skip non-video or null fields");
    return;
  }

  if (event.publication) {
    logLine(instanceTag_.c_str(),
            "onTrackSubscribed: pub sid=%s simulcast=%s pubWxH=%ux%u mime=%s",
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
      logLine(instanceTag_.c_str(),
              "onTrackSubscribed: skip state apply (room_=%s stopRequested=%s)",
              room_ ? "set" : "null",
              stopRequested_.load() ? "true" : "false");
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

  logLine(instanceTag_.c_str(),
          "onTrackSubscribed: now playing track='%s' from='%s'",
          trackName.c_str(), participantIdentity.c_str());
  emit statusChanged(QStringLiteral("Playing track '%1' from '%2'")
                         .arg(QString::fromStdString(trackName),
                              QString::fromStdString(participantIdentity)));
}

void LiveKitPlayer::onTrackUnsubscribed(
    livekit::Room & /* room */, const livekit::TrackUnsubscribedEvent &event) {
  logLine(instanceTag_.c_str(),
          "onTrackUnsubscribed: part=%s track=%s activePart='%s' activeTrack='%s'",
          event.participant ? event.participant->identity().c_str() : "null",
          event.track ? event.track->name().c_str() : "null",
          activeParticipantIdentity_.c_str(),
          activeTrackName_.c_str());
  emit statusChanged(QStringLiteral("Video track unsubscribed"));
}

void LiveKitPlayer::onDisconnected(
    livekit::Room & /* room */, const livekit::DisconnectedEvent &event) {
  logLine(instanceTag_.c_str(),
          "onDisconnected: reason=%d activePart='%s' activeTrack='%s' "
          "-> resetting room_ from inside delegate callback",
          static_cast<int>(event.reason),
          activeParticipantIdentity_.c_str(),
          activeTrackName_.c_str());
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
  logLine(instanceTag_.c_str(),
          "onConnectionStateChanged: state=%s",
          connectionStateToString(event.state).toStdString().c_str());
  emit statusChanged(QStringLiteral("Connection state: %1")
                         .arg(connectionStateToString(event.state)));
}

void LiveKitPlayer::onTrackSubscriptionFailed(
    livekit::Room & /* room */,
    const livekit::TrackSubscriptionFailedEvent &event) {
  logLine(instanceTag_.c_str(),
          "onTrackSubscriptionFailed: sid=%s error=%s",
          event.track_sid.c_str(), event.error.c_str());
  emit errorOccurred(
      QStringLiteral("Track subscription failed for SID %1: %2")
          .arg(QString::fromStdString(event.track_sid),
               QString::fromStdString(event.error)));
}

void LiveKitPlayer::onParticipantConnected(
    livekit::Room & /* room */,
    const livekit::ParticipantConnectedEvent &event) {
  logLine(instanceTag_.c_str(),
          "onParticipantConnected: identity=%s kind=%d",
          event.participant ? event.participant->identity().c_str() : "null",
          event.participant ? static_cast<int>(event.participant->kind()) : -1);
  if (event.participant) {
    const auto &pubs = event.participant->trackPublications();
    logLine(instanceTag_.c_str(),
            "onParticipantConnected: trackPublications=%zu",
            pubs.size());
    for (const auto &entry : pubs) {
      const auto &p = entry.second;
      if (!p) {
        logLine(instanceTag_.c_str(),
                "onParticipantConnected:   pub sid=%s value=null",
                entry.first.c_str());
        continue;
      }
      logLine(instanceTag_.c_str(),
              "onParticipantConnected:   pub sid=%s name='%s' kind=%d source=%d subscribed=%s",
              p->sid().c_str(), p->name().c_str(),
              static_cast<int>(p->kind()),
              static_cast<int>(p->source()),
              p->subscribed() ? "true" : "false");
    }
  }
}

void LiveKitPlayer::onParticipantDisconnected(
    livekit::Room & /* room */,
    const livekit::ParticipantDisconnectedEvent &event) {
  logLine(instanceTag_.c_str(),
          "onParticipantDisconnected: identity=%s",
          event.participant ? event.participant->identity().c_str() : "null");
}

void LiveKitPlayer::onReconnecting(livekit::Room & /* room */,
                                   const livekit::ReconnectingEvent & /* event */) {
  logLine(instanceTag_.c_str(), "onReconnecting");
}

void LiveKitPlayer::onReconnected(livekit::Room & /* room */,
                                  const livekit::ReconnectedEvent & /* event */) {
  logLine(instanceTag_.c_str(), "onReconnected");
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
  logLine(instanceTag_.c_str(),
          "trySubscribePublication: pub=%s part=%s room_=%s subscribed=%s",
          publication ? publication->sid().c_str() : "null",
          participant ? participant->identity().c_str() : "null",
          room_ ? "set" : "null",
          publication ? (publication->subscribed() ? "true" : "false") : "?");
  if (!publication || !participant || !room_) {
    logLine(instanceTag_.c_str(),
            "trySubscribePublication: early return null");
    return;
  }
  if (publication->kind() != livekit::TrackKind::KIND_VIDEO) {
    logLine(instanceTag_.c_str(),
            "trySubscribePublication: skip non-video kind=%d",
            static_cast<int>(publication->kind()));
    return;
  }

  activeParticipantIdentity_ = participant->identity();
  activeTrackName_ = publication->name();
  activeTrackSource_ = publication->source();
  activePublication_ = publication;

  if (!receivingDesired_.load()) {
    logLine(instanceTag_.c_str(),
            "trySubscribePublication: skip (receivingDesired=false)");
    return;
  }

  applyVideoDimensions(publication);
  logLine(instanceTag_.c_str(),
          "trySubscribePublication: calling setSubscribed(true) sid=%s name='%s'",
          publication->sid().c_str(), publication->name().c_str());
  publication->setSubscribed(true);
  logLine(instanceTag_.c_str(),
          "trySubscribePublication: setSubscribed(true) returned, subscribed=%s",
          publication->subscribed() ? "true" : "false");
}

void LiveKitPlayer::scanAndSubscribeExistingTracksLocked() {
  if (!room_) {
    logLine(instanceTag_.c_str(), "scan: room_ is null");
    return;
  }
  const auto participants = room_->remoteParticipants();
  logLine(instanceTag_.c_str(), "scan: remoteParticipants=%zu",
          participants.size());
  for (const auto &participant : participants) {
    if (!participant) {
      logLine(instanceTag_.c_str(), "scan: null participant entry");
      continue;
    }
    const auto &pubs = participant->trackPublications();
    logLine(instanceTag_.c_str(),
            "scan: participant='%s' pubs=%zu",
            participant->identity().c_str(), pubs.size());
    for (const auto &entry : pubs) {
      const auto &publication = entry.second;
      if (!publication) {
        logLine(instanceTag_.c_str(),
                "scan:   pub null entry");
        continue;
      }
      logLine(instanceTag_.c_str(),
              "scan:   pub sid=%s name='%s' kind=%d source=%d subscribed=%s",
              publication->sid().c_str(),
              publication->name().c_str(),
              static_cast<int>(publication->kind()),
              static_cast<int>(publication->source()),
              publication->subscribed() ? "true" : "false");
      if (publication->kind() != livekit::TrackKind::KIND_VIDEO) {
        continue;
      }
      logLine(instanceTag_.c_str(),
              "scan:   -> trySubscribePublication for sid=%s",
              publication->sid().c_str());
      trySubscribePublication(publication, participant.get());
      return;
    }
  }
  logLine(instanceTag_.c_str(),
          "scan: no video publication found to subscribe");
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
  logLine(instanceTag_.c_str(),
          "ensureVideoCallbackRegisteredLocked: registered=%s room_=%s part='%s' track='%s'",
          videoCallbackRegistered_ ? "true" : "false",
          room_ ? "set" : "null",
          activeParticipantIdentity_.c_str(),
          activeTrackName_.c_str());
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
  logLine(instanceTag_.c_str(),
          "ensureVideoCallbackRegisteredLocked: setOnVideoFrameCallback registered");
}

void LiveKitPlayer::clearActiveVideoCallbackLocked() {
  logLine(instanceTag_.c_str(),
          "clearActiveVideoCallbackLocked: room_=%s part='%s' track='%s'",
          room_ ? "set" : "null",
          activeParticipantIdentity_.c_str(),
          activeTrackName_.c_str());
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
  logLine(instanceTag_.c_str(),
          "onRoomConnected: room_=%s receivingDesired=%s stopRequested=%s",
          room_ ? "set" : "null",
          receivingDesired_.load() ? "true" : "false",
          stopRequested_.load() ? "true" : "false");
  bool needRetry = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load() || !room_) {
      logLine(instanceTag_.c_str(),
              "onRoomConnected: early return");
      return;
    }
    if (receivingDesired_.load()) {
      resumeReceivingLocked();
    }
    // Some publishers' track publications appear in the participant's map
    // shortly after onRoomConnected runs, without ever firing a delegate
    // event we can react to. Schedule a small number of delayed re-scans
    // so we don't strand those slots indefinitely.
    needRetry = !activePublication_.lock();
  }
  if (needRetry) {
    QTimer::singleShot(250, this, [this]() { retryDelayedScan(3); });
  }
  emit statusChanged(QStringLiteral("Connected. Waiting for remote video track..."));
}

void LiveKitPlayer::retryDelayedScan(int remaining) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load() || !room_) {
      return;
    }
    if (activePublication_.lock()) {
      logLine(instanceTag_.c_str(),
              "retryDelayedScan: already subscribed, stopping retries");
      return;
    }
    logLine(instanceTag_.c_str(),
            "retryDelayedScan: remaining=%d, re-scanning", remaining);
    scanAndSubscribeExistingTracksLocked();
    if (activePublication_.lock()) {
      logLine(instanceTag_.c_str(),
              "retryDelayedScan: subscribed via retry scan");
      return;
    }
  }
  if (remaining > 1) {
    const int nextDelayMs = (remaining == 3) ? 750 : 1500;
    QTimer::singleShot(nextDelayMs, this,
                       [this, remaining]() { retryDelayedScan(remaining - 1); });
  } else {
    logLine(instanceTag_.c_str(),
            "retryDelayedScan: gave up after retries");
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

  if (!firstFrameLogged_) {
    firstFrameLogged_ = true;
    logLine(instanceTag_.c_str(),
            "emitFrameFromLiveKit: FIRST FRAME %dx%d type=%d",
            frame.width(), frame.height(),
            static_cast<int>(frame.type()));
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
  logLine(instanceTag_.c_str(),
          "connectWorker: enter wssUrl=%s httpsUrl=%s",
          wssUrl.toStdString().c_str(),
          httpsUrl.toStdString().c_str());
  static std::once_flag sdkInitFlag;
  std::call_once(sdkInitFlag, []() {
    if (livekit::initialize(livekit::LogLevel::Info, livekit::LogSink::kConsole)) {
      sdkInitialized_.store(true);
    }
  });

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (room_) {
      logLine(instanceTag_.c_str(),
              "connectWorker: room_ already set -> queue onRoomConnected");
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
      logLine(instanceTag_.c_str(),
              "tryConnect: stopRequested before connect");
      return nullptr;
    }
    logLine(instanceTag_.c_str(),
            "tryConnect: creating Room and setDelegate(this) url=%s",
            url.toStdString().c_str());
    emit statusChanged(QStringLiteral("Connecting to LiveKit at %1...").arg(url));
    auto room = std::make_unique<livekit::Room>();
    room->setDelegate(this);
    logLine(instanceTag_.c_str(),
            "tryConnect: calling Room::Connect()");
    const bool ok =
        room->Connect(url.toStdString(), token.toStdString(), options);
    logLine(instanceTag_.c_str(),
            "tryConnect: Connect returned ok=%s stopRequested=%s",
            ok ? "true" : "false",
            stopRequested_.load() ? "true" : "false");
    if (!ok || stopRequested_.load()) {
      room->setDelegate(nullptr);
      return nullptr;
    }
    return room;
  };

  auto room = tryConnect(wssUrl);
  if (!room && !stopRequested_.load() && wssUrl != httpsUrl) {
    logLine(instanceTag_.c_str(),
            "connectWorker: wss failed -> retrying https");
    emit statusChanged(
        QStringLiteral("wss connect failed; retrying https..."));
    room = tryConnect(httpsUrl);
  }

  if (!room) {
    logLine(instanceTag_.c_str(),
            "connectWorker: connect FAILED (stopRequested=%s)",
            stopRequested_.load() ? "true" : "false");
    if (!stopRequested_.load()) {
      emit errorOccurred(QStringLiteral("Failed to connect to LiveKit room"));
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopRequested_.load()) {
      logLine(instanceTag_.c_str(),
              "connectWorker: stopRequested after connect, discarding room");
      room->setDelegate(nullptr);
      return;
    }
    logLine(instanceTag_.c_str(),
            "connectWorker: assigning room_ = std::move(room)");
    room_ = std::move(room);
  }

  logLine(instanceTag_.c_str(),
          "connectWorker: queuing onRoomConnected");
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
