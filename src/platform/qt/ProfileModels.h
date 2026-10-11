/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QAbstractItemModel>
#include <QVector>

#include <functional>
#include <memory>

#include "ProfileData.h"

namespace QGBA {

namespace ProfileRole {
	enum {
		// The raw value a column shows, for sorting
		SORT = Qt::UserRole,
		FUNCTION,
		NODE,
		// Text to match when searching: the display and symbol-table names
		SEARCH,
	};
}

// Shared by the views of one profile: which profile, and how to show names
class ProfileModelBase {
public:
	void setShortNames(bool shortNames) { m_shortNames = shortNames; }

protected:
	QString nameOf(uint32_t function) const;
	QVariant nameData(uint32_t function, int role) const;

	std::shared_ptr<ProfileData> m_data;
	bool m_shortNames = true;
};

class ProfileTreeModel : public QAbstractItemModel, public ProfileModelBase {
Q_OBJECT

public:
	enum Column {
		NAME,
		TOTAL_PERCENT,
		SELF_PERCENT,
		TOTAL_CYCLES,
		SELF_CYCLES,
		CALLS,
		RECURSIVE,
		COLUMNS
	};

	ProfileTreeModel(QObject* parent = nullptr);

	void setProfile(std::shared_ptr<ProfileData>);
	void refreshNames(bool shortNames);
	QModelIndex indexOfNode(uint32_t node) const;

	QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
	QModelIndex parent(const QModelIndex& index) const override;
	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation, int role = Qt::DisplayRole) const override;

private:
	// Each node's position among its siblings
	QVector<int> m_rows;
};

class ProfileFunctionModel : public QAbstractTableModel, public ProfileModelBase {
Q_OBJECT

public:
	enum Column {
		NAME,
		SELF_PERCENT,
		SELF_CYCLES,
		INCLUSIVE_PERCENT,
		INCLUSIVE_CYCLES,
		CALLS,
		RECURSIVE,
		EXECUTIONS,
		COLUMNS
	};

	ProfileFunctionModel(QObject* parent = nullptr);

	void setProfile(std::shared_ptr<ProfileData>);
	void refreshNames(bool shortNames);
	int rowOf(uint32_t function) const;

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation, int role = Qt::DisplayRole) const override;

private:
	QVector<uint32_t> m_functions;
};

// A function's callers or callees, merged over all call sites
class ProfileEdgeModel : public QAbstractTableModel, public ProfileModelBase {
Q_OBJECT

public:
	enum Column {
		NAME,
		CALLS,
		RECURSIVE,
		INCLUSIVE_PERCENT,
		INCLUSIVE_CYCLES,
		COLUMNS
	};

	ProfileEdgeModel(bool callers, QObject* parent = nullptr);

	void setFunction(std::shared_ptr<ProfileData>, uint32_t function);
	void refreshNames(bool shortNames);

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation, int role = Qt::DisplayRole) const override;

private:
	bool m_callers;
	QVector<const mProfileEdge*> m_edges;
};

// A function's instructions, with what each cost
class ProfileInstructionModel : public QAbstractTableModel {
Q_OBJECT

public:
	// Reads code for the disassembly; returns false if it can't
	using CodeReader = std::function<bool(uint32_t address, unsigned width, uint32_t* value)>;

	enum Column {
		ADDRESS,
		CYCLES,
		PERCENT,
		EXECUTIONS,
		INSTRUCTION,
		COLUMNS
	};

	ProfileInstructionModel(QObject* parent = nullptr);

	void setFunction(std::shared_ptr<ProfileData>, uint32_t function, CodeReader);

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation, int role = Qt::DisplayRole) const override;

private:
	struct Row {
		uint32_t address;
		quint64 cycles;
		quint64 executions;
		QString instruction;
	};

	QString disassemble(uint32_t address, bool thumb, bool* combined) const;

	std::shared_ptr<ProfileData> m_data;
	uint32_t m_function = mPROFILE_NONE;
	quint64 m_functionCycles = 0;
	CodeReader m_reader;
	QVector<Row> m_rows;
};

}
