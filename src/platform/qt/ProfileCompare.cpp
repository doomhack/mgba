/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ProfileCompare.h"
#include "moc_ProfileCompare.cpp"

#include <QColor>
#include <QHash>
#include <QLocale>

#include <algorithm>
#include <cmath>

#include "ProfileModels.h"

using namespace QGBA;

static bool _active(const mProfileFunction* function) {
	return function->executions || function->selfCycles || function->calls;
}

ProfileComparison::ProfileComparison(std::shared_ptr<ProfileData> baseline, std::shared_ptr<ProfileData> current)
	: m_baseline(baseline)
	, m_current(current)
{
	uint32_t baselineFrames = baseline->profile()->frames;
	uint32_t currentFrames = current->profile()->frames;
	m_perFrame = baselineFrames && currentFrames;
	m_baselineScale = m_perFrame ? 1.0 / baselineFrames : 1.0;
	m_currentScale = m_perFrame ? 1.0 / currentFrames : 1.0;

	QHash<QString, int> byName;
	for (uint32_t i = 0; i < baseline->functionCount(); ++i) {
		if (_active(baseline->function(i))) {
			byName.insert(baseline->rawName(i), m_functions.size());
			m_functions.append({ i, mPROFILE_NONE });
		}
	}
	for (uint32_t i = 0; i < current->functionCount(); ++i) {
		if (!_active(current->function(i))) {
			continue;
		}
		auto match = byName.find(current->rawName(i));
		if (match != byName.end()) {
			m_functions[*match].current = i;
		} else {
			m_functions.append({ mPROFILE_NONE, i });
		}
	}

	// Match the trees level by level, by function name
	QVector<int> pending;
	auto matchChildren = [&](int parent, const QVector<uint32_t>& baselineNodes, const QVector<uint32_t>& currentNodes) {
		QHash<QString, int> children;
		for (uint32_t node : baselineNodes) {
			int index = addNode(parent, node, mPROFILE_NONE);
			children.insert(m_baseline->rawName(m_baseline->node(node)->function), index);
			pending.append(index);
		}
		for (uint32_t node : currentNodes) {
			auto match = children.find(m_current->rawName(m_current->node(node)->function));
			if (match != children.end()) {
				m_nodes[*match].current = node;
			} else {
				pending.append(addNode(parent, mPROFILE_NONE, node));
			}
		}
	};
	static const QVector<uint32_t> none;
	matchChildren(-1, baseline->roots(), current->roots());
	while (!pending.isEmpty()) {
		int index = pending.takeLast();
		uint32_t baselineNode = m_nodes[index].baseline;
		uint32_t currentNode = m_nodes[index].current;
		matchChildren(index, baselineNode != mPROFILE_NONE ? baseline->children(baselineNode) : none,
		              currentNode != mPROFILE_NONE ? current->children(currentNode) : none);
	}

	// The most expensive first, in either profile
	auto byCost = [this](int a, int b) {
		return nodeCost(m_nodes[a]) > nodeCost(m_nodes[b]);
	};
	for (Node& node : m_nodes) {
		std::stable_sort(node.children.begin(), node.children.end(), byCost);
		for (int i = 0; i < node.children.size(); ++i) {
			m_nodes[node.children[i]].row = i;
		}
	}
	// [main] stays first
	if (m_roots.size() > 1) {
		std::stable_sort(m_roots.begin() + 1, m_roots.end(), byCost);
	}
	for (int i = 0; i < m_roots.size(); ++i) {
		m_nodes[m_roots[i]].row = i;
	}
}

int ProfileComparison::addNode(int parent, uint32_t baseline, uint32_t current) {
	int index = m_nodes.size();
	m_nodes.append({ baseline, current, parent, 0, {} });
	if (parent >= 0) {
		m_nodes[parent].children.append(index);
	} else {
		m_roots.append(index);
	}
	return index;
}

quint64 ProfileComparison::nodeCost(const Node& node) const {
	double baseline = node.baseline != mPROFILE_NONE ? baselineCost(m_baseline->node(node.baseline)->subtreeCycles) : 0;
	double current = node.current != mPROFILE_NONE ? currentCost(m_current->node(node.current)->subtreeCycles) : 0;
	return static_cast<quint64>(std::max(baseline, current));
}

QString ProfileComparison::name(const Function& function, bool shortName) const {
	if (function.current != mPROFILE_NONE) {
		return shortName ? m_current->shortName(function.current) : m_current->name(function.current);
	}
	return shortName ? m_baseline->shortName(function.baseline) : m_baseline->name(function.baseline);
}

