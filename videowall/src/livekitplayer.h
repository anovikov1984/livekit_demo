#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "livekit/livekit.h"
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
  void onParticipantConnected(
      livekit::Room &room,
      const livekit::ParticipantConnectedEvent &event) override;
  void onParticipantDisconnected(
      livekit::Room &room,
      const livekit::ParticipantDisconnectedEvent &event) override;
  void onTrackPublished(livekit::Room &room,
                        const livekit::TrackPublishedEvent &event) override;
  void onTrackSubscribed(livekit::Room &room,
                         const livekit::TrackSubscribedEvent &event) override;
  void onTrackUnsubscribed(livekit::Room &room,
                           const livekit::TrackUnsubscribedEvent &event) override;
  void onTrackSubscriptionFailed(
      livekit::Room &room,
      const livekit::TrackSubscriptionFailedEvent &event) override;
  void onDisconnected(livekit::Room &room,
                      const livekit::DisconnectedEvent &event) override;
  void onConnectionStateChanged(
      livekit::Room &room,
      const livekit::ConnectionStateChangedEvent &event) override;

private:
  struct SubscribedTrack {
    std::shared_ptr<livekit::RemoteTrackPublication> publication;
    std::string participantIdentity;
    std::string trackName;
  };

  void onTokenReply(QNetworkReply *reply);
  void connectWorker(QString wssUrl, QString httpsUrl, QString token);
  void performTokenFetchAndConnect();
  void attemptAutoReconnectLocked();
  void subscribeParticipantLocked(livekit::RemoteParticipant *participant);
  void registerTrackLocked(
      const std::shared_ptr<livekit::RemoteTrackPublication> &publication,
      const std::string &participantIdentity);
  void registerFrameCallbackLocked(const std::string &participantIdentity,
                                   const std::string &trackName);
  void applyVideoDimensions(
      const std::shared_ptr<livekit::RemoteTrackPublication> &publication);
  void emitFrameFromLiveKit(const livekit::VideoFrame &frame);
  static QString connectionStateToString(livekit::ConnectionState state);

  std::mutex mutex_;
  std::unique_ptr<livekit::Room> room_;
  std::thread worker_;
  std::atomic_bool receivingDesired_{false};
  std::atomic_bool stopRequested_{false};
  std::atomic_bool frameInFlight_{false};

  std::vector<SubscribedTrack> streams_;

  int fpsFrameCount_{0};
  std::chrono::steady_clock::time_point fpsWindowStart_{};

  QNetworkAccessManager *nam_{nullptr};
  QNetworkReply *currentReply_{nullptr};

  std::string descriptorCameraId_;
  int publisherCheckAttempt_{0};
  QString bearerJwt_;
  QJsonObject descriptor_;
  int autoReconnectAttempt_{0};

  static std::atomic_bool sdkInitialized_;
  static std::atomic<int> requestedWidth_;
  static std::atomic<int> requestedHeight_;

private slots:
  void onRoomConnected();
  void logRoomSnapshot();
  void publisherJoinCheck();
};
