/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ProfilerView.h"
#include "moc_ProfilerView.cpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "CoreController.h"
#include "GBAApp.h"

#include <mgba/core/core.h>

using namespace QGBA;

static const int TAB_CALL_TREE = 0;
static const int TAB_FUNCTIONS = 1;

ProfilerView::ProfilerView(QWidget* parent)
	: QWidget(parent)
{
	setWindowTitle(tr("Profiler"));
	m_treeModel = new ProfileTreeModel(this);
	m_functionModel = new ProfileFunctionModel(this);
	m_callerModel = new ProfileEdgeModel(true, this);
	m_calleeModel = new ProfileEdgeModel(false, this);
	m_instructionModel = new ProfileInstructionModel(this);
	m_compareFunctionModel = new ProfileCompareFunctionModel(this);
	m_compareTreeModel = new ProfileCompareTreeModel(this);
	QVBoxLayout* layout = new QVBoxLayout(this);

	QHBoxLayout* buttons = new QHBoxLayout;
	m_start = new QPushButton(tr("Start"));
	m_stop = new QPushButton(tr("Stop"));
	m_reset = new QPushButton(tr("Reset"));
	m_symbols = new QPushButton(tr("Symbols..."));
	QPushButton* open = new QPushButton(tr("Open..."));
	m_save = new QPushButton(tr("Save..."));
	m_export = new QPushButton(tr("Export callgrind..."));
	m_compare = new QPushButton(tr("Compare"));
	QMenu* compareMenu = new QMenu(m_compare);
	m_compareFile = compareMenu->addAction(tr("With a saved profile..."), this, &ProfilerView::compareWithFile);
	m_comparePrevious = compareMenu->addAction(tr("With the previous profile"), this, &ProfilerView::compareWithPrevious);
	compareMenu->addSeparator();
	m_compareStop = compareMenu->addAction(tr("Stop comparing"), this, &ProfilerView::stopComparing);
	m_compare->setMenu(compareMenu);
	m_compare->setToolTip(tr("Compare this profile with an earlier one. Later recordings are compared with it too."));
	m_shortNames = new QCheckBox(tr("Short names"));
	m_shortNames->setChecked(true);
	m_shortNames->setToolTip(tr("Leave out C++ template arguments, parameters and return types"));
	m_reset->setToolTip(tr("Discard what has been recorded so far and carry on recording"));
	m_symbols->setToolTip(tr("Load function names from the game's ELF file"));
	m_export->setToolTip(tr("Export for KCachegrind or QCachegrind"));
	buttons->addWidget(m_start);
	buttons->addWidget(m_stop);
	buttons->addWidget(m_reset);
	buttons->addSpacing(12);
	buttons->addWidget(m_symbols);
	buttons->addSpacing(12);
	buttons->addWidget(open);
	buttons->addWidget(m_save);
	buttons->addWidget(m_export);
	buttons->addSpacing(12);
	buttons->addWidget(m_compare);
	buttons->addStretch();
	buttons->addWidget(m_shortNames);
	layout->addLayout(buttons);

	connect(m_start, &QAbstractButton::clicked, this, &ProfilerView::start);
	connect(m_stop, &QAbstractButton::clicked, this, &ProfilerView::stop);
	connect(m_reset, &QAbstractButton::clicked, this, &ProfilerView::reset);
	connect(m_symbols, &QAbstractButton::clicked, this, &ProfilerView::loadSymbols);
	connect(open, &QAbstractButton::clicked, this, &ProfilerView::openProfile);
	connect(m_save, &QAbstractButton::clicked, this, &ProfilerView::saveProfile);
	connect(m_export, &QAbstractButton::clicked, this, &ProfilerView::exportCallgrind);
	connect(m_shortNames, &QAbstractButton::toggled, this, &ProfilerView::setShortNames);

	m_summary = new QLabel;
	m_summary->setWordWrap(true);
	m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_summary);

	m_live = new QTreeWidget;
	m_live->setHeaderLabels({ tr("Function"), tr("Self %") });
	m_live->setRootIsDecorated(false);
	m_live->setMaximumHeight(220);
	m_live->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_live->header()->setStretchLastSection(false);
	m_live->hide();
	layout->addWidget(m_live);

	QSplitter* splitter = new QSplitter(Qt::Vertical);
	m_tabs = new QTabWidget;
	m_tabs->addTab(buildCallTree(), tr("Call tree"));
	m_tabs->addTab(buildFunctionList(), tr("Functions"));
	// Added as a tab only while comparing
	m_comparePage = buildComparison();
	m_comparePage->setParent(this);
	m_comparePage->hide();
	splitter->addWidget(m_tabs);
	splitter->addWidget(buildDetails());
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	layout->addWidget(splitter, 1);

	resize(1000, 760);
	updateControls();
	updateSummary();
}

