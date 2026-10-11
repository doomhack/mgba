/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ProfileModels.h"
#include "moc_ProfileModels.cpp"

#include <QFontDatabase>
#include <QLocale>

#include <mgba/internal/arm/decoder.h>

using namespace QGBA;

static QString formatCycles(quint64 cycles) {
	return QLocale().toString(cycles);
}

static QVariant number(quint64 value) {
	return QVariant::fromValue<qulonglong>(value);
}

static QString formatPercent(double percent) {
	return QString("%1%").arg(percent, 0, 'f', 2);
}

static QVariant alignment(int column) {
	if (column == 0) {
		return QVariant::fromValue<int>(Qt::AlignLeft | Qt::AlignVCenter);
	}
	return QVariant::fromValue<int>(Qt::AlignRight | Qt::AlignVCenter);
}

QString ProfileModelBase::nameOf(uint32_t function) const {
	return m_shortNames ? m_data->shortName(function) : m_data->name(function);
}

QVariant ProfileModelBase::nameData(uint32_t function, int role) const {
	switch (role) {
	case Qt::DisplayRole:
		return nameOf(function);
	case Qt::ToolTipRole: {
		QString name = m_data->name(function);
		QString raw = m_data->rawName(function);
		return raw == name || m_data->function(function)->pseudo ? name : QString("%1\n%2").arg(name, raw);
	}
	case ProfileRole::SORT:
		return nameOf(function).toLower();
	case ProfileRole::SEARCH:
		return QString("%1 %2").arg(m_data->name(function), m_data->rawName(function));
	case ProfileRole::FUNCTION:
		return function;
	default:
		return {};
	}
}

ProfileTreeModel::ProfileTreeModel(QObject* parent)
	: QAbstractItemModel(parent)
{
}

void ProfileTreeModel::setProfile(std::shared_ptr<ProfileData> data) {
	beginResetModel();
	m_data = data;
	m_rows.clear();
	if (m_data) {
		m_rows.resize(m_data->nodeCount());
		const QVector<uint32_t>& roots = m_data->roots();
		for (int i = 0; i < roots.size(); ++i) {
			m_rows[roots[i]] = i;
		}
		for (uint32_t node = 0; node < m_data->nodeCount(); ++node) {
			const QVector<uint32_t>& children = m_data->children(node);
			for (int i = 0; i < children.size(); ++i) {
				m_rows[children[i]] = i;
			}
		}
	}
	endResetModel();
}

void ProfileTreeModel::refreshNames(bool shortNames) {
	setShortNames(shortNames);
	emit layoutAboutToBeChanged();
	emit layoutChanged();
}

QModelIndex ProfileTreeModel::indexOfNode(uint32_t node) const {
	if (!m_data || node >= m_data->nodeCount()) {
		return {};
	}
	const mProfileNode* entry = m_data->node(node);
	if (entry->parent == mPROFILE_NONE && !m_data->roots().contains(node)) {
		return {};
	}
	return createIndex(m_rows[node], 0, static_cast<quintptr>(node));
}

QModelIndex ProfileTreeModel::index(int row, int column, const QModelIndex& parent) const {
	if (!m_data || row < 0 || column < 0 || column >= COLUMNS) {
		return {};
	}
	const QVector<uint32_t>& siblings =
	    parent.isValid() ? m_data->children(static_cast<uint32_t>(parent.internalId())) : m_data->roots();
	if (row >= siblings.size()) {
		return {};
	}
	return createIndex(row, column, static_cast<quintptr>(siblings[row]));
}

QModelIndex ProfileTreeModel::parent(const QModelIndex& index) const {
	if (!m_data || !index.isValid()) {
		return {};
	}
	uint32_t parent = m_data->node(static_cast<uint32_t>(index.internalId()))->parent;
	if (parent == mPROFILE_NONE) {
		return {};
	}
	return createIndex(m_rows[parent], 0, static_cast<quintptr>(parent));
}

