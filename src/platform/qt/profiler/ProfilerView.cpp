#include "profiler/ProfilerView.h"

using namespace QGBA;

ProfilerView::ProfilerView(QWidget* parent)
    : QMainWindow(parent)
{
	m_ui.setupUi(this);

	setupViews();
}

void ProfilerView::setupViews()
{
	const callTreeNode* callTree = GetCallTree();

	QTreeWidgetItem* rootNode = buildCallTreeViewRecursive(callTree, nullptr);

	this->m_ui.treeWidget->insertTopLevelItem(0, rootNode);
}

QTreeWidgetItem* ProfilerView::buildCallTreeViewRecursive(const callTreeNode* node, QTreeWidgetItem* parent)
{
	QString functionName = (node->function) ? node->function->name.c_str() : "<unknown>";

	QTreeWidgetItem* item = new QTreeWidgetItem(parent, QStringList() << functionName << QString::number(node->callCount) << QString::number(node->cycleCount));

    for (auto it = node->childNodes.begin(); it != node->childNodes.end(); ++it)
	{
		buildCallTreeViewRecursive(it->second, item);
	}

	return item;
}