QTableView* ProfilerView::buildTable(QAbstractItemModel* model) {
	QTableView* table = new QTableView;
	table->setModel(model);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setWordWrap(false);
	table->verticalHeader()->hide();
	table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 4);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	return table;
}

QWidget* ProfilerView::buildCallTree() {
	m_tree = new QTreeView;
	m_tree->setModel(m_treeModel);
	m_tree->setUniformRowHeights(true);
	m_tree->setAllColumnsShowFocus(true);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(ProfileTreeModel::NAME, QHeaderView::Stretch);
	connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection& selected) {
		if (!selected.indexes().isEmpty()) {
			showFunction(functionAt(selected.indexes().first()));
		}
	});
	connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
		uint32_t function = functionAt(m_tree->indexAt(point));
		if (function == mPROFILE_NONE) {
			return;
		}
		QMenu menu;
		menu.addAction(tr("Show in function list"), this, [this, function]() {
			showInFunctionList(function);
		});
		menu.exec(m_tree->viewport()->mapToGlobal(point));
	});
	return m_tree;
}

QWidget* ProfilerView::buildFunctionList() {
	QWidget* page = new QWidget;
	QVBoxLayout* layout = new QVBoxLayout(page);
	layout->setContentsMargins(0, 0, 0, 0);
	m_search = new QLineEdit;
	m_search->setPlaceholderText(tr("Search functions"));
	m_search->setAccessibleName(tr("Search functions"));
	m_search->setClearButtonEnabled(true);
	layout->addWidget(m_search);

	m_functionProxy = new QSortFilterProxyModel(this);
	m_functionProxy->setSourceModel(m_functionModel);
	m_functionProxy->setSortRole(ProfileRole::SORT);
	m_functionProxy->setFilterRole(ProfileRole::SEARCH);
	m_functionProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
	connect(m_search, &QLineEdit::textChanged, m_functionProxy, &QSortFilterProxyModel::setFilterFixedString);

	m_functions = buildTable(m_functionProxy);
	m_functions->setSortingEnabled(true);
	m_functions->sortByColumn(ProfileFunctionModel::SELF_CYCLES, Qt::DescendingOrder);
	m_functions->setContextMenuPolicy(Qt::CustomContextMenu);
	layout->addWidget(m_functions);

	connect(m_functions->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection& selected) {
		if (!selected.indexes().isEmpty()) {
			showFunction(functionAt(selected.indexes().first()));
		}
	});
	connect(m_functions, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
		showInCallTree(functionAt(index));
	});
	connect(m_functions, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
		uint32_t function = functionAt(m_functions->indexAt(point));
		if (function == mPROFILE_NONE) {
			return;
		}
		QMenu menu;
		menu.addAction(tr("Show in call tree"), this, [this, function]() {
			showInCallTree(function);
		});
		menu.exec(m_functions->viewport()->mapToGlobal(point));
	});
	return page;
}

