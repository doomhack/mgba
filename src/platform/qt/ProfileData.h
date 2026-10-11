/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <memory>

#include <mgba/internal/debugger/profile.h>

struct VFile;

namespace QGBA {

// A finished profile, from a recording or a file. It never changes once loaded, so views can
// keep pointers into it while the emulator records the next one.
class ProfileData {
public:
	~ProfileData();

	static std::shared_ptr<ProfileData> load(VFile* vf, const QString& source);
	static std::shared_ptr<ProfileData> open(const QString& path);
	bool save(const QString& path) const;
	bool exportCallgrind(const QString& path) const;

	const mProfile* profile() const { return &m_profile; }
	QString source() const { return m_source; }

	uint32_t functionCount() const { return m_names.size(); }
	const mProfileFunction* function(uint32_t function) const;
	QString name(uint32_t function) const { return m_names[function]; }
	QString shortName(uint32_t function) const { return m_shortNames[function]; }
	QString rawName(uint32_t function) const;
	// The function with this symbol-table name, or mPROFILE_NONE
	uint32_t functionByRawName(const QString& name) const;
	bool isThumb(uint32_t function) const;

	uint32_t nodeCount() const { return m_children.size(); }
	const mProfileNode* node(uint32_t node) const;
	// A node's children, most expensive first
	const QVector<uint32_t>& children(uint32_t node) const { return m_children[node]; }
	// Main code first, then interrupts, hardware DMA and idle time
	const QVector<uint32_t>& roots() const { return m_roots; }
	// A function's nodes, most expensive first
	const QVector<uint32_t>& nodesOf(uint32_t function) const { return m_nodesOf[function]; }

	quint64 totalCycles() const { return m_profile.totalCycles; }
	double percent(quint64 cycles) const;
	mProfileFrameSummary frames() const;

private:
	ProfileData();

	void index();

	mProfile m_profile;
	QString m_source;
	QVector<QString> m_names;
	QVector<QString> m_shortNames;
	QHash<QString, uint32_t> m_byRawName;
	QVector<QVector<uint32_t>> m_children;
	QVector<uint32_t> m_roots;
	QVector<QVector<uint32_t>> m_nodesOf;
	QVector<bool> m_thumb;
};

// "void bn::sprite_ptr::set_position<int>(bn::fixed_point const&) [clone]" becomes
// "bn::sprite_ptr::set_position [clone]"
QString shortFunctionName(const QString& name);

}
