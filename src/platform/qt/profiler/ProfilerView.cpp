#include "profiler/ProfilerView.h"

using namespace QGBA;

ProfilerView::ProfilerView(QWidget* parent)
    : QMainWindow(parent)
{
	m_ui.setupUi(this);

	setupViews();
}

void ProfilerView::setupViews() {
	const callTreeNode* callTree = GetCallTree();

	QTreeWidgetItem* rootNode = buildCallTreeViewRecursive(callTree, nullptr);

	this->m_ui.treeWidget->insertTopLevelItem(0, rootNode);

	std::map<functionEntry*, functionStats*> functionListItems = GetFunctionCounts();

	for (const auto& pair : functionListItems)
	{
		double inclusiveTimeMs = pair.second->cycles / (double) cpuToMs;

		QString functionName = (pair.first) ? pair.first->name.c_str() : "<unknown>";

		QTreeWidgetItem* item =
		    new QTreeWidgetItem(this->m_ui.listWidget, 
			QStringList() << 
			functionName << 
			QString::number(pair.second->callCount) << 
			QString::number(inclusiveTimeMs, 'f', 3));
	}

	connect(this->m_ui.treeWidget, &QTreeWidget::itemClicked, this, &ProfilerView::onFunctionSelected);
	connect(this->m_ui.listWidget, &QTreeWidget::itemClicked, this, &ProfilerView::onFunctionSelected);
}

QTreeWidgetItem* ProfilerView::buildCallTreeViewRecursive(const callTreeNode* node, QTreeWidgetItem* parent)
{
	QString functionName = (node->function) ? node->function->name.c_str() : "<unknown>";

	double timeMs = node->cycleCount / (double)cpuToMs;
	double inclusiveTimeMs = node->inclusiveCycleCount / (double) cpuToMs;

	QTreeWidgetItem* item = new QTreeWidgetItem(parent,
	                        QStringList() << functionName << QString::number(node->callCount) << QString::number(inclusiveTimeMs, 'f', 3) << QString::number(timeMs, 'f', 3));

    for (auto it = node->childNodes.begin(); it != node->childNodes.end(); ++it)
	{
		buildCallTreeViewRecursive(it->second, item);
	}

	return item;
}

void ProfilerView::onFunctionSelected(QTreeWidgetItem* item, int column)
{
	this->m_ui.instructionCounts->clear();

	QString functionName = item->text(0);

	std::map<void*, uint64_t> instructionCounts = GetInstructionsForFunction(functionName.toLatin1().constData());

	for (const auto& pair : instructionCounts) {
		double inclusiveTimeMs = pair.second / (double) cpuToMs;

		QString hexStr = QString::asprintf("0x%08X", pair.first);

		QTreeWidgetItem* item = new QTreeWidgetItem(
		    this->m_ui.instructionCounts, QStringList() << hexStr << QString::number(inclusiveTimeMs, 'f', 3));
	}
}