QWidget* ProfilerView::buildDetails() {
	QWidget* page = new QWidget;
	QVBoxLayout* layout = new QVBoxLayout(page);
	layout->setContentsMargins(0, 0, 0, 0);
	m_functionTitle = new QLabel(tr("Select a function to see its callers, callees and instructions."));
	m_functionTitle->setWordWrap(true);
	m_functionTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_functionTitle);

	m_detailTabs = new QTabWidget;
	for (ProfileEdgeModel* model : { m_callerModel, m_calleeModel }) {
		QSortFilterProxyModel* proxy = new QSortFilterProxyModel(this);
		proxy->setSourceModel(model);
		proxy->setSortRole(ProfileRole::SORT);
		QTableView* table = buildTable(proxy);
		table->setSortingEnabled(true);
		table->sortByColumn(ProfileEdgeModel::INCLUSIVE_CYCLES, Qt::DescendingOrder);
		connect(table, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
			uint32_t function = functionAt(index);
			if (function != mPROFILE_NONE) {
				showInFunctionList(function);
			}
		});
		table->setToolTip(tr("Double-click to go to that function"));
		m_detailTabs->addTab(table, model == m_callerModel ? tr("Callers") : tr("Callees"));
	}

	QWidget* instructions = new QWidget;
	QVBoxLayout* instructionLayout = new QVBoxLayout(instructions);
	instructionLayout->setContentsMargins(0, 0, 0, 0);
	m_disassemblyNote = new QLabel(tr("To see the instructions, load the game this profile was recorded from."));
	m_disassemblyNote->hide();
	instructionLayout->addWidget(m_disassemblyNote);
	QTableView* table = buildTable(m_instructionModel);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(ProfileInstructionModel::INSTRUCTION, QHeaderView::Stretch);
	instructionLayout->addWidget(table);
	m_detailTabs->addTab(instructions, tr("Instructions"));
	layout->addWidget(m_detailTabs);
	return page;
}

QWidget* ProfilerView::buildComparison() {
	QWidget* page = new QWidget;
	QVBoxLayout* layout = new QVBoxLayout(page);
	layout->setContentsMargins(0, 0, 0, 0);
	m_compareSummary = new QLabel;
	m_compareSummary->setWordWrap(true);
	m_compareSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_compareSummary);

	QTabWidget* tabs = new QTabWidget;
	QSortFilterProxyModel* proxy = new QSortFilterProxyModel(this);
	proxy->setSourceModel(m_compareFunctionModel);
	proxy->setSortRole(ProfileRole::SORT);
	QTableView* functions = buildTable(proxy);
	// There are many columns, so the names get a width of their own rather than what's left over
	functions->horizontalHeader()->setSectionResizeMode(ProfileCompareFunctionModel::NAME, QHeaderView::Interactive);
	functions->horizontalHeader()->resizeSection(ProfileCompareFunctionModel::NAME, 260);
	functions->setSortingEnabled(true);
	functions->sortByColumn(ProfileCompareFunctionModel::SELF_CURRENT, Qt::DescendingOrder);
	functions->setToolTip(tr("Sort by a Change column to find what got faster or slower"));
	tabs->addTab(functions, tr("Functions"));

	m_compareTree = new QTreeView;
	m_compareTree->setModel(m_compareTreeModel);
	m_compareTree->setUniformRowHeights(true);
	m_compareTree->setAllColumnsShowFocus(true);
	m_compareTree->header()->setStretchLastSection(false);
	m_compareTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_compareTree->header()->setSectionResizeMode(ProfileCompareTreeModel::NAME, QHeaderView::Stretch);
	tabs->addTab(m_compareTree, tr("Call tree"));
	layout->addWidget(tabs);
	return page;
}

uint32_t ProfilerView::functionAt(const QModelIndex& index) const {
	if (!index.isValid()) {
		return mPROFILE_NONE;
	}
	QVariant function = index.data(ProfileRole::FUNCTION);
	return function.isValid() ? function.toUInt() : mPROFILE_NONE;
}