QString ProfileComparison::name(const Node& node, bool shortName) const {
	if (node.current != mPROFILE_NONE) {
		uint32_t function = m_current->node(node.current)->function;
		return shortName ? m_current->shortName(function) : m_current->name(function);
	}
	uint32_t function = m_baseline->node(node.baseline)->function;
	return shortName ? m_baseline->shortName(function) : m_baseline->name(function);
}

static QString formatCost(double cost) {
	// Small per-frame averages need decimals; whole numbers and big ones don't
	if (std::abs(cost) >= 100 || cost == std::floor(cost)) {
		return QLocale().toString(cost, 'f', 0);
	}
	return QLocale().toString(cost, 'f', 2);
}

QVariant ProfileCompareModelBase::costData(int column, double baseline, double current, int role) const {
	if (role == Qt::TextAlignmentRole) {
		return QVariant::fromValue<int>(Qt::AlignRight | Qt::AlignVCenter);
	}
	if (column != CompareColumn::CHANGE) {
		double cost = column == CompareColumn::BASELINE ? baseline : current;
		if (role == Qt::DisplayRole) {
			return formatCost(cost);
		}
		if (role == ProfileRole::SORT) {
			return cost;
		}
		return {};
	}
	switch (role) {
	case Qt::DisplayRole:
		if (!baseline && !current) {
			return QString();
		}
		if (!baseline) {
			return QObject::tr("new");
		}
		if (!current) {
			return QObject::tr("gone");
		}
		return QString("%1%2%").arg(current >= baseline ? "+" : "").arg((current - baseline) * 100.0 / baseline, 0, 'f', 1);
	case Qt::ToolTipRole:
		return QObject::tr("%1 cycles").arg(formatCost(current - baseline));
	case Qt::ForegroundRole:
		// Small changes are expected between runs, so only bigger ones stand out
		if (current > baseline * 1.02 + 1) {
			return QColor(220, 90, 80);
		}
		if (current < baseline * 0.98 - 1) {
			return QColor(80, 170, 90);
		}
		return {};
	case ProfileRole::SORT:
		return current - baseline;
	default:
		return {};
	}
}

QString ProfileCompareModelBase::costHeader(const QString& what, int column) const {
	if (column == CompareColumn::CHANGE) {
		return QObject::tr("Change");
	}
	QString which = column == CompareColumn::BASELINE ? QObject::tr("baseline") : QObject::tr("current");
	if (m_comparison && m_comparison->perFrame()) {
		return QObject::tr("%1/frame, %2").arg(what, which);
	}
	return QObject::tr("%1, %2").arg(what, which);
}

ProfileCompareFunctionModel::ProfileCompareFunctionModel(QObject* parent)
	: QAbstractTableModel(parent)
{
}

void ProfileCompareFunctionModel::setComparison(std::shared_ptr<ProfileComparison> comparison) {
	beginResetModel();
	m_comparison = comparison;
	endResetModel();
	emit headerDataChanged(Qt::Horizontal, 0, COLUMNS - 1);
}

void ProfileCompareFunctionModel::refreshNames(bool shortNames) {
	setShortNames(shortNames);
	if (rowCount()) {
		emit dataChanged(index(0, NAME), index(rowCount() - 1, NAME));
	}
}

int ProfileCompareFunctionModel::rowCount(const QModelIndex& parent) const {
	if (parent.isValid() || !m_comparison) {
		return 0;
	}
	return m_comparison->functions().size();
}

int ProfileCompareFunctionModel::columnCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return COLUMNS;
}

QVariant ProfileCompareFunctionModel::data(const QModelIndex& index, int role) const {
	if (!m_comparison || !index.isValid() || index.row() >= rowCount()) {
		return {};
	}
	const ProfileComparison::Function& function = m_comparison->functions()[index.row()];
	if (index.column() == NAME) {
		switch (role) {
		case Qt::DisplayRole:
			return m_comparison->name(function, m_shortNames);
		case Qt::ToolTipRole:
			return m_comparison->name(function, false);
		case ProfileRole::SORT:
			return m_comparison->name(function, m_shortNames).toLower();
		case ProfileRole::SEARCH:
			return m_comparison->name(function, false);
		default:
			return {};
		}
	}
	const mProfileFunction* baseline = function.baseline != mPROFILE_NONE ? m_comparison->baseline()->function(function.baseline) : nullptr;
	const mProfileFunction* current = function.current != mPROFILE_NONE ? m_comparison->current()->function(function.current) : nullptr;
	int group = (index.column() - 1) / 3;
	int column = (index.column() - 1) % 3 + 1;
	quint64 baselineValue = 0;
	quint64 currentValue = 0;
	switch (group) {
	case 0:
		baselineValue = baseline ? baseline->selfCycles : 0;
		currentValue = current ? current->selfCycles : 0;
		break;
	case 1:
		baselineValue = baseline ? baseline->inclusiveCycles : 0;
		currentValue = current ? current->inclusiveCycles : 0;
		break;
	default:
		baselineValue = baseline ? baseline->calls : 0;
		currentValue = current ? current->calls : 0;
		break;
	}
	return costData(column, m_comparison->baselineCost(baselineValue), m_comparison->currentCost(currentValue), role);
}