int ProfileTreeModel::rowCount(const QModelIndex& parent) const {
	if (!m_data) {
		return 0;
	}
	if (!parent.isValid()) {
		return m_data->roots().size();
	}
	if (parent.column() > 0) {
		return 0;
	}
	return m_data->children(static_cast<uint32_t>(parent.internalId())).size();
}

int ProfileTreeModel::columnCount(const QModelIndex&) const {
	return COLUMNS;
}

QVariant ProfileTreeModel::data(const QModelIndex& index, int role) const {
	if (!m_data || !index.isValid()) {
		return {};
	}
	uint32_t nodeIndex = static_cast<uint32_t>(index.internalId());
	const mProfileNode* node = m_data->node(nodeIndex);
	if (role == ProfileRole::NODE) {
		return nodeIndex;
	}
	if (role == Qt::TextAlignmentRole) {
		return alignment(index.column());
	}
	if (index.column() == NAME || role == ProfileRole::FUNCTION) {
		return nameData(node->function, role);
	}
	if (role != Qt::DisplayRole && role != ProfileRole::SORT) {
		return {};
	}
	bool display = role == Qt::DisplayRole;
	switch (index.column()) {
	case TOTAL_PERCENT:
		return display ? QVariant(formatPercent(m_data->percent(node->subtreeCycles))) : number(node->subtreeCycles);
	case SELF_PERCENT:
		return display ? QVariant(formatPercent(m_data->percent(node->selfCycles))) : number(node->selfCycles);
	case TOTAL_CYCLES:
		return display ? QVariant(formatCycles(node->subtreeCycles)) : number(node->subtreeCycles);
	case SELF_CYCLES:
		return display ? QVariant(formatCycles(node->selfCycles)) : number(node->selfCycles);
	case CALLS:
		return display ? QVariant(formatCycles(node->calls)) : number(node->calls);
	case RECURSIVE:
		if (display && !node->recursiveCalls) {
			return QString();
		}
		return display ? QVariant(formatCycles(node->recursiveCalls)) : number(node->recursiveCalls);
	default:
		return {};
	}
}

QVariant ProfileTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal) {
		return {};
	}
	if (role == Qt::TextAlignmentRole) {
		return alignment(section);
	}
	if (role != Qt::DisplayRole) {
		return {};
	}
	switch (section) {
	case NAME:
		return tr("Function");
	case TOTAL_PERCENT:
		return tr("Total %");
	case SELF_PERCENT:
		return tr("Self %");
	case TOTAL_CYCLES:
		return tr("Total cycles");
	case SELF_CYCLES:
		return tr("Self cycles");
	case CALLS:
		return tr("Calls");
	case RECURSIVE:
		return tr("Recursive");
	default:
		return {};
	}
}

ProfileFunctionModel::ProfileFunctionModel(QObject* parent)
	: QAbstractTableModel(parent)
{
}

void ProfileFunctionModel::setProfile(std::shared_ptr<ProfileData> data) {
	beginResetModel();
	m_data = data;
	m_functions.clear();
	if (m_data) {
		for (uint32_t i = 0; i < m_data->functionCount(); ++i) {
			const mProfileFunction* function = m_data->function(i);
			if (function->executions || function->selfCycles || function->calls) {
				m_functions.append(i);
			}
		}
	}
	endResetModel();
}

void ProfileFunctionModel::refreshNames(bool shortNames) {
	setShortNames(shortNames);
	if (!m_functions.isEmpty()) {
		emit dataChanged(index(0, NAME), index(m_functions.size() - 1, NAME));
	}
}

int ProfileFunctionModel::rowOf(uint32_t function) const {
	return m_functions.indexOf(function);
}

int ProfileFunctionModel::rowCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return m_functions.size();
}

int ProfileFunctionModel::columnCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return COLUMNS;
}