void ProfilerView::setController(std::shared_ptr<CoreController> controller) {
	if (std::shared_ptr<ProfilerController> old = m_profiler.lock()) {
		disconnect(old.get(), nullptr, this, nullptr);
	}
	if (m_coreController) {
		disconnect(m_coreController.get(), nullptr, this, nullptr);
	}
	m_coreController = controller;
	m_profiler.reset();
	if (controller) {
		std::shared_ptr<ProfilerController> profiler = controller->profilerController().lock();
		m_profiler = profiler;
		connect(profiler.get(), &ProfilerController::recordingChanged, this, &ProfilerView::recordingChanged);
		connect(profiler.get(), &ProfilerController::liveUpdated, this, &ProfilerView::liveUpdated);
		connect(profiler.get(), &ProfilerController::finished, this, &ProfilerView::setProfile);
		connect(profiler.get(), &ProfilerController::symbolsChanged, this, &ProfilerView::updateSummary);
		// The game's profiler goes when the game does, but the last recording stays
		connect(controller.get(), &CoreController::stopping, this, [this]() {
			setController(nullptr);
		});
	}
	updateControls();
	updateSummary();
	if (m_function != mPROFILE_NONE) {
		// Whether the instructions can be shown depends on the game
		showFunction(m_function);
	}
}

void ProfilerView::start() {
	std::shared_ptr<ProfilerController> profiler = m_profiler.lock();
	if (!profiler) {
		return;
	}
	if (!profiler->start()) {
		QMessageBox::warning(this, tr("Profiler"),
		                     tr("The profiler couldn't start. If the debugger console is profiling, stop it there first."));
	}
}

void ProfilerView::stop() {
	if (std::shared_ptr<ProfilerController> profiler = m_profiler.lock()) {
		profiler->stop();
	}
}

void ProfilerView::reset() {
	if (std::shared_ptr<ProfilerController> profiler = m_profiler.lock()) {
		profiler->reset();
		m_live->clear();
	}
}

void ProfilerView::loadSymbols() {
	std::shared_ptr<ProfilerController> profiler = m_profiler.lock();
	if (!profiler) {
		return;
	}
	QString path = GBAApp::app()->getOpenFileName(this, tr("Load symbols"), tr("ELF files (*.elf);;All files (*)"));
	if (path.isEmpty()) {
		return;
	}
	if (!profiler->loadSymbols(path)) {
		QMessageBox::warning(this, tr("Profiler"), tr("%1 isn't a 32-bit ELF file with a symbol table.").arg(path));
	}
	updateSummary();
}

void ProfilerView::openProfile() {
	QString path = GBAApp::app()->getOpenFileName(this, tr("Open profile"), tr("Profiles (*.mprof);;All files (*)"));
	if (path.isEmpty()) {
		return;
	}
	std::shared_ptr<ProfileData> data = ProfileData::open(path);
	if (!data) {
		QMessageBox::warning(this, tr("Profiler"), tr("%1 isn't a profile this version of mGBA can read.").arg(path));
		return;
	}
	setProfile(data);
}

void ProfilerView::saveProfile() {
	if (!m_data) {
		return;
	}
	QString path = GBAApp::app()->getSaveFileName(this, tr("Save profile"), tr("Profiles (*.mprof)"));
	if (path.isEmpty()) {
		return;
	}
	if (!m_data->save(path)) {
		QMessageBox::warning(this, tr("Profiler"), tr("Couldn't write %1.").arg(path));
	}
}

void ProfilerView::exportCallgrind() {
	if (!m_data) {
		return;
	}
	QString path = GBAApp::app()->getSaveFileName(this, tr("Export callgrind file"),
	                                              tr("Callgrind files (callgrind.out.* *.callgrind);;All files (*)"));
	if (path.isEmpty()) {
		return;
	}
	if (!m_data->exportCallgrind(path)) {
		QMessageBox::warning(this, tr("Profiler"), tr("Couldn't write %1.").arg(path));
	}
}

