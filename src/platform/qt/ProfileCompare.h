/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QAbstractItemModel>
#include <QVector>

#include <memory>

#include "ProfileData.h"

namespace QGBA {

// Two profiles matched up: functions by their symbol-table names, and call tree nodes by the
// names along their call paths. Costs are compared per frame, so recordings of different lengths
// can be compared.
class ProfileComparison {
public:
	struct Function {
		uint32_t baseline;
		uint32_t current;
	};

	struct Node {
		uint32_t baseline;
		uint32_t current;
		int parent;
		int row;
		QVector<int> children;
	};

	ProfileComparison(std::shared_ptr<ProfileData> baseline, std::shared_ptr<ProfileData> current);

	std::shared_ptr<ProfileData> baseline() const { return m_baseline; }
	std::shared_ptr<ProfileData> current() const { return m_current; }
	// Whether costs are per frame, or for the whole recording because one has no frames
	bool perFrame() const { return m_perFrame; }
	double baselineCost(quint64 cycles) const { return cycles * m_baselineScale; }
	double currentCost(quint64 cycles) const { return cycles * m_currentScale; }

	QString name(const Function&, bool shortName) const;
	QString name(const Node&, bool shortName) const;

	const QVector<Function>& functions() const { return m_functions; }
	const QVector<Node>& nodes() const { return m_nodes; }
	const QVector<int>& roots() const { return m_roots; }

private:
	int addNode(int parent, uint32_t baseline, uint32_t current);
	QString rawName(uint32_t baseline, uint32_t current) const;
	quint64 nodeCost(const Node&) const;

	std::shared_ptr<ProfileData> m_baseline;
	std::shared_ptr<ProfileData> m_current;
	bool m_perFrame;
	double m_baselineScale;
	double m_currentScale;
	QVector<Function> m_functions;
	QVector<Node> m_nodes;
	QVector<int> m_roots;
};

namespace CompareColumn {
	enum {
		NAME,
		BASELINE,
		CURRENT,
		CHANGE,
	};
}

// Shared formatting for the comparison views
class ProfileCompareModelBase {
public:
	void setShortNames(bool shortNames) { m_shortNames = shortNames; }

protected:
	QVariant costData(int column, double baseline, double current, int role) const;
	QString costHeader(const QString& what, int column) const;

	std::shared_ptr<ProfileComparison> m_comparison;
	bool m_shortNames = true;
};

class ProfileCompareFunctionModel : public QAbstractTableModel, public ProfileCompareModelBase {
Q_OBJECT

public:
	enum Column {
		NAME = CompareColumn::NAME,
		SELF_BASELINE,
		SELF_CURRENT,
		SELF_CHANGE,
		INCLUSIVE_BASELINE,
		INCLUSIVE_CURRENT,
		INCLUSIVE_CHANGE,
		CALLS_BASELINE,
		CALLS_CURRENT,
		COLUMNS
	};

	ProfileCompareFunctionModel(QObject* parent = nullptr);

	void setComparison(std::shared_ptr<ProfileComparison>);
	void refreshNames(bool shortNames);

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation, int role = Qt::DisplayRole) const override;
};

class ProfileCompareTreeModel : public QAbstractItemModel, public ProfileCompareModelBase {
Q_OBJECT

public:
	enum Column {
		NAME = CompareColumn::NAME,
		TOTAL_BASELINE,
		TOTAL_CURRENT,
		TOTAL_CHANGE,
		CALLS_BASELINE,
		CALLS_CURRENT,
		COLUMNS
	};

	ProfileCompareTreeModel(QObject* parent = nullptr);

	void setComparison(std::shared_ptr<ProfileComparison>);
	void refreshNames(bool shortNames);

	QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
	QModelIndex parent(const QModelIndex& index) const override;
	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation, int role = Qt::DisplayRole) const override;
};

}
