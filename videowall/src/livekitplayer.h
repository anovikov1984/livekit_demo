#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "livekit/livekit.h"

class LiveKitPlayer : public QObject, public livekit::RoomDelegate {
  Q_OBJECT

public:
  explicit LiveKitPlayer(QObject *parent = nullptr);
  ~LiveKitPlayer() override;

  void startPlayback(const QString &apiUrl, const QString &token);
  void stopPlayback();
  void clearFrameInFlight() { frameInFlight_.store(false); }

signals:
  void frameReady(const QImage &frame);
  void statusChanged(const QString &status);
  void errorOccurred(const QString &errorMessage);

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

  static std::atomic_bool sdkInitialized_;
};