void ProfilerView::compareWithFile() {
	QString path = GBAApp::app()->getOpenFileName(this, tr("Compare with profile"), tr("Profiles (*.mprof);;All files (*)"));
	if (path.isEmpty()) {
		return;
	}
	std::shared_ptr<ProfileData> data = ProfileData::open(path);
	if (!data) {
		QMessageBox::warning(this, tr("Profiler"), tr("%1 isn't a profile this version of mGBA can read.").arg(path));
		return;
	}
	m_baseline = data;
	updateComparison();
	m_tabs->setCurrentWidget(m_comparePage);
}

void ProfilerView::compareWithPrevious() {
	if (!m_previous) {
		return;
	}
	m_baseline = m_previous;
	updateComparison();
	m_tabs->setCurrentWidget(m_comparePage);
}

void ProfilerView::stopComparing() {
	m_baseline.reset();
	updateComparison();
}

void ProfilerView::updateComparison() {
	int tab = m_tabs->indexOf(m_comparePage);
	if (!m_baseline || !m_data) {
		if (tab >= 0) {
			m_tabs->removeTab(tab);
		}
		m_comparePage->hide();
		m_compareFunctionModel->setComparison(nullptr);
		m_compareTreeModel->setComparison(nullptr);
		updateControls();
		return;
	}
	std::shared_ptr<ProfileComparison> comparison = std::make_shared<ProfileComparison>(m_baseline, m_data);
	m_compareFunctionModel->setComparison(comparison);
	m_compareTreeModel->setComparison(comparison);
	if (m_compareTreeModel->rowCount()) {
		m_compareTree->expand(m_compareTreeModel->index(0, 0));
	}
	if (tab < 0) {
		m_tabs->addTab(m_comparePage, tr("Compare"));
	}

	auto describe = [](const ProfileData& data) {
		mProfileFrameSummary frames = data.frames();
		QString text = data.source();
		if (frames.count) {
			text += tr(": %1% of frame time used on average, %2% at most")
			            .arg(frames.busyCycles * 100.0 / frames.cycles, 0, 'f', 1)
			            .arg(frames.worstBusyCycles * 100.0 / frames.worstCycles, 0, 'f', 1);
		}
		return text;
	};
	QStringList lines;
	lines << tr("Baseline: %1").arg(describe(*m_baseline));
	lines << tr("Current: %1").arg(describe(*m_data));
	if (comparison->perFrame()) {
		lines << tr("Costs are cycles per frame. Recordings aren't reproducible, so some difference between them is normal.");
	} else {
		lines << tr("Costs are totals for each recording, since one of them has no frames recorded.");
	}
	if (m_baseline->profile()->romCrc32 != m_data->profile()->romCrc32) {
		lines << tr("These were recorded from different ROMs.");
	}
	m_compareSummary->setText(lines.join("\n"));
	updateControls();
}

void ProfilerView::recordingChanged(bool recording) {
	if (recording) {
		m_live->clear();
	}
	updateControls();
	updateSummary();
}

void ProfilerView::liveUpdated(const ProfilerController::LiveSummary& summary) {
	m_live->clear();
	for (const auto& function : summary.top) {
		QTreeWidgetItem* item = new QTreeWidgetItem({
			m_shortNames->isChecked() ? shortFunctionName(function.name) : function.name,
			QString("%1%").arg(summary.cycles ? function.selfCycles * 100.0 / summary.cycles : 0, 0, 'f', 2),
		});
		item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
		item->setToolTip(0, function.name);
		m_live->addTopLevelItem(item);
	}
	QString status = tr("Recording: %1 cycles so far").arg(QLocale().toString(static_cast<qulonglong>(summary.cycles)));
	if (summary.frames) {
		status += tr(", %1 frames, %2% of frame time used").arg(summary.frames).arg(summary.frameBusy, 0, 'f', 1);
	}
	m_summary->setText(status + tr(". Stop to see the full results."));
}