QVariant ProfileFunctionModel::data(const QModelIndex& index, int role) const {
	if (!m_data || !index.isValid() || index.row() >= m_functions.size()) {
		return {};
	}
	uint32_t functionIndex = m_functions[index.row()];
	const mProfileFunction* function = m_data->function(functionIndex);
	if (role == Qt::TextAlignmentRole) {
		return alignment(index.column());
	}
	if (index.column() == NAME || role == ProfileRole::FUNCTION) {
		return nameData(functionIndex, role);
	}
	if (role != Qt::DisplayRole && role != ProfileRole::SORT) {
		return {};
	}
	bool display = role == Qt::DisplayRole;
	switch (index.column()) {
	case SELF_PERCENT:
		return display ? QVariant(formatPercent(m_data->percent(function->selfCycles))) : number(function->selfCycles);
	case SELF_CYCLES:
		return display ? QVariant(formatCycles(function->selfCycles)) : number(function->selfCycles);
	case INCLUSIVE_PERCENT:
		return display ? QVariant(formatPercent(m_data->percent(function->inclusiveCycles))) :
		                 number(function->inclusiveCycles);
	case INCLUSIVE_CYCLES:
		return display ? QVariant(formatCycles(function->inclusiveCycles)) : number(function->inclusiveCycles);
	case CALLS:
		return display ? QVariant(formatCycles(function->calls)) : number(function->calls);
	case RECURSIVE:
		if (display && !function->recursiveCalls) {
			return QString();
		}
		return display ? QVariant(formatCycles(function->recursiveCalls)) : number(function->recursiveCalls);
	case EXECUTIONS:
		return display ? QVariant(formatCycles(function->executions)) : number(function->executions);
	default:
		return {};
	}
}

QVariant ProfileFunctionModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal) {
		return {};
	}
	if (role == Qt::TextAlignmentRole) {
		return alignment(section);
	}
	if (role == Qt::ToolTipRole && section == EXECUTIONS) {
		return tr("Instructions run");
	}
	if (role != Qt::DisplayRole) {
		return {};
	}
	switch (section) {
	case NAME:
		return tr("Function");
	case SELF_PERCENT:
		return tr("Self %");
	case SELF_CYCLES:
		return tr("Self cycles");
	case INCLUSIVE_PERCENT:
		return tr("Inclusive %");
	case INCLUSIVE_CYCLES:
		return tr("Inclusive cycles");
	case CALLS:
		return tr("Calls");
	case RECURSIVE:
		return tr("Recursive");
	case EXECUTIONS:
		return tr("Executions");
	default:
		return {};
	}
}

ProfileEdgeModel::ProfileEdgeModel(bool callers, QObject* parent)
	: QAbstractTableModel(parent)
	, m_callers(callers)
{
}

void ProfileEdgeModel::setFunction(std::shared_ptr<ProfileData> data, uint32_t function) {
	beginResetModel();
	m_data = data;
	m_edges.clear();
	if (m_data && function != mPROFILE_NONE) {
		const mProfile* profile = m_data->profile();
		for (size_t i = 0; i < mProfileEdgeListSize(&profile->edges); ++i) {
			const mProfileEdge* edge = mProfileEdgeListGetConstPointer(&profile->edges, i);
			if ((m_callers ? edge->callee : edge->caller) == function) {
				m_edges.append(edge);
			}
		}
		std::sort(m_edges.begin(), m_edges.end(), [](const mProfileEdge* a, const mProfileEdge* b) {
			return a->inclusiveCycles > b->inclusiveCycles;
		});
	}
	endResetModel();
}

void ProfileEdgeModel::refreshNames(bool shortNames) {
	setShortNames(shortNames);
	if (!m_edges.isEmpty()) {
		emit dataChanged(index(0, NAME), index(m_edges.size() - 1, NAME));
	}
}

int ProfileEdgeModel::rowCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return m_edges.size();
}

int ProfileEdgeModel::columnCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return COLUMNS;
}

