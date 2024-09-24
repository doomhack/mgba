#pragma once

#include "ui_ProfilerView.h"

namespace QGBA {

class ProfilerView : public QMainWindow {
	Q_OBJECT

public:
	ProfilerView(QWidget* parent = nullptr);

private:
	Ui::ProfilerView m_ui;
};
}