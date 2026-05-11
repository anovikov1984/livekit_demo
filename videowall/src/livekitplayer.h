#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "livekit/livekit.h"
#include "yuvbufferpool.h"
#include "yuvframe.h"

class QNetworkAccessManager;
class QNetworkReply;

class LiveKitPlayer : public QObject, public livekit::RoomDelegate {
  Q_OBJECT

public:
  explicit LiveKitPlayer(QObject *parent = nullptr);
  ~LiveKitPlayer() override;

  // Fetch a LiveKit token via the create-token API using the given Bearer JWT
  // and camera descriptor body, then connect (wss first, https fallback).
  void startPlayback(const QString &bearerJwt, const QJsonObject &descriptor);
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
  void onTokenReply(QNetworkReply *reply);
  void connectWorker(QString wssUrl, QString httpsUrl, QString token);
  static QString connectionStateToString(livekit::ConnectionState state);

  std::mutex mutex_;
  std::unique_ptr<livekit::Room> room_;
  std::thread worker_;
  std::atomic_bool stopRequested_{false};
  std::atomic_bool frameInFlight_{false};
  std::string activeParticipantIdentity_;
  std::string activeTrackName_;

  // FPS tracking — sampled ~1 Hz in emitFrameFromLiveKit.
  int fpsFrameCount_{0};
  std::chrono::steady_clock::time_point fpsWindowStart_{};

  // Plane-buffer recycler. Per-frame plane allocations come from here so we
  // amortize the heap churn at 30 fps × 3 planes per stream.
  std::shared_ptr<YuvBufferPool> framePool_{std::make_shared<YuvBufferPool>()};

  // HTTP token fetch (lives on the GUI thread).
  QNetworkAccessManager *nam_{nullptr};
  QNetworkReply *currentReply_{nullptr};

  static std::atomic_bool sdkInitialized_;
  static std::atomic<int> requestedWidth_;
  static std::atomic<int> requestedHeight_;
};