void ProfilerView::setProfile(std::shared_ptr<ProfileData> data) {
	if (!data) {
		return;
	}
	if (m_data && m_data != data) {
		m_previous = m_data;
	}
	m_data = data;
	m_function = mPROFILE_NONE;
	m_treeModel->setProfile(data);
	m_functionModel->setProfile(data);
	m_callerModel->setFunction(nullptr, mPROFILE_NONE);
	m_calleeModel->setFunction(nullptr, mPROFILE_NONE);
	m_instructionModel->setFunction(nullptr, mPROFILE_NONE, {});
	m_functionTitle->setText(tr("Select a function to see its callers, callees and instructions."));
	m_disassemblyNote->hide();
	// Open the main code down to the first function that matters
	QModelIndex index = m_treeModel->index(0, 0);
	while (index.isValid()) {
		m_tree->expand(index);
		index = m_treeModel->index(0, 0, index);
		if (m_treeModel->rowCount(index) != 1) {
			break;
		}
	}
	updateComparison();
	updateControls();
	updateSummary();
}

void ProfilerView::setShortNames(bool shortNames) {
	m_treeModel->refreshNames(shortNames);
	m_functionModel->refreshNames(shortNames);
	m_callerModel->refreshNames(shortNames);
	m_calleeModel->refreshNames(shortNames);
	m_compareFunctionModel->refreshNames(shortNames);
	m_compareTreeModel->refreshNames(shortNames);
}

void ProfilerView::showFunction(uint32_t function) {
	if (!m_data || function == mPROFILE_NONE || function >= m_data->functionCount()) {
		return;
	}
	m_function = function;
	const mProfileFunction* entry = m_data->function(function);
	QLocale locale;
	QString text = QString("<b>%1</b>").arg(m_data->name(function).toHtmlEscaped());
	if (!entry->pseudo && m_data->rawName(function) != m_data->name(function)) {
		text += QString("<br>%1").arg(m_data->rawName(function).toHtmlEscaped());
	}
	text += "<br>" + tr("Self %1 cycles (%2%), inclusive %3 cycles (%4%), %5 calls")
	                     .arg(locale.toString(static_cast<qulonglong>(entry->selfCycles)))
	                     .arg(m_data->percent(entry->selfCycles), 0, 'f', 2)
	                     .arg(locale.toString(static_cast<qulonglong>(entry->inclusiveCycles)))
	                     .arg(m_data->percent(entry->inclusiveCycles), 0, 'f', 2)
	                     .arg(locale.toString(static_cast<qulonglong>(entry->calls)));
	if (entry->recursiveCalls) {
		text += tr(" (+%1 recursive)").arg(locale.toString(static_cast<qulonglong>(entry->recursiveCalls)));
	}
	if (!entry->pseudo) {
		text += tr(", at 0x%1").arg(entry->start, 8, 16, QChar('0'));
	}
	m_functionTitle->setText(text);

	m_callerModel->setFunction(m_data, function);
	m_calleeModel->setFunction(m_data, function);
	ProfileInstructionModel::CodeReader reader = codeReader();
	m_instructionModel->setFunction(m_data, function, reader);
	m_disassemblyNote->setVisible(!reader && m_instructionModel->rowCount());
}

void ProfilerView::showInFunctionList(uint32_t function) {
	int row = m_functionModel->rowOf(function);
	if (row < 0) {
		return;
	}
	QModelIndex index = m_functionProxy->mapFromSource(m_functionModel->index(row, 0));
	if (!index.isValid()) {
		// Hidden by the search
		m_search->clear();
		index = m_functionProxy->mapFromSource(m_functionModel->index(row, 0));
	}
	m_tabs->setCurrentIndex(TAB_FUNCTIONS);
	m_functions->setCurrentIndex(index);
	m_functions->scrollTo(index);
}

void ProfilerView::showInCallTree(uint32_t function) {
	if (!m_data || function == mPROFILE_NONE) {
		return;
	}
	// Where it costs the most
	for (uint32_t node : m_data->nodesOf(function)) {
		QModelIndex index = m_treeModel->indexOfNode(node);
		if (!index.isValid()) {
			continue;
		}
		for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
			m_tree->expand(parent);
		}
		m_tabs->setCurrentIndex(TAB_CALL_TREE);
		m_tree->setCurrentIndex(index);
		m_tree->scrollTo(index);
		return;
	}
}

