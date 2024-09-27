#pragma once

 #include <QTreeWidget>

#include <mgba/profiler/collector.h>

#include "ui_ProfilerView.h"

namespace QGBA {

class ProfilerView : public QMainWindow {
	Q_OBJECT

public:
	ProfilerView(QWidget* parent = nullptr);

private:
	void setupViews();

	QTreeWidgetItem* buildCallTreeViewRecursive(const callTreeNode* node, QTreeWidgetItem* parent);

	Ui::ProfilerView m_ui;
};
}