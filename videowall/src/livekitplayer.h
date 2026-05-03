#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "livekit/livekit.h"
#include "yuvframe.h"

class LiveKitPlayer : public QObject, public livekit::RoomDelegate {
  Q_OBJECT

public:
  explicit LiveKitPlayer(QObject *parent = nullptr);
  ~LiveKitPlayer() override;

  void startPlayback(const QString &apiUrl, const QString &token);
  void stopPlayback();
  void clearFrameInFlight() { frameInFlight_.store(false); }

  // Process-wide override for the dimensions requested via
  // RemoteTrackPublication::setVideoDimensions on each new subscription.
  // Set once at startup (e.g. from main()) — applies to all subsequent
  // onTrackSubscribed callbacks. (0, 0) means "don't override".
  static void setRequestedDimensions(int width, int height);

signals:
  void frameReady(const YuvFrame &frame);
  void statusChanged(const QString &status);
  void errorOccurred(const QString &errorMessage);
  void mimeTypeReceived(const std::string &mime);

protected:
  void onTrackSubscribed(livekit::Room &room,
                         const livekit::TrackSubscribedEvent &event) override;
  void onTrackUnsubscribed(livekit::Room &room,
                           const livekit::TrackUnsubscribedEvent &event) override;
  void onDisconnected(livekit::Room &room,
                      const livekit::DisconnectedEvent &event) override;
  void onConnectionStateChanged(
      livekit::Room &room,
      const livekit::ConnectionStateChangedEvent &event) override;
  void onTrackSubscriptionFailed(
      livekit::Room &room,
      const livekit::TrackSubscriptionFailedEvent &event) override;

private:
  void clearActiveVideoCallbackLocked();
  void emitFrameFromLiveKit(const livekit::VideoFrame &frame);
  void connectWorker(QString apiUrl, QString token);
  static QString connectionStateToString(livekit::ConnectionState state);

  std::mutex mutex_;
  std::unique_ptr<livekit::Room> room_;
  std::thread worker_;
  std::atomic_bool stopRequested_{false};
  std::atomic_bool frameInFlight_{false};
  std::string activeParticipantIdentity_;
  std::string activeTrackName_;
  // Triple-buffered I420 frames to avoid QImage COW detach.
  std::array<YuvFrame, 3> frameBuffers_;
  int writeIdx_{0};

  // FPS tracking — sampled ~1 Hz in emitFrameFromLiveKit.
  int fpsFrameCount_{0};
  std::chrono::steady_clock::time_point fpsWindowStart_{};

  static std::atomic_bool sdkInitialized_;
  static std::atomic<int> requestedWidth_;
  static std::atomic<int> requestedHeight_;
};
