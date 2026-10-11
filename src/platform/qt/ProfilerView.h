/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QWidget>

#include <memory>

#include "ProfileCompare.h"
#include "ProfileModels.h"
#include "ProfilerController.h"

class QAction;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTabWidget;
class QTableView;
class QTreeView;
class QTreeWidget;

namespace QGBA {

class CoreController;

// Tools > Profiler: records the running game, and shows recordings and saved profiles. Profiles
// can be opened and compared with no game running.
class ProfilerView : public QWidget {
Q_OBJECT

public:
	ProfilerView(QWidget* parent = nullptr);

public slots:
	void setController(std::shared_ptr<CoreController> controller);

	void start();
	void stop();
	void reset();
	void loadSymbols();
	void openProfile();
	void saveProfile();
	void exportCallgrind();
	void compareWithFile();
	void compareWithPrevious();
	void stopComparing();

private slots:
	void recordingChanged(bool recording);
	void liveUpdated(const ProfilerController::LiveSummary&);
	void setProfile(std::shared_ptr<ProfileData>);
	void setShortNames(bool shortNames);
	void showFunction(uint32_t function);
	void showInFunctionList(uint32_t function);
	void showInCallTree(uint32_t function);

private:
	QWidget* buildCallTree();
	QWidget* buildFunctionList();
	QWidget* buildDetails();
	QWidget* buildComparison();
	QTableView* buildTable(QAbstractItemModel* model);
	void updateControls();
	void updateSummary();
	void updateComparison();
	ProfileInstructionModel::CodeReader codeReader() const;
	uint32_t functionAt(const QModelIndex&) const;

	std::shared_ptr<CoreController> m_coreController;
	std::weak_ptr<ProfilerController> m_profiler;
	std::shared_ptr<ProfileData> m_data;
	// The profile shown before this one, and what this one is being compared with
	std::shared_ptr<ProfileData> m_previous;
	std::shared_ptr<ProfileData> m_baseline;
	uint32_t m_function = mPROFILE_NONE;

	QPushButton* m_start;
	QPushButton* m_stop;
	QPushButton* m_reset;
	QPushButton* m_symbols;
	QPushButton* m_save;
	QPushButton* m_export;
	QPushButton* m_compare;
	QAction* m_compareFile;
	QAction* m_comparePrevious;
	QAction* m_compareStop;
	QCheckBox* m_shortNames;
	QLabel* m_summary;
	QTreeWidget* m_live;
	QTabWidget* m_tabs;
	QTreeView* m_tree;
	QTableView* m_functions;
	QLineEdit* m_search;
	QLabel* m_functionTitle;
	QLabel* m_disassemblyNote;
	QTabWidget* m_detailTabs;

	ProfileTreeModel* m_treeModel;
	ProfileFunctionModel* m_functionModel;
	QSortFilterProxyModel* m_functionProxy;
	ProfileEdgeModel* m_callerModel;
	ProfileEdgeModel* m_calleeModel;
	ProfileInstructionModel* m_instructionModel;

	QWidget* m_comparePage;
	QLabel* m_compareSummary;
	ProfileCompareFunctionModel* m_compareFunctionModel;
	ProfileCompareTreeModel* m_compareTreeModel;
	QTreeView* m_compareTree;
};

}
