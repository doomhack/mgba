/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ProfileData.h"

#include <algorithm>

#include "VFileDevice.h"

#include <mgba-util/vfs.h>

using namespace QGBA;

ProfileData::ProfileData() {
	mProfileInit(&m_profile);
}

ProfileData::~ProfileData() {
	mProfileDeinit(&m_profile);
}

std::shared_ptr<ProfileData> ProfileData::load(VFile* vf, const QString& source) {
	std::shared_ptr<ProfileData> data(new ProfileData);
	if (!mProfileLoad(&data->m_profile, vf)) {
		return nullptr;
	}
	data->m_source = source;
	data->index();
	return data;
}

std::shared_ptr<ProfileData> ProfileData::open(const QString& path) {
	VFile* vf = VFileDevice::open(path, O_RDONLY);
	if (!vf) {
		return nullptr;
	}
	std::shared_ptr<ProfileData> data = load(vf, path);
	vf->close(vf);
	return data;
}

bool ProfileData::save(const QString& path) const {
	VFile* vf = VFileDevice::open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		return false;
	}
	bool success = mProfileSave(&m_profile, vf);
	vf->close(vf);
	return success;
}

bool ProfileData::exportCallgrind(const QString& path) const {
	VFile* vf = VFileDevice::open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		return false;
	}
	bool success = mProfileExportCallgrind(&m_profile, vf);
	vf->close(vf);
	return success;
}

const mProfileFunction* ProfileData::function(uint32_t function) const {
	return mProfileFunctionListGetConstPointer(&m_profile.functions, function);
}

QString ProfileData::rawName(uint32_t function) const {
	return QString::fromUtf8(this->function(function)->name);
}

uint32_t ProfileData::functionByRawName(const QString& name) const {
	return m_byRawName.value(name, mPROFILE_NONE);
}

bool ProfileData::isThumb(uint32_t function) const {
	return m_thumb[function];
}

const mProfileNode* ProfileData::node(uint32_t node) const {
	return mProfileNodeListGetConstPointer(&m_profile.nodes, node);
}

double ProfileData::percent(quint64 cycles) const {
	if (!m_profile.totalCycles) {
		return 0;
	}
	return cycles * 100.0 / m_profile.totalCycles;
}

mProfileFrameSummary ProfileData::frames() const {
	mProfileFrameSummary summary;
	mProfileSummarizeFrames(&m_profile, &summary);
	return summary;
}

void ProfileData::index() {
	uint32_t functions = mProfileFunctionListSize(&m_profile.functions);
	m_names.resize(functions);
	m_shortNames.resize(functions);
	m_nodesOf.resize(functions);
	m_thumb.fill(false, functions);
	for (uint32_t i = 0; i < functions; ++i) {
		char name[1024];
		mProfileDisplayName(&m_profile, i, name, sizeof(name));
		m_names[i] = QString::fromUtf8(name);
		m_shortNames[i] = shortFunctionName(m_names[i]);
		m_byRawName.insert(rawName(i), i);
		m_thumb[i] = function(i)->thumb;
	}

	// Code without a symbol has no mode recorded. Thumb code runs at addresses ARM code can't.
	for (unsigned region = 0; region < mPROFILE_REGIONS; ++region) {
		mProfileCounter** directory = m_profile.pages[region];
		if (!directory) {
			continue;
		}
		for (size_t page = 0; page < mPROFILE_PAGES; ++page) {
			if (!directory[page]) {
				continue;
			}
			for (size_t slot = 1; slot < mPROFILE_SLOTS; slot += 2) {
				if (directory[page][slot].executions) {
					uint32_t address = (region << 24) | (uint32_t) (page << mPROFILE_PAGE_BITS) | (uint32_t) (slot << 1);
					m_thumb[mProfileFunctionAt(&m_profile, address)] = true;
				}
			}
		}
	}

	uint32_t nodes = mProfileNodeListSize(&m_profile.nodes);
	m_children.resize(nodes);
	for (uint32_t i = 0; i < nodes; ++i) {
		const mProfileNode* entry = node(i);
		if (entry->parent == mPROFILE_NONE) {
			if (i == mPROFILE_ROOT_MAIN || entry->subtreeCycles) {
				m_roots.append(i);
			}
		} else {
			m_children[entry->parent].append(i);
		}
		if (entry->function < functions) {
			m_nodesOf[entry->function].append(i);
		}
	}
	auto bySubtree = [this](uint32_t a, uint32_t b) {
		return node(a)->subtreeCycles > node(b)->subtreeCycles;
	};
	for (auto& children : m_children) {
		std::stable_sort(children.begin(), children.end(), bySubtree);
	}
	for (auto& list : m_nodesOf) {
		std::stable_sort(list.begin(), list.end(), bySubtree);
	}
	// [main] stays first; the other roots by cost
	std::stable_sort(m_roots.begin() + 1, m_roots.end(), bySubtree);
}

QString QGBA::shortFunctionName(const QString& name) {
	if (name.startsWith('[')) {
		// Pseudo-functions such as [DMA 3]
		return name;
	}
	QString text = name;
	QString suffix;
	if (text.endsWith(']')) {
		int open = text.lastIndexOf(QLatin1String(" ["));
		if (open > 0) {
			suffix = text.mid(open);
			text.truncate(open);
		}
	}

	// Template arguments. The angle brackets of operator<, operator<< and operator-> aren't.
	QString stripped;
	int depth = 0;
	for (QChar c : text) {
		bool isOperator = depth == 0 &&
		                  (stripped.endsWith(QLatin1String("operator")) || stripped.endsWith(QLatin1String("operator<")) ||
		                   stripped.endsWith(QLatin1String("operator>")) || stripped.endsWith(QLatin1String("operator-")));
		if ((c == '<' || c == '>') && isOperator) {
			stripped += c;
		} else if (c == '<') {
			++depth;
		} else if (c == '>') {
			if (depth) {
				--depth;
			}
		} else if (!depth) {
			stripped += c;
		}
	}

	// The parameter list: the parentheses that close last, before any qualifiers such as const
	int close = stripped.lastIndexOf(')');
	if (close >= 0) {
		int level = 0;
		int i;
		for (i = close; i >= 0; --i) {
			if (stripped[i] == ')') {
				++level;
			} else if (stripped[i] == '(' && !--level) {
				break;
			}
		}
		if (i > 0) {
			stripped.truncate(i);
		}
	}

	// The return type, before the last space outside parentheses, unless that space belongs to an
	// operator such as "operator new"
	int cut = -1;
	depth = 0;
	for (int i = stripped.size() - 1; i >= 0; --i) {
		QChar c = stripped[i];
		if (c == ')') {
			++depth;
		} else if (c == '(') {
			--depth;
		} else if (c == ' ' && !depth && !stripped.left(i).endsWith(QLatin1String("operator"))) {
			cut = i;
			break;
		}
	}
	if (cut >= 0) {
		stripped = stripped.mid(cut + 1);
	}
	return stripped + suffix;
}
