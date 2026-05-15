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

  void startPlayback(const QString &bearerJwt, const QJsonObject &descriptor);
  void pauseReceiving();
  void shutdownPlayback();
  void clearFrameInFlight() { frameInFlight_.store(false); }

  static void setRequestedDimensions(int width, int height);

signals:
  void frameReady(const YuvFrame &frame);
  void statusChanged(const QString &status);
  void errorOccurred(const QString &errorMessage);
  void mimeTypeReceived(const std::string &mime);

protected:
  void onTrackPublished(livekit::Room &room,
                        const livekit::TrackPublishedEvent &event) override;
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
  void destroyRoomLocked();
  void pauseReceivingLocked();
  void resumeReceivingLocked();
  void scanAndSubscribeExistingTracksLocked();
  void trySubscribePublication(
      const std::shared_ptr<livekit::RemoteTrackPublication> &publication,
      livekit::RemoteParticipant *participant);
  void applyVideoDimensions(
      const std::shared_ptr<livekit::RemoteTrackPublication> &publication);
  void ensureVideoCallbackRegisteredLocked();
  void emitFrameFromLiveKit(const livekit::VideoFrame &frame);
  void onTokenReply(QNetworkReply *reply);
  void connectWorker(QString wssUrl, QString httpsUrl, QString token);
  static QString connectionStateToString(livekit::ConnectionState state);

  std::mutex mutex_;
  std::unique_ptr<livekit::Room> room_;
  std::thread worker_;
  std::atomic_bool receivingDesired_{false};
  std::atomic_bool stopRequested_{false};
  std::atomic_bool frameInFlight_{false};
  bool videoCallbackRegistered_{false};
  std::string activeParticipantIdentity_;
  std::string activeTrackName_;
  livekit::TrackSource activeTrackSource_{livekit::TrackSource::SOURCE_UNKNOWN};
  std::weak_ptr<livekit::RemoteTrackPublication> activePublication_;

  int fpsFrameCount_{0};
  std::chrono::steady_clock::time_point fpsWindowStart_{};

  std::shared_ptr<YuvBufferPool> framePool_{std::make_shared<YuvBufferPool>()};

  QNetworkAccessManager *nam_{nullptr};
  QNetworkReply *currentReply_{nullptr};

  static std::atomic_bool sdkInitialized_;
  static std::atomic<int> requestedWidth_;
  static std::atomic<int> requestedHeight_;

private slots:
  void onRoomConnected();
};