QVariant ProfileCompareFunctionModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
		return {};
	}
	switch (section) {
	case NAME:
		return tr("Function");
	case SELF_BASELINE:
	case SELF_CURRENT:
	case SELF_CHANGE:
		return costHeader(tr("Self"), (section - 1) % 3 + 1);
	case INCLUSIVE_BASELINE:
	case INCLUSIVE_CURRENT:
	case INCLUSIVE_CHANGE:
		return costHeader(tr("Inclusive"), (section - 1) % 3 + 1);
	case CALLS_BASELINE:
	case CALLS_CURRENT:
		return costHeader(tr("Calls"), (section - 1) % 3 + 1);
	default:
		return {};
	}
}

ProfileCompareTreeModel::ProfileCompareTreeModel(QObject* parent)
	: QAbstractItemModel(parent)
{
}

void ProfileCompareTreeModel::setComparison(std::shared_ptr<ProfileComparison> comparison) {
	beginResetModel();
	m_comparison = comparison;
	endResetModel();
	emit headerDataChanged(Qt::Horizontal, 0, COLUMNS - 1);
}

void ProfileCompareTreeModel::refreshNames(bool shortNames) {
	setShortNames(shortNames);
	emit layoutAboutToBeChanged();
	emit layoutChanged();
}

QModelIndex ProfileCompareTreeModel::index(int row, int column, const QModelIndex& parent) const {
	if (!m_comparison || row < 0 || column < 0 || column >= COLUMNS) {
		return {};
	}
	const QVector<int>& siblings = parent.isValid() ? m_comparison->nodes()[parent.internalId()].children : m_comparison->roots();
	if (row >= siblings.size()) {
		return {};
	}
	return createIndex(row, column, static_cast<quintptr>(siblings[row]));
}

QModelIndex ProfileCompareTreeModel::parent(const QModelIndex& index) const {
	if (!m_comparison || !index.isValid()) {
		return {};
	}
	int parent = m_comparison->nodes()[index.internalId()].parent;
	if (parent < 0) {
		return {};
	}
	return createIndex(m_comparison->nodes()[parent].row, 0, static_cast<quintptr>(parent));
}

int ProfileCompareTreeModel::rowCount(const QModelIndex& parent) const {
	if (!m_comparison) {
		return 0;
	}
	if (!parent.isValid()) {
		return m_comparison->roots().size();
	}
	if (parent.column() > 0) {
		return 0;
	}
	return m_comparison->nodes()[parent.internalId()].children.size();
}

int ProfileCompareTreeModel::columnCount(const QModelIndex&) const {
	return COLUMNS;
}

QVariant ProfileCompareTreeModel::data(const QModelIndex& index, int role) const {
	if (!m_comparison || !index.isValid()) {
		return {};
	}
	const ProfileComparison::Node& node = m_comparison->nodes()[index.internalId()];
	if (index.column() == NAME) {
		if (role == Qt::DisplayRole) {
			return m_comparison->name(node, m_shortNames);
		}
		if (role == Qt::ToolTipRole) {
			return m_comparison->name(node, false);
		}
		return {};
	}
	const mProfileNode* baseline = node.baseline != mPROFILE_NONE ? m_comparison->baseline()->node(node.baseline) : nullptr;
	const mProfileNode* current = node.current != mPROFILE_NONE ? m_comparison->current()->node(node.current) : nullptr;
	bool calls = index.column() >= CALLS_BASELINE;
	int column = calls ? index.column() - CALLS_BASELINE + 1 : index.column();
	quint64 baselineValue = baseline ? (calls ? baseline->calls : baseline->subtreeCycles) : 0;
	quint64 currentValue = current ? (calls ? current->calls : current->subtreeCycles) : 0;
	return costData(column, m_comparison->baselineCost(baselineValue), m_comparison->currentCost(currentValue), role);
}

QVariant ProfileCompareTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
		return {};
	}
	switch (section) {
	case NAME:
		return tr("Function");
	case TOTAL_BASELINE:
	case TOTAL_CURRENT:
	case TOTAL_CHANGE:
		return costHeader(tr("Total"), section);
	case CALLS_BASELINE:
	case CALLS_CURRENT:
		return costHeader(tr("Calls"), section - CALLS_BASELINE + 1);
	default:
		return {};
	}
}
