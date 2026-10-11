/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ProfilerController.h"
#include "moc_ProfilerController.cpp"

#include <QDateTime>

#include "CoreController.h"
#include "ProfileData.h"
#include "VFileDevice.h"

#include <mgba/core/core.h>
#include <mgba-util/vfs.h>

using namespace QGBA;

static const int LIVE_INTERVAL_MS = 1000;
static const size_t LIVE_FUNCTIONS = 10;

ProfilerController::ProfilerController(CoreController* controller, QObject* parent)
	: QObject(parent)
	, m_controller(controller)
{
	mDebuggerProfilerInit(&m_profiler);
	mCore* core = m_controller->thread()->core;
	core->checksum(core, &m_crc32, mCHECKSUM_CRC32);
	m_liveTimer.setInterval(LIVE_INTERVAL_MS);
	connect(&m_liveTimer, &QTimer::timeout, this, &ProfilerController::updateLive);
}

ProfilerController::~ProfilerController() {
	m_liveTimer.stop();
	if (m_attached) {
		CoreController::Interrupter interrupter(m_controller);
		if (m_profiler.recording) {
			mDebuggerProfilerStop(&m_profiler);
		}
		m_controller->detachDebuggerModule(&m_profiler.d);
	}
	mDebuggerProfilerDeinit(&m_profiler);
}

QString ProfilerController::symbolSource() const {
	return QString::fromUtf8(m_profiler.profile.symbolSource);
}

unsigned ProfilerController::symbolCount() const {
	return m_profiler.profile.symbolCount;
}

QString ProfilerController::idleOptimization() const {
	return QString::fromUtf8(m_profiler.profile.idleOptimization);
}

uint32_t ProfilerController::romCrc32() const {
	return m_crc32;
}

void ProfilerController::attach() {
	if (m_attached) {
		return;
	}
	m_controller->attachDebuggerModule(&m_profiler.d, false);
	m_attached = true;
}

bool ProfilerController::loadSymbols(const QString& path) {
	if (m_profiler.recording) {
		return false;
	}
	VFile* vf = VFileDevice::open(path, O_RDONLY);
	if (!vf) {
		return false;
	}
	attach();
	CoreController::Interrupter interrupter(m_controller);
	bool success = mDebuggerProfilerLoadSymbols(&m_profiler, vf, path.toUtf8().constData());
	interrupter.resume();
	vf->close(vf);
	if (success) {
		emit symbolsChanged();
	}
	return success;
}

bool ProfilerController::start() {
	if (m_profiler.recording) {
		return true;
	}
	attach();
	CoreController::Interrupter interrupter(m_controller);
	bool autoloaded = false;
	if (!m_profiler.profile.symbolSource) {
		autoloaded = mDebuggerProfilerAutoloadSymbols(&m_profiler);
	}
	// Each recording starts from nothing
	mProfileClear(&m_profiler.profile);
	bool started = mDebuggerProfilerStart(&m_profiler);
	interrupter.resume();
	if (autoloaded) {
		emit symbolsChanged();
	}
	if (started) {
		m_liveTimer.start();
		emit recordingChanged(true);
	}
	return started;
}

std::shared_ptr<ProfileData> ProfilerController::stop() {
	if (!m_profiler.recording) {
		return nullptr;
	}
	m_liveTimer.stop();
	CoreController::Interrupter interrupter(m_controller);
	mDebuggerProfilerStop(&m_profiler);
	interrupter.resume();
	std::shared_ptr<ProfileData> data = snapshot();
	emit recordingChanged(false);
	emit finished(data);
	return data;
}

void ProfilerController::reset() {
	CoreController::Interrupter interrupter(m_controller);
	mDebuggerProfilerReset(&m_profiler);
}

std::shared_ptr<ProfileData> ProfilerController::snapshot() {
	// A copy through the native format, so the views have data of their own that the next
	// recording doesn't touch
	VFile* vf = VFileMemChunk(nullptr, 0);
	{
		CoreController::Interrupter interrupter(m_controller);
		mDebuggerProfilerLock(&m_profiler);
		if (m_profiler.recording || !m_profiler.profile.finished) {
			mProfileFinish(&m_profiler.profile);
		}
		mProfileSave(&m_profiler.profile, vf);
		mDebuggerProfilerUnlock(&m_profiler);
	}
	QString source = tr("Recording at %1").arg(QDateTime::currentDateTime().toString(Qt::TextDate));
	std::shared_ptr<ProfileData> data = ProfileData::load(vf, source);
	vf->close(vf);
	return data;
}

void ProfilerController::updateLive() {
	if (!m_profiler.recording) {
		m_liveTimer.stop();
		return;
	}
	LiveSummary summary;
	CoreController::Interrupter interrupter(m_controller);
	mDebuggerProfilerLock(&m_profiler);
	mProfile* profile = &m_profiler.profile;
	mProfileFinish(profile);
	summary.cycles = profile->totalCycles;
	mProfileFrameSummary frames;
	mProfileSummarizeFrames(profile, &frames);
	summary.frames = frames.count;
	if (frames.cycles) {
		summary.frameBusy = frames.busyCycles * 100.0 / frames.cycles;
	}
	size_t top[LIVE_FUNCTIONS];
	size_t count = mProfileTopFunctions(profile, top, LIVE_FUNCTIONS);
	for (size_t i = 0; i < count; ++i) {
		char name[512];
		mProfileDisplayName(profile, top[i], name, sizeof(name));
		summary.top.append({ QString::fromUtf8(name), mProfileFunctionListGetConstPointer(&profile->functions, top[i])->selfCycles });
	}
	mDebuggerProfilerUnlock(&m_profiler);
	interrupter.resume();
	emit liveUpdated(summary);
}