QVariant ProfileEdgeModel::data(const QModelIndex& index, int role) const {
	if (!m_data || !index.isValid() || index.row() >= m_edges.size()) {
		return {};
	}
	const mProfileEdge* edge = m_edges[index.row()];
	uint32_t other = m_callers ? edge->caller : edge->callee;
	if (role == Qt::TextAlignmentRole) {
		return alignment(index.column());
	}
	if (index.column() == NAME || role == ProfileRole::FUNCTION) {
		return nameData(other, role);
	}
	if (role != Qt::DisplayRole && role != ProfileRole::SORT) {
		return {};
	}
	bool display = role == Qt::DisplayRole;
	switch (index.column()) {
	case CALLS:
		return display ? QVariant(formatCycles(edge->calls)) : number(edge->calls);
	case RECURSIVE:
		if (display && !edge->recursiveCalls) {
			return QString();
		}
		return display ? QVariant(formatCycles(edge->recursiveCalls)) : number(edge->recursiveCalls);
	case INCLUSIVE_PERCENT:
		return display ? QVariant(formatPercent(m_data->percent(edge->inclusiveCycles))) : number(edge->inclusiveCycles);
	case INCLUSIVE_CYCLES:
		return display ? QVariant(formatCycles(edge->inclusiveCycles)) : number(edge->inclusiveCycles);
	default:
		return {};
	}
}

QVariant ProfileEdgeModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal) {
		return {};
	}
	if (role == Qt::TextAlignmentRole) {
		return alignment(section);
	}
	if (role == Qt::ToolTipRole && section >= INCLUSIVE_PERCENT) {
		return m_callers ? tr("Time in this function when called from there") : tr("Time in that function when called from here");
	}
	if (role != Qt::DisplayRole) {
		return {};
	}
	switch (section) {
	case NAME:
		return m_callers ? tr("Caller") : tr("Callee");
	case CALLS:
		return tr("Calls");
	case RECURSIVE:
		return tr("Recursive");
	case INCLUSIVE_PERCENT:
		return tr("Inclusive %");
	case INCLUSIVE_CYCLES:
		return tr("Inclusive cycles");
	default:
		return {};
	}
}

ProfileInstructionModel::ProfileInstructionModel(QObject* parent)
	: QAbstractTableModel(parent)
{
}

void ProfileInstructionModel::setFunction(std::shared_ptr<ProfileData> data, uint32_t function, CodeReader reader) {
	beginResetModel();
	m_data = data;
	m_function = function;
	m_reader = reader;
	m_rows.clear();
	m_functionCycles = 0;
	if (!m_data || function == mPROFILE_NONE) {
		endResetModel();
		return;
	}
	const mProfile* profile = m_data->profile();
	const mProfileFunction* entry = m_data->function(function);
	m_functionCycles = entry->selfCycles;
	bool thumb = m_data->isThumb(function);
	unsigned width = thumb ? 2 : 4;

	if (!entry->pseudo && entry->size && entry->size <= 0x10000) {
		// Every instruction of the function, whether it ran or not
		for (uint32_t address = entry->start & ~(width - 1); address - entry->start < entry->size; address += width) {
			const mProfileCounter* counter = mProfileGetCounter(profile, address);
			m_rows.append({ address, counter ? counter->cycles : 0, counter ? counter->executions : 0, {} });
		}
	} else if (entry->size) {
		// Only the instructions that ran, since the range is large or has no symbol
		uint64_t end = (uint64_t) entry->start + entry->size;
		uint64_t address = entry->start;
		while (address < end) {
			uint32_t region = address >> 24;
			if (region >= mPROFILE_REGIONS) {
				break;
			}
			mProfileCounter** directory = profile->pages[region];
			if (!directory) {
				address = (uint64_t) (region + 1) << 24;
				continue;
			}
			const mProfileCounter* page = directory[(address >> mPROFILE_PAGE_BITS) & (mPROFILE_PAGES - 1)];
			uint64_t pageEnd = (address | (mPROFILE_PAGE_SIZE - 1)) + 1;
			if (!page) {
				address = pageEnd;
				continue;
			}
			for (; address < pageEnd && address < end; address += 2) {
				const mProfileCounter* counter = &page[(address & (mPROFILE_PAGE_SIZE - 1)) >> 1];
				if (counter->executions && mProfileFunctionAt(profile, address) == function) {
					m_rows.append({ static_cast<uint32_t>(address), counter->cycles, counter->executions, {} });
				}
			}
		}
	}

	for (int i = 0; i < m_rows.size(); ++i) {
		bool combined = false;
		m_rows[i].instruction = disassemble(m_rows[i].address, thumb, &combined);
		if (combined && i + 1 < m_rows.size() && m_rows[i + 1].address == m_rows[i].address + 2) {
			// Thumb's BL is two halves; both are counted, but they're one instruction
			++i;
			m_rows[i].instruction = tr("(second half)");
		}
	}
	endResetModel();
}