ProfileInstructionModel::CodeReader ProfilerView::codeReader() const {
	std::shared_ptr<ProfilerController> profiler = m_profiler.lock();
	if (!m_coreController || !profiler || !m_data || profiler->romCrc32() != m_data->profile()->romCrc32) {
		return {};
	}
	std::weak_ptr<CoreController> weak = m_coreController;
	return [weak](uint32_t address, unsigned width, uint32_t* value) {
		std::shared_ptr<CoreController> controller = weak.lock();
		if (!controller) {
			return false;
		}
		mCore* core = controller->thread()->core;
		*value = width == 2 ? core->rawRead16(core, address, -1) : core->rawRead32(core, address, -1);
		return true;
	};
}

void ProfilerView::updateControls() {
	std::shared_ptr<ProfilerController> profiler = m_profiler.lock();
	bool recording = profiler && profiler->isRecording();
	m_start->setEnabled(profiler && !recording);
	m_stop->setEnabled(recording);
	m_reset->setEnabled(recording);
	m_symbols->setEnabled(profiler && !recording);
	m_save->setEnabled(static_cast<bool>(m_data));
	m_export->setEnabled(static_cast<bool>(m_data));
	m_compare->setEnabled(static_cast<bool>(m_data));
	m_comparePrevious->setEnabled(static_cast<bool>(m_previous));
	m_compareStop->setEnabled(static_cast<bool>(m_baseline));
	m_live->setVisible(recording);
}

void ProfilerView::updateSummary() {
	std::shared_ptr<ProfilerController> profiler = m_profiler.lock();
	QStringList lines;
	if (profiler && profiler->isRecording()) {
		lines << tr("Recording. The functions using the most time so far are shown below, and the full results when you stop.");
	} else if (profiler) {
		if (profiler->symbolCount()) {
			lines << tr("Symbols: %1 functions from %2").arg(profiler->symbolCount()).arg(profiler->symbolSource());
		} else {
			lines << tr("No symbols loaded: time will be shown by memory region. Use Symbols... to load the game's ELF file.");
		}
	} else if (!m_data) {
		lines << tr("Load a game to record a profile, or open a saved one.");
	}

	if (m_data) {
		const mProfile* profile = m_data->profile();
		QLocale locale;
		lines << tr("Showing: %1").arg(m_data->source());
		QString totals = tr("%1 cycles over %2 frames").arg(locale.toString(static_cast<qulonglong>(profile->totalCycles))).arg(profile->frames);
		if (profile->totalCycles != profile->elapsedCycles) {
			totals += tr(" (%1 elapsed)").arg(locale.toString(static_cast<qulonglong>(profile->elapsedCycles)));
		}
		if (!profile->symbolSource) {
			totals += tr(", no symbols");
		}
		lines << totals;
		mProfileFrameSummary frames = m_data->frames();
		if (frames.count) {
			lines << tr("Frame time used: %1% on average (%2 of %3 cycles), %4% in the busiest frame (frame %5)")
			             .arg(frames.busyCycles * 100.0 / frames.cycles, 0, 'f', 1)
			             .arg(locale.toString(static_cast<qulonglong>(frames.busyCycles / frames.count)))
			             .arg(locale.toString(static_cast<qulonglong>(frames.cycles / frames.count)))
			             .arg(frames.worstBusyCycles * 100.0 / frames.worstCycles, 0, 'f', 1)
			             .arg(frames.worst + 1);
		}
		if (profile->idleOptimization && strcmp(profile->idleOptimization, "ignore") != 0) {
			lines << tr("Idle-loop optimization was on, so busy-wait loops mGBA recognized count as idle time.");
		}
	}
	m_summary->setText(lines.join("\n"));
}
