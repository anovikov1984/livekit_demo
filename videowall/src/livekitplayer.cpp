#include "livekitplayer.h"

#include <cstring>
#include <mutex>

namespace {
constexpr int kMinFrameDimension = 1;

void copyPlane(QImage &dst, const livekit::VideoPlaneInfo &src,
               int planeW, int planeH) {
  const std::uint8_t *srcPtr =
      reinterpret_cast<const std::uint8_t *>(src.data_ptr);
  std::uint8_t *dstPtr = dst.bits();
  const int dstStride = dst.bytesPerLine();
  const int srcStride = static_cast<int>(src.stride);
  if (srcStride == dstStride && srcStride == planeW) {
    std::memcpy(dstPtr, srcPtr, static_cast<std::size_t>(planeW) * planeH);
  } else {
    for (int row = 0; row < planeH; ++row) {
      std::memcpy(dstPtr + row * dstStride, srcPtr + row * srcStride,
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

  if (event.publication) {
    emit statusChanged(QStringLiteral(
        "Publication: sid=%1, simulcast=%2, published=%3x%4, mime=%5")
        .arg(QString::fromStdString(event.publication->sid()))
        .arg(event.publication->simulcasted() ? "true" : "false")
        .arg(event.publication->width())
        .arg(event.publication->height())
        .arg(QString::fromStdString(event.publication->mimeType())));

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

  YuvFrame &buf = frameBuffers_[writeIdx_];
  const int w = frame.width();
  const int h = frame.height();
  const int cw = w / 2;
  const int ch = h / 2;

  if (buf.y.width() != w || buf.y.height() != h)
    buf.y = QImage(w, h, QImage::Format_Grayscale8);
  if (buf.u.width() != cw || buf.u.height() != ch) {
    buf.u = QImage(cw, ch, QImage::Format_Grayscale8);
    buf.v = QImage(cw, ch, QImage::Format_Grayscale8);
  }

  copyPlane(buf.y, planes[0], w, h);
  copyPlane(buf.u, planes[1], cw, ch);
  copyPlane(buf.v, planes[2], cw, ch);

  emit frameReady(buf);

  emit statusChanged(QStringLiteral("Receiving %1x%2")
                         .arg(frame.width())
                         .arg(frame.height()));

  writeIdx_ = (writeIdx_ + 1) % static_cast<int>(frameBuffers_.size());
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
