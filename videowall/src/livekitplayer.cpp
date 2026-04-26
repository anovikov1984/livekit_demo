#include "livekitplayer.h"

#include <mutex>

namespace {
constexpr int kMinFrameDimension = 1;
}

std::atomic_bool LiveKitPlayer::sdkInitialized_{false};

LiveKitPlayer::LiveKitPlayer(QObject *parent) : QObject(parent) {}

LiveKitPlayer::~LiveKitPlayer() {
  stopPlayback();
}

void LiveKitPlayer::startPlayback(const QString &apiUrl, const QString &token) {
  stopPlayback();

  stopRequested_.store(false);
  worker_ = std::thread(&LiveKitPlayer::connectWorker, this, apiUrl, token);
}

void LiveKitPlayer::stopPlayback() {
  stopRequested_.store(true);

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

void LiveKitPlayer::onTrackSubscribed(
    livekit::Room &room, const livekit::TrackSubscribedEvent &event) {
  if (!event.track || event.track->kind() != livekit::TrackKind::KIND_VIDEO ||
      event.participant == nullptr) {
    return;
  }

  livekit::VideoStream::Options options;
  options.capacity = 2;
  options.format = livekit::VideoBufferType::RGBA;

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
  if (frame.width() < kMinFrameDimension || frame.height() < kMinFrameDimension ||
      frame.data() == nullptr) {
    return;
  }

  const QImage view(frame.data(), frame.width(), frame.height(),
                    QImage::Format_RGBA8888);
  const QImage copied = view.copy();
  emit frameReady(copied);
}

void LiveKitPlayer::connectWorker(QString apiUrl, QString token) {
  emit statusChanged(QStringLiteral("Connecting to LiveKit..."));

  static std::once_flag sdkInitFlag;
  std::call_once(sdkInitFlag, []() {
    if (livekit::initialize(livekit::LogLevel::Info, livekit::LogSink::kConsole)) {
      sdkInitialized_.store(true);
    }
  });

  auto room = std::make_unique<livekit::Room>();
  room->setDelegate(this);

  livekit::RoomOptions options;
  options.auto_subscribe = true;
  options.dynacast = false;

  const bool connected =
      room->Connect(apiUrl.toStdString(), token.toStdString(), options);

  if (!connected) {
    emit errorOccurred(QStringLiteral("Failed to connect to LiveKit room"));
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
