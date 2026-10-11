/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <memory>

#include <mgba/internal/debugger/profiler.h>

namespace QGBA {

class CoreController;
class ProfileData;

// The profiler for one running game. It belongs to the game's CoreController, so a recording
// lasts as long as the game does.
class ProfilerController : public QObject {
Q_OBJECT

public:
	struct LiveFunction {
		QString name;
		quint64 selfCycles;
	};

	struct LiveSummary {
		quint64 cycles = 0;
		size_t frames = 0;
		double frameBusy = 0;
		QVector<LiveFunction> top;
	};

	ProfilerController(CoreController* controller, QObject* parent = nullptr);
	~ProfilerController();

	CoreController* coreController() const { return m_controller; }
	bool isRecording() const { return m_profiler.recording; }
	QString symbolSource() const;
	unsigned symbolCount() const;
	QString idleOptimization() const;
	uint32_t romCrc32() const;

	bool loadSymbols(const QString& path);
	// The profile so far, or the last recording's
	std::shared_ptr<ProfileData> snapshot();

public slots:
	bool start();
	// Returns the finished recording
	std::shared_ptr<ProfileData> stop();
	void reset();

signals:
	void recordingChanged(bool recording);
	// A recording ended, by stop() or because the game is closing
	void finished(std::shared_ptr<ProfileData>);
	void symbolsChanged();
	void liveUpdated(const LiveSummary&);

private slots:
	void updateLive();

private:
	void attach();

	CoreController* m_controller;
	mDebuggerProfiler m_profiler{};
	bool m_attached = false;
	uint32_t m_crc32 = 0;
	QTimer m_liveTimer;
};

}