QString ProfileInstructionModel::disassemble(uint32_t address, bool thumb, bool* combined) const {
	*combined = false;
	if (!m_reader) {
		return {};
	}
	ARMInstructionInfo info;
	uint32_t pc;
	if (thumb) {
		uint32_t opcode;
		if (!m_reader(address, 2, &opcode)) {
			return {};
		}
		ARMDecodeThumb(opcode, &info);
		uint32_t next;
		if (m_reader(address + 2, 2, &next)) {
			ARMInstructionInfo info2;
			ARMInstructionInfo joined;
			ARMDecodeThumb(next, &info2);
			if (ARMDecodeThumbCombine(&info, &info2, &joined)) {
				info = joined;
				*combined = true;
			}
		}
		pc = address + 4;
	} else {
		uint32_t opcode;
		if (!m_reader(address, 4, &opcode)) {
			return {};
		}
		ARMDecodeARM(opcode, &info);
		pc = address + 8;
	}
	char buffer[128];
	ARMDisassemble(&info, nullptr, nullptr, pc, buffer, sizeof(buffer));
	QString text = QString::fromUtf8(buffer);
	if ((info.mnemonic == ARM_MN_B || info.mnemonic == ARM_MN_BL) && (info.operandFormat & ARM_OPERAND_IMMEDIATE_1)) {
		uint32_t target = pc + info.op1.immediate;
		uint32_t function = mProfileFunctionAt(m_data->profile(), target);
		if (function != m_function) {
			text += QString("  ; %1").arg(m_data->shortName(function));
		}
	}
	return text;
}

int ProfileInstructionModel::rowCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return m_rows.size();
}

int ProfileInstructionModel::columnCount(const QModelIndex& parent) const {
	if (parent.isValid()) {
		return 0;
	}
	return COLUMNS;
}

QVariant ProfileInstructionModel::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || index.row() >= m_rows.size()) {
		return {};
	}
	const Row& row = m_rows[index.row()];
	if (role == Qt::TextAlignmentRole) {
		return index.column() == INSTRUCTION ? alignment(0) : alignment(1);
	}
	if (role == Qt::FontRole && (index.column() == ADDRESS || index.column() == INSTRUCTION)) {
		return QFontDatabase::systemFont(QFontDatabase::FixedFont);
	}
	if (role != Qt::DisplayRole) {
		return {};
	}
	switch (index.column()) {
	case ADDRESS:
		return QString("%1").arg(row.address, 8, 16, QChar('0')).toUpper();
	case CYCLES:
		return row.executions ? formatCycles(row.cycles) : QString();
	case PERCENT:
		if (!row.executions || !m_functionCycles) {
			return QString();
		}
		return formatPercent(row.cycles * 100.0 / m_functionCycles);
	case EXECUTIONS:
		return row.executions ? formatCycles(row.executions) : QString();
	case INSTRUCTION:
		return row.instruction;
	default:
		return {};
	}
}

QVariant ProfileInstructionModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if (orientation != Qt::Horizontal) {
		return {};
	}
	if (role == Qt::TextAlignmentRole) {
		return section == INSTRUCTION ? alignment(0) : alignment(1);
	}
	if (role != Qt::DisplayRole) {
		return {};
	}
	switch (section) {
	case ADDRESS:
		return tr("Address");
	case CYCLES:
		return tr("Cycles");
	case PERCENT:
		return tr("% of function");
	case EXECUTIONS:
		return tr("Executions");
	case INSTRUCTION:
		return tr("Instruction");
	default:
		return {};
	}
}
